#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/metal/backend.hpp>
#include "forward_output_fixture.hpp"
#include "surface_geometry_fixture.hpp"
#include <merlin/render/gpu_scene_packing.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using merlin::render::RenderResult;

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error("Metal Forward lighting: " + message);
  }
}

bool Same(const RenderResult& a, const RenderResult& b) {
  return a.color.pixels == b.color.pixels && a.depth.pixels == b.depth.pixels &&
         a.prim_id.pixels == b.prim_id.pixels &&
         a.instance_id.pixels == b.instance_id.pixels;
}

RenderResult Render(merlin::render::Backend& backend,
    std::shared_ptr<const merlin::extraction::FrameSnapshot> snapshot,
    merlin::render::OutputTransformSettings output_transform = {}) {
  merlin::render::RenderRequest request;
  request.snapshot = std::move(snapshot);
  request.output_transform = output_transform;
  request.width = request.height = 64;
  request.products = {{merlin::Aov::Color, true}, {merlin::Aov::Depth, true},
      {merlin::Aov::PrimId, true}, {merlin::Aov::InstanceId, true}};
  return backend.Resolve(backend.Submit(request));
}

void RunFixture(merlin::render::Backend& backend, merlin::Vec4 albedo,
    const std::filesystem::path& output) {
  std::filesystem::create_directories(output);
  merlin::RenderWorld world;
  merlin::extraction::SceneExtractor extractor;
  merlin::MeshDescriptor mesh;
  mesh.positions = {{-0.7F, -0.7F, 0.5F}, {0.7F, -0.7F, 0.5F},
      {0.7F, 0.7F, 0.5F}, {-0.7F, 0.7F, 0.5F}};
  mesh.normals.assign(4, {0, 0, 1});
  mesh.indices = {0, 1, 2, 0, 2, 3};
  merlin::MaterialDescriptor material;
  material.features = merlin::MaterialFeature::DirectionalLight;
  material.parameters.base_color = albedo;
  merlin::InstanceDescriptor instance;
  instance.mesh = world.CreateMesh(mesh);
  instance.material = world.CreateMaterial(material);
  const auto instance_handle = world.CreateInstance(instance);
  merlin::CameraDescriptor camera;
  const auto camera_handle = world.CreateCamera(camera);
  extractor.SetActiveCamera(camera_handle);
  merlin::LightDescriptor point;
  point.type = merlin::LightType::Point;
  const auto point_handle = world.CreateLight(point);
  const auto render = [&] {
    extractor.Apply(world, world.Commit());
    return Render(backend, extractor.snapshot());
  };
  // Each frame context lazily creates its AOV/readback targets. Warm the whole
  // ring before distinguishing edit costs from initial target allocations.
  for (std::uint32_t frame = 0;
       frame < backend.capabilities().limits.max_frames_in_flight; ++frame) {
    (void)render();
  }

  merlin::tests::VerifyForwardOutputTransforms(
      [&](merlin::render::OutputTransformSettings settings) {
        return Render(backend, extractor.snapshot(), settings);
      }, albedo);

  std::ofstream report(output / "comparison.json");
  report << "{\n  \"schema\": \"merlin-metal-forward-lighting/v1\",\n"
            "  \"color_aov\": \"linear-rgba8-with-embedded-reinhard\",\n"
            "  \"lighting_profile\": \"forward-sh-l2-reinhard/v1\",\n"
            "  \"off_policy\": \"unit-white-plus-z-diagnostic-fallback\",\n"
            "  \"analytic_tolerance\": 1,\n  \"repeat_count\": 3,\n"
            "  \"phases\": [\n";
  bool first = true;
  // Independent +Z evaluation of the packaged environment, projected from HDR.
  // The CPU gate checks the embedded SH against the source HDR separately.
  // It deliberately does not call ExtractForwardDirectionalLighting.
  const std::array<double, 3> base{albedo.x, albedo.y, albedo.z};
  std::array<double, 3> environment{
      0.947761416 * 0.2820947918 + 0.325469345 * 0.4886025119 -
          0.0461782999 * 0.6307831306,
      1.00647175 * 0.2820947918 + 0.341748059 * 0.4886025119 -
          0.0449634455 * 0.6307831306,
      1.12809479 * 0.2820947918 + 0.369059116 * 0.4886025119 -
          0.0430846624 * 0.6307831306};
  const auto capture = [&](const std::string& phase, double intensity,
                           double cosine = 1.0,
                           merlin::Vec3 color = {1, 1, 1}) {
    const auto result = render();
    Require(result.color.pixels.size() >= 64U * result.color.row_pitch_bytes &&
                result.depth.pixels.size() == 64U * 64U &&
                result.prim_id.pixels.size() == 64U * 64U &&
                result.instance_id.pixels.size() == 64U * 64U,
        phase + " missing AOV readback");
    constexpr auto center = 32U * 64U + 32U;
    Require(result.depth.pixels[center] < 1 &&
                result.prim_id.pixels[center] ==
                    static_cast<std::uint32_t>(instance.mesh.value()) &&
                result.instance_id.pixels[center] ==
                    static_cast<std::uint32_t>(instance_handle.value()),
        phase + " center depth/identity changed: depth=" +
            std::to_string(result.depth.pixels[center]) + " primId=" +
            std::to_string(result.prim_id.pixels[center]) + " instanceId=" +
            std::to_string(result.instance_id.pixels[center]));
    const auto stable_resources = [&](const auto& frame) {
      Require(frame.telemetry.upload_bytes == 0 &&
                  frame.telemetry.pipeline_creation_count == 0 &&
                  frame.telemetry.allocation_count == 0 &&
                  frame.telemetry.draw_count == 1,
          phase + " rebuilt resources or changed draw count");
    };
    stable_resources(result);
    for (int repeat = 0; repeat < 3; ++repeat) {
      const auto repeated = render();
      Require(Same(result, repeated), phase + " repeated frame differs");
      stable_resources(repeated);
    }
    const std::array<double, 3> light_color{color.x, color.y, color.z};
    std::array<int, 3> actual{}, expected{};
    int maximum_error{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
      actual[channel] = result.color.pixels[
          32U * result.color.row_pitch_bytes + 32U * 4U + channel];
      const auto linear = base[channel] *
          (0.15 * environment[channel] + 0.85 * intensity * cosine * light_color[channel]);
      expected[channel] = static_cast<int>(std::lround(255 * std::clamp(
          linear / (1.0 + linear), 0.0, 1.0)));
      maximum_error = std::max(maximum_error,
          std::abs(actual[channel] - expected[channel]));
      Require(maximum_error <= 1, phase + " differs from analytic response");
      // Very dark normal-only SH samples can legitimately quantize to zero.
      Require((expected[channel] == 0 || actual[channel] > 0) && actual[channel] < 255,
          phase + " lost color headroom");
    }
    std::ofstream image(output / (phase + ".ppm"), std::ios::binary);
    image << "P6\n64 64\n255\n";
    for (std::size_t y = 0; y < 64; ++y) {
      for (std::size_t x = 0; x < 64; ++x) {
        image.write(reinterpret_cast<const char*>(result.color.pixels.data() +
                        y * result.color.row_pitch_bytes + x * 4), 3);
      }
    }
    Require(bool(image), phase + " image write failed");
    if (!first)
      report << ",\n";
    first = false;
    report << "    {\"name\": \"" << phase << "\", \"center_rgb\": ["
           << actual[0] << ", " << actual[1] << ", " << actual[2]
           << "], \"expected_rgb\": [" << expected[0] << ", " << expected[1]
           << ", " << expected[2] << "], \"maximum_channel_error\": "
           << maximum_error << ", \"upload_bytes\": " << result.telemetry.upload_bytes
           << ", \"pipeline_creation_count\": " << result.telemetry.pipeline_creation_count
           << ", \"allocation_count\": " << result.telemetry.allocation_count
           << ", \"draw_count\": " << result.telemetry.draw_count << "}";
    return result;
  };
  const auto off = capture("off-static", 1);
  merlin::LightDescriptor light;
  const auto sine = std::sin(0.53 * std::acos(-1.0) / 360.0);
  light.intensity = static_cast<float>(15000 * sine * sine);
  const auto intensity = light.intensity;
  const auto light_handle = world.CreateLight(light);
  const auto on = capture("on-static", intensity);
  constexpr float cosine = 0.8660254F;
  camera.view.values[0] = cosine;
  camera.view.values[2] = 0.5F;
  camera.view.values[8] = -0.5F;
  camera.view.values[10] = cosine;
  camera.view.values[12] = 0.37F;
  camera.view.values[14] = 0.5F * (1 - cosine);
  world.UpdateCamera(camera_handle, camera);
  const auto camera_only = capture("world-light-moving-camera", intensity);
  light.transform.values[8] = 0.5F;
  light.transform.values[10] = cosine;
  world.UpdateLight(light_handle, light, merlin::ChangeAspect::Transform);
  const auto moving = capture("on-moving", intensity, cosine);
  Require(camera_only.depth.pixels == moving.depth.pixels &&
              camera_only.prim_id.pixels == moving.prim_id.pixels &&
              camera_only.instance_id.pixels == moving.instance_id.pixels &&
              camera_only.color.pixels != moving.color.pixels,
      "light rotation did not change only shading");
  world.Remove(light_handle);
  (void)capture("off-moving", 1);
  camera = {};
  world.UpdateCamera(camera_handle, camera);
  Require(Same(off, capture("off-restored", 1)), "OFF return differs");
  light.transform = {};
  const auto restored_handle = world.CreateLight(light);
  Require(Same(on, capture("on-restored", intensity)), "ON return differs");
  light.intensity = 0;
  world.UpdateLight(restored_handle, light, merlin::ChangeAspect::LightParameters);
  (void)capture("zero-energy", 0);

  light.intensity = 0.5F;
  light.color = {0.25F, 0.5F, 0.75F};
  world.UpdateLight(restored_handle, light, merlin::ChangeAspect::LightParameters);
  merlin::LightDescriptor fill;
  fill.intensity = 0.75F;
  fill.color = {0.8F, 0.2F, 0.1F};
  const auto fill_handle = world.CreateLight(fill);
  // Recreated handles carry a generation, so creation order alone does not
  // establish the smallest stable value. Give the actual minimum the key color.
  const auto selected_handle = restored_handle.value() < fill_handle.value()
                                   ? restored_handle : fill_handle;
  const auto remaining_handle = selected_handle == restored_handle
                                    ? fill_handle : restored_handle;
  world.UpdateLight(selected_handle, light, merlin::ChangeAspect::LightParameters);
  world.UpdateLight(remaining_handle, fill, merlin::ChangeAspect::LightParameters);
  const auto selected = capture("stable-selection", light.intensity, 1, light.color);
  const auto retained = extractor.snapshot();
  world.Remove(point_handle);
  Require(Same(selected, capture("compacted-selection", light.intensity, 1, light.color)),
      "table compaction changed illumination");
  Require(extractor.snapshot()->lights.begin()->light == fill_handle.value(),
      "fixture did not move the last light into the first dense slot");
  Require(Same(selected, Render(backend, retained)), "retained snapshot changed");
  light.transform.values[12] = 123;
  light.transform.values[13] = -456;
  world.UpdateLight(selected_handle, light, merlin::ChangeAspect::Transform);
  Require(Same(selected, capture("translated-light", light.intensity, 1, light.color)),
      "translation changed a directional light");
  for (const auto& [name, magnitude] : std::array{
           std::pair{"large-axis", std::numeric_limits<float>::max()},
           std::pair{"small-axis", std::numeric_limits<float>::min()}}) {
    light.transform.values[8] = magnitude;
    light.transform.values[9] = 0;
    light.transform.values[10] = magnitude;
    world.UpdateLight(selected_handle, light, merlin::ChangeAspect::Transform);
    (void)capture(name, light.intensity, 1 / std::sqrt(2.0), light.color);
  }
  for (const auto& [name, axis] : std::array{
           std::pair{"zero-axis", 0.0F},
           std::pair{"infinite-axis", std::numeric_limits<float>::infinity()},
           std::pair{"nan-axis", std::numeric_limits<float>::quiet_NaN()}}) {
    light.transform.values[8] = axis;
    light.transform.values[10] = 0;
    world.UpdateLight(selected_handle, light, merlin::ChangeAspect::Transform);
    Require(Same(selected, capture(name, light.intensity, 1, light.color)),
        std::string(name) + " did not recover to +Z");
  }
  light.transform = {};
  // Submit must reject invalid selected energy before a GPU frame is produced,
  // and a subsequent valid submission must still recover exactly.
  std::size_t rejected_inputs{};
  for (const bool invalid_color : {false, true}) {
    for (const auto value : {-1.0F, std::numeric_limits<float>::infinity(),
             std::numeric_limits<float>::quiet_NaN()}) {
      auto invalid = light;
      if (invalid_color)
        invalid.color.y = value;
      else
        invalid.intensity = value;
      world.UpdateLight(selected_handle, invalid, merlin::ChangeAspect::LightParameters);
      bool rejected{};
      try {
        (void)render();
      } catch (const merlin::render::RendererError& error) {
        rejected = error.code() == merlin::render::RendererErrorCode::InvalidRequest &&
                   error.operation() == "extract Forward directional lighting";
      }
      Require(rejected, "invalid selected energy was not rejected");
      ++rejected_inputs;
    }
  }
  world.UpdateLight(selected_handle, light, merlin::ChangeAspect::LightParameters);
  Require(Same(selected, capture("valid-after-rejection", light.intensity, 1, light.color)),
      "valid frame did not recover after rejected inputs");
  world.Remove(selected_handle);
  (void)capture("selected-removed", fill.intensity, 1, fill.color);
  world.Remove(remaining_handle);
  Require(Same(off, capture("all-lights-removed", 1)), "fallback did not recover");
  // Independent SH samples, same normal-only fixture as Vulkan. The direct
  // +Z diagnostic source is perpendicular to all three normals.
  const std::array<std::array<double, 3>, 3> environment_samples{{
      {0.947761416 * 0.2820947918 + 0.417900145 * 0.4886025119 +
           0.0461782999 * 0.3153915653 + 0.108191431 * 0.5462742153,
       1.00647175 * 0.2820947918 + 0.473879695 * 0.4886025119 +
           0.0449634455 * 0.3153915653 + 0.107941866 * 0.5462742153,
       1.12809479 * 0.2820947918 + 0.577024102 * 0.4886025119 +
           0.0430846624 * 0.3153915653 + 0.10881608 * 0.5462742153},
      {0.947761416 * 0.2820947918 - 0.417900145 * 0.4886025119 +
           0.0461782999 * 0.3153915653 + 0.108191431 * 0.5462742153,
       1.00647175 * 0.2820947918 - 0.473879695 * 0.4886025119 +
           0.0449634455 * 0.3153915653 + 0.107941866 * 0.5462742153,
       1.12809479 * 0.2820947918 - 0.577024102 * 0.4886025119 +
           0.0430846624 * 0.3153915653 + 0.10881608 * 0.5462742153},
      {0.947761416 * 0.2820947918 - 0.294570357 * 0.4886025119 +
           0.0461782999 * 0.3153915653 - 0.108191431 * 0.5462742153,
       1.00647175 * 0.2820947918 - 0.322227687 * 0.4886025119 +
           0.0449634455 * 0.3153915653 - 0.107941866 * 0.5462742153,
       1.12809479 * 0.2820947918 - 0.367371589 * 0.4886025119 +
           0.0430846624 * 0.3153915653 - 0.10881608 * 0.5462742153},
  }};
  std::size_t sample{};
  for (const auto& [phase, normal] : std::array{
           std::pair{"environment-up", merlin::Vec3{0, 1, 0}},
           std::pair{"environment-down", merlin::Vec3{0, -1, 0}},
           std::pair{"environment-side", merlin::Vec3{1, 0, 0}}}) {
    mesh.normals.assign(4, normal);
    world.UpdateMesh(instance.mesh, mesh, merlin::ChangeAspect::Primvars);
    for (std::uint32_t frame = 0;
         frame < backend.capabilities().limits.max_frames_in_flight; ++frame) {
      (void)render();
    }
    environment = environment_samples[sample++];
    (void)capture(phase, 1, 0);
  }
  report << "\n  ],\n  \"rejected_input_count\": " << rejected_inputs
         << ",\n  \"validation_messages\": " << backend.statistics().validation_messages
         << "\n}\n";
  Require(bool(report), "report write failed");
  Require(backend.statistics().validation_messages == 0, "validation messages");
}

} // namespace

