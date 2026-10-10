#pragma once

#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/vulkan/renderer.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace merlin::tests {

inline void RequireForwardImage(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error("Forward image fixture: " + message);
  }
}

// Native camera-light input uses the documented Hdx radiance conversion.
// OFF deliberately removes the source: the current no-light diagnostic
// fallback is unit white +Z, whereas an explicit zero source is ambient only.
inline void RunForwardImageFixture(vulkan::Renderer& renderer,
    const vulkan::ShaderPaths& shaders, MaterialDescriptor material,
    const std::filesystem::path& output) {
  std::filesystem::create_directories(output);
  RenderWorld world;
  extraction::SceneExtractor extractor;
  MeshDescriptor mesh;
  mesh.positions = {{-0.7F, -0.7F, 0.5F}, {0.7F, -0.7F, 0.5F},
      {0.7F, 0.7F, 0.5F}, {-0.7F, 0.7F, 0.5F}};
  mesh.normals.assign(4, {0, 0, 1});
  mesh.indices = {0, 1, 2, 0, 2, 3};
  material.features = MaterialFeature::DirectionalLight;
  const auto material_handle = world.CreateMaterial(material);
  InstanceDescriptor instance;
  instance.mesh = world.CreateMesh(mesh);
  instance.material = material_handle;
  world.CreateInstance(instance);
  CameraDescriptor camera;
  const auto camera_handle = world.CreateCamera(camera);
  extractor.SetActiveCamera(camera_handle);
  const auto render = [&] {
    extractor.Apply(world, world.Commit());
    return renderer.Render(*extractor.snapshot(), 64, 64, shaders);
  };
  const auto same = [](const auto& a, const auto& b) {
    return a.color.pixels == b.color.pixels && a.depth.pixels == b.depth.pixels &&
           a.prim_id.pixels == b.prim_id.pixels &&
           a.instance_id.pixels == b.instance_id.pixels;
  };
  const auto center = [](const auto& result, std::size_t channel) {
    return result.color.pixels[32 * result.color.row_pitch_bytes + 32 * 4 + channel];
  };
  // Warm both generated and handwritten variants before measuring edits.
  (void)render();
  const auto generated = material.module.has_value();
  auto builtin = material;
  builtin.module.reset();
  builtin.generated_parameters = {};
  builtin.generated_resources = {};
  world.UpdateMaterial(material_handle, builtin, ChangeAspect::MaterialModule);
  (void)render();
  world.UpdateMaterial(material_handle, material, ChangeAspect::MaterialModule);

  std::ofstream report(output / "comparison.json");
  report << "{\n  \"schema\": \"merlin-forward-image-fixture/v1\",\n"
            "  \"off_policy\": \"unit-white-plus-z-diagnostic-fallback\",\n"
            "  \"color_aov\": \"linear-rgba8-with-embedded-reinhard\",\n"
            "  \"material_parity_tolerance\": 1,\n  \"phases\": [\n";
  bool first = true;
  const auto capture = [&](const char* phase) {
    const auto result = render();
    int material_channel_error{};
    RequireForwardImage(result.depth.pixels[32 * 64 + 32] < 1, "center is uncovered");
    RequireForwardImage(result.counters.upload_bytes == 0 &&
                            result.counters.pipeline_creation_count == 0,
        "edit rebuilt GPU resources");
    RequireForwardImage(result.counters.generated_material_draw_count == (generated ? 1U : 0U) &&
                            result.counters.generated_material_fallback_count == 0,
        "material execution changed");
    for (int repeat = 0; repeat < 3; ++repeat) {
      const auto repeated = render();
      RequireForwardImage(same(result, repeated), std::string(phase) + " flickered");
      RequireForwardImage(repeated.counters.upload_bytes == 0 &&
                              repeated.counters.pipeline_creation_count == 0,
          "static frame rebuilt resources");
    }
    if (generated) {
      world.UpdateMaterial(material_handle, builtin, ChangeAspect::MaterialModule);
      const auto reference = render();
      RequireForwardImage(result.depth.pixels == reference.depth.pixels &&
                              result.prim_id.pixels == reference.prim_id.pixels &&
                              result.instance_id.pixels == reference.instance_id.pixels,
          "material changed IDs/depth");
      for (std::size_t i = 0; i < result.color.pixels.size(); ++i) {
        const auto error = std::abs(int(result.color.pixels[i]) - int(reference.color.pixels[i]));
        material_channel_error = std::max(material_channel_error, error);
        RequireForwardImage(error <= 1,
            "generated/handwritten lighting differs");
      }
      world.UpdateMaterial(material_handle, material, ChangeAspect::MaterialModule);
    }
    for (std::size_t channel = 0; channel < 3; ++channel) {
      RequireForwardImage(center(result, channel) > 0 && center(result, channel) < 255,
          "center lost color headroom");
    }
    std::ofstream image(output / (std::string(phase) + ".ppm"), std::ios::binary);
    image << "P6\n64 64\n255\n";
    for (std::size_t y = 0; y < 64; ++y) {
      for (std::size_t x = 0; x < 64; ++x) {
        image.write(reinterpret_cast<const char*>(result.color.pixels.data() +
                                                  y * result.color.row_pitch_bytes + x * 4),
            3);
      }
    }
    RequireForwardImage(bool(image), "image write failed");
    if (!first)
      report << ",\n";
    first = false;
    report << "    {\"name\": \"" << phase << "\", \"center_rgb\": ["
           << int(center(result, 0)) << ", " << int(center(result, 1)) << ", "
           << int(center(result, 2)) << "], \"upload_bytes\": "
           << result.counters.upload_bytes << ", \"pipeline_creation_count\": "
           << result.counters.pipeline_creation_count
           << ", \"material_maximum_channel_error\": " << material_channel_error << "}";
    return result;
  };
  const auto off = capture("off-static");
  LightDescriptor light;
  const auto sine = std::sin(0.53 * 3.141592653589793 / 360.0);
  light.intensity = static_cast<float>(15000 * sine * sine);
  const auto light_handle = world.CreateLight(light);
  const auto on = capture("on-static");
  // Orthographic camera motion rotates about the patch center and translates
  // in X. The camera light follows orientation, while normals remain world-space.
  constexpr float cosine = 0.8660254F;
  camera.view.values[0] = cosine;
  camera.view.values[2] = 0.5F;
  camera.view.values[8] = -0.5F;
  camera.view.values[10] = cosine;
  camera.view.values[12] = 0.37F;
  camera.view.values[14] = 0.5F * (1 - cosine);
  world.UpdateCamera(camera_handle, camera);
  light.transform.values[8] = 0.5F;
  light.transform.values[10] = cosine;
  world.UpdateLight(light_handle, light, ChangeAspect::Transform);
  const auto moving = capture("on-moving");
  RequireForwardImage(on.color.pixels != moving.color.pixels, "motion did not change the image");
  world.Remove(light_handle);
  const auto off_moving = capture("off-moving");
  camera = {};
  world.UpdateCamera(camera_handle, camera);
  const auto restored = capture("off-restored");
  RequireForwardImage(same(off, restored), "return to OFF changed the image");
  light.transform = {};
  const auto restored_light = world.CreateLight(light);
  RequireForwardImage(same(on, capture("on-restored")), "return to ON changed the image");
  light.intensity = 0;
  world.UpdateLight(restored_light, light, ChangeAspect::LightParameters);
  const auto zero = capture("zero-energy");
  for (std::size_t channel = 0; channel < 3; ++channel) {
    RequireForwardImage(center(off, channel) > center(on, channel) &&
                            center(on, channel) >= center(moving, channel) &&
                            center(moving, channel) > center(zero, channel) &&
                            center(off_moving, channel) == center(off, channel),
        "unexpected directional response");
  }
  report << "\n  ]\n}\n";
  RequireForwardImage(bool(report), "report write failed");
  RequireForwardImage(renderer.statistics().validation_messages == 0,
      "Vulkan validation messages");
}

} // namespace merlin::tests