// Every Metal Forward entry builds the same surface context. The packed GPU
// Scene path reads its normal matrix from the instance table instead of the
// draw constants, so both paths run the analytic gate. Each fixture case is a
// new extraction source; Metal geometry residency is keyed by handle and
// revision only, so every source gets a fresh backend.
void RunSurfaceFixture(const merlin::render::BackendCreateInfo& info,
    bool gpu_scene) {
  const merlin::render::GpuScenePackingCapacities capacities{1, 1, 1, 1};
  merlin::metal::BackendOptions options;
  if (gpu_scene)
    options.gpu_scene_capacities = capacities;
  const merlin::metal::BackendFactory factory(options);
  std::unique_ptr<merlin::render::Backend> backend;
  std::optional<merlin::render::GpuScenePackingState> packing;
  std::uint64_t source{};
  std::uint64_t validation_messages{};
  const std::vector placements{merlin::render::GpuGeometryPlacement{0, 0}};
  const std::vector identities{merlin::render::GpuInstanceIdentity{17, 23}};
  std::uint64_t completion{};
  const auto report = merlin::tests::VerifySurfaceGeometryViews(
      [&](std::shared_ptr<const merlin::extraction::FrameSnapshot> snapshot,
          merlin::render::DebugView view) {
        if (snapshot->source_id != source) {
          if (backend)
            validation_messages += backend->statistics().validation_messages;
          backend = factory.Create(info);
          packing.emplace(capacities);
          source = snapshot->source_id;
          completion = 0;
        }
        merlin::render::RenderRequest request;
        request.snapshot = snapshot;
        request.debug_view = view;
        request.width = request.height = 64;
        request.products = {{merlin::Aov::Color, true},
            {merlin::Aov::Depth, true}};
        if (gpu_scene) {
          // A fresh backend places the fixture's normal map at the first
          // argument-table texture and sampler slots.
          const auto bindings =
              merlin::tests::SurfaceGeometryMaterialBindings(*snapshot, 0, 0);
          request.gpu_scene_update =
              std::make_shared<merlin::render::GpuScenePackedFrameUpdate>(
                  packing->Apply(*snapshot, completion, completion,
                      {placements, identities, bindings}));
        }
        auto result = backend->Resolve(backend->Submit(request));
        completion = result.completion_value;
        Require(result.telemetry.gpu_scene_draw_count == (gpu_scene ? 1U : 0U),
            "surface fixture changed Forward path");
        return result;
      });
  validation_messages += backend->statistics().validation_messages;
  Require(report.cases == 21 && report.samples > 0,
      "surface fixture skipped cases");
  Require(validation_messages == 0, "surface fixture validation messages");
  std::cout << (gpu_scene ? "gpu-scene" : "conventional")
            << ": surface geometry cases=" << report.cases
            << " samples=" << report.samples
            << " max_channel_error=" << report.maximum_channel_error << '\n';
  merlin::render::RenderRequest invalid;
  invalid.snapshot = std::make_shared<merlin::extraction::FrameSnapshot>();
  invalid.debug_view = merlin::render::DebugView::Depth;
  bool rejected{};
  try {
    (void)backend->Submit(invalid);
  } catch (const merlin::render::RendererError& error) {
    rejected = error.code() == merlin::render::RendererErrorCode::InvalidRequest;
  }
  Require(rejected, "unsupported debug view reached Metal submission");
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: metal_forward_lighting_test IMAGE_DIR\n";
    return 1;
  }
  try {
    merlin::metal::BackendFactory factory;
    const auto availability = factory.availability();
    if (!availability.available) {
      std::cerr << "skip: " << availability.detail << '\n';
      return 77;
    }
    merlin::render::BackendCreateInfo info;
    info.enable_validation = true;
    auto backend = factory.Create(info);
    RunFixture(*backend, {0.4F, 0.1875F, 0.1F, 1},
        std::filesystem::path(argv[1]) / "color");
    RunFixture(*backend, {1, 1, 1, 1}, std::filesystem::path(argv[1]) / "white");
    RunSurfaceFixture(info, false);
    RunSurfaceFixture(info, true);
    std::cout << "Metal Forward directional selection and analytic images verified\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
