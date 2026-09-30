#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/metal/backend.hpp>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <string_view>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

namespace {
void Require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
std::uint8_t Channel(const merlin::render::RenderResult& image,
    unsigned x, unsigned y, unsigned c) {
  return image.color.pixels.at(y * image.color.row_pitch_bytes + x * 4 + c);
}
std::uint32_t Id(const merlin::render::RenderResult& image,
    unsigned x, unsigned y) {
  return image.instance_id.pixels.at(y * image.instance_id.row_pitch_bytes / 4 + x);
}
} // namespace

int RunImages(bool gpu) {
  merlin::metal::BackendFactory factory;
  if (!factory.availability().available)
    return 77;
  try {
    merlin::render::BackendCreateInfo info;
    info.frames_in_flight = 3;
    info.enable_validation = true;
    auto backend = factory.Create(info);
    auto reference = factory.Create(info);
    Require(backend->capabilities().gpu_driven_gaussian, "Metal GPU Gaussian capability missing");
    merlin::RenderWorld world;
    merlin::extraction::SceneExtractor extractor;
    merlin::GaussianDescriptor g;
    // Red in front of green, deliberately stored in the reverse of draw order.
    // A third off-center particle tests Metal's Y orientation and IDs.
    g.positions = {{0, 0, 0.3F}, {0, 0, 0.6F}, {0.5F, 0.5F, 0.4F}};
    g.covariances.assign(3, {0.01F, 0, 0, 0.0025F, 0, 0.0001F});
    g.opacities = {0.5F, 0.5F, 1.0F};
    constexpr float sh = 0.2820947918F;
    g.spherical_harmonics_coefficients = {
        {0.5F / sh, -0.5F / sh, -0.5F / sh},
        {-0.5F / sh, 0.5F / sh, -0.5F / sh},
        {-0.5F / sh, -0.5F / sh, 0.5F / sh}};
    const auto handle = world.CreateGaussian(g);
    auto snapshot = [&] {
      extractor.Apply(world, world.Commit());
      return extractor.snapshot();
    };
    merlin::render::RenderRequest request;
    request.snapshot = snapshot();
    request.width = request.height = 64;
    request.clear_color = {0, 0, 0, 0};
    request.products = {{merlin::Aov::Color, true}, {merlin::Aov::Depth, true},
        {merlin::Aov::PrimId, true}, {merlin::Aov::InstanceId, true}};
    if (gpu) request.gpu_driven_gaussian.mode = merlin::render::GpuDrivenGaussianMode::Require;
    auto render = [&] {
      auto actual = backend->Resolve(backend->Submit(request));
      if (request.gpu_driven_gaussian.mode != merlin::render::GpuDrivenGaussianMode::Disabled) {
        auto cpu = request;
        cpu.gpu_driven_gaussian.mode = merlin::render::GpuDrivenGaussianMode::Disabled;
        const auto expected = reference->Resolve(reference->Submit(cpu));
        Require(actual.color.pixels.size() == expected.color.pixels.size(), "GPU image size differs");
        std::size_t worst_index = 0, differing = 0;
        int worst = 0;
        for (std::size_t i = 0; i < actual.color.pixels.size(); ++i) {
          const auto delta = std::abs(int(actual.color.pixels[i]) - int(expected.color.pixels[i]));
          if (delta > 2) ++differing;
          if (delta > worst) { worst = delta; worst_index = i; }
        }
        if (worst > 2)
          throw std::runtime_error("GPU color differs from reference: max=" +
              std::to_string(worst) + " at " + std::to_string(worst_index) +
              " actual=" + std::to_string(actual.color.pixels[worst_index]) +
              " expected=" + std::to_string(expected.color.pixels[worst_index]) +
              " differing=" + std::to_string(differing) +
              " tile=" + std::to_string(actual.telemetry.gaussian_gpu_tile_raster_frame_count));
        Require(actual.depth.pixels == expected.depth.pixels &&
                    actual.prim_id.pixels == expected.prim_id.pixels &&
                    actual.instance_id.pixels == expected.instance_id.pixels,
            "GPU depth/ID differs from reference");
        Require(actual.telemetry.gaussian_candidate_count == expected.telemetry.gaussian_candidate_count &&
                    actual.telemetry.gaussian_visible_count == expected.telemetry.gaussian_visible_count &&
                    actual.telemetry.gaussian_hidden_count == expected.telemetry.gaussian_hidden_count &&
                    actual.telemetry.gaussian_opacity_culled_count == expected.telemetry.gaussian_opacity_culled_count &&
                    actual.telemetry.gaussian_frustum_culled_count == expected.telemetry.gaussian_frustum_culled_count &&
                    actual.telemetry.gaussian_invalid_culled_count == expected.telemetry.gaussian_invalid_culled_count &&
                    actual.telemetry.gaussian_gpu_sorted_count == expected.telemetry.gaussian_sorted_count &&
                    actual.telemetry.gaussian_gpu_raster_instance_count ==
                        (actual.telemetry.gaussian_gpu_tile_raster_frame_count ? 0 : expected.telemetry.gaussian_visible_count),
            "GPU counters differ from reference");
        Require(actual.telemetry.gaussian_upload_bytes == 0 &&
                    actual.telemetry.gaussian_preparation_cache_misses == 0 &&
                    actual.timings.gaussian_preparation_ns == 0,
            "GPU frame performed CPU particle preparation");
      }
      return actual;
    };
    const auto first = render();
    if (gpu && backend->capabilities().gaussian_gpu_stage_timestamps) {
      Require(first.timings.gaussian_gpu_preparation_ns > 0 &&
                  first.timings.gaussian_gpu_sort_ns > 0 &&
                  first.timings.gaussian_raster_ns > 0,
          "Metal Gaussian stage samples were not resolved");
    }
    Require(first.telemetry.gaussian_visible_count == 3 &&
                first.telemetry.gaussian_draw_count == 1,
        "missing Gaussian draw");
    Require((gpu ? first.telemetry.gaussian_attribute_upload_bytes : first.telemetry.gaussian_upload_bytes) != 0, "missing upload telemetry");
    const auto red = Channel(first, 32, 32, 0);
    const auto green = Channel(first, 32, 32, 1);
    Require(red > 105 && red < 130 && green > 55 && green < 70,
        "back-to-front alpha blending failed");
    Require(Id(first, 32, 32) == 0, "nearest particle did not win picking");
    Require(first.prim_id.pixels.at(32 * first.prim_id.row_pitch_bytes / 4 + 32) ==
                static_cast<std::uint32_t>(handle.value()),
        "Gaussian resource ID mismatch");
    Require(first.depth.pixels.at(32 * first.depth.row_pitch_bytes / 4 + 32) == 1.0F,
        "transparent Gaussian wrote depth");
    Require(Id(first, 48, 16) == 2 && Channel(first, 48, 16, 2) > 200,
        "Metal Gaussian is vertically inverted");
    Require(Id(first, 48, 48) == std::numeric_limits<std::uint32_t>::max(),
        "Gaussian appeared in the mirrored location");
    Require(Channel(first, 39, 39, 0) == 0, "ellipse leaked into bounding quad");
    Require(Channel(first, 35, 32, 0) > Channel(first, 32, 35, 0),
        "anisotropic covariance was lost");
    // All frame slots reuse the same immutable stream, with no new uploads.
    for (unsigned i = 0; i != 4; ++i) {
      const auto steady = render();
      Require(steady.color.pixels == first.color.pixels, "static frame drifted");
      Require((gpu ? steady.telemetry.gaussian_attribute_upload_bytes == 0 : steady.telemetry.gaussian_preparation_cache_hits == 1) &&
                  steady.telemetry.gaussian_upload_bytes == 0,
          "static stream was rebuilt");
    }
    request.gpu_driven_gaussian.mode = merlin::render::GpuDrivenGaussianMode::Prefer;
    request.gpu_driven_gaussian.raster = merlin::render::GaussianRasterPath::Tiled;
    const auto tiled = render();
    Require(tiled.telemetry.gaussian_gpu_tile_raster_frame_count == 1 &&
                tiled.telemetry.gaussian_gpu_raster_instance_count == 0 &&
                tiled.telemetry.gaussian_gpu_fallback_count == 0,
        "tiled prefer did not select compute raster");
    request.gpu_driven_gaussian.mode = merlin::render::GpuDrivenGaussianMode::Require;
    Require(render().telemetry.gaussian_gpu_tile_raster_frame_count == 1,
        "required tiled raster was not selected");
    request.gpu_driven_gaussian.raster = merlin::render::GaussianRasterPath::SortedStream;
    request.gpu_driven_gaussian.mode = gpu ? merlin::render::GpuDrivenGaussianMode::Require : merlin::render::GpuDrivenGaussianMode::Disabled;

    // Submit edits before resolving the earlier frame: each must retain its
    // own stream, even when the global preparation cache has been replaced.
    const auto old_token = backend->Submit(request);
    g.opacities[0] = 0;
    world.UpdateGaussian(handle, g);
    request.snapshot = snapshot();
    const auto edited_token = backend->Submit(request);
    const auto old_image = backend->Resolve(old_token);
    const auto edited = backend->Resolve(edited_token);
    Require(old_image.color.pixels == first.color.pixels,
        "in-flight Gaussian stream was overwritten");
    Require(Id(edited, 32, 32) == 1 && Channel(edited, 32, 32, 0) == 0,
        "opacity edit did not invalidate the cache");

    // A camera move changes projected position without changing table identity.
    auto moved = std::make_shared<merlin::extraction::FrameSnapshot>(*request.snapshot);
    moved->view.values[12] = -0.5F;
    request.snapshot = moved;
    const auto camera = render();
    Require((gpu ? camera.telemetry.gaussian_attribute_upload_bytes == 0 && camera.telemetry.allocation_count == 0 : camera.telemetry.gaussian_preparation_cache_misses == 1) &&
                Id(camera, 16, 32) == 1,
        "camera motion did not rebuild projection");
    request.width = 96;
    const auto resized = render();
    Require((gpu ? resized.telemetry.gaussian_attribute_upload_bytes == 0 : resized.telemetry.gaussian_preparation_cache_misses == 1) &&
                Id(resized, 24, 32) == 1,
        "resize did not rebuild projection");
    request.width = 64;

    // Opaque mesh at z=.45 hides green (.6) but permits red (.3).
    merlin::MeshDescriptor mesh;
    mesh.positions = {{-0.8F, -0.8F, 0.45F}, {0.8F, -0.8F, 0.45F},
        {0.8F, 0.8F, 0.45F}, {-0.8F, 0.8F, 0.45F}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    const auto mesh_handle = world.CreateMesh(mesh);
    merlin::MaterialDescriptor material;
    material.parameters.base_color = {0, 0, 1, 1};
    material.double_sided = true;
    const auto material_handle = world.CreateMaterial(material);
    merlin::InstanceDescriptor instance;
    instance.mesh = mesh_handle;
    instance.material = material_handle;
    world.CreateInstance(instance);
    g.opacities[0] = 0.5F;
    world.UpdateGaussian(handle, g);
    request.snapshot = snapshot();
    const auto mixed = render();
    Require(Channel(mixed, 32, 32, 0) > 105 && Channel(mixed, 32, 32, 1) == 0 &&
                Channel(mixed, 32, 32, 2) > 120,
        "mesh/Gaussian depth composition failed");
    Require(std::abs(mixed.depth.pixels.at(32 * mixed.depth.row_pitch_bytes / 4 + 32) - 0.45F) < 0.001F,
        "Gaussian changed opaque depth");
    Require(Id(mixed, 32, 32) == 0, "front Gaussian lost its picking ID");
    if (gpu) {
      request.gpu_driven_gaussian.raster = merlin::render::GaussianRasterPath::Tiled;
      Require(render().telemetry.gaussian_gpu_tile_raster_frame_count == 1,
          "tiled mesh/Gaussian composition was not selected");
    }
    g.visible = false;
    world.UpdateGaussian(handle, g);
    request.snapshot = snapshot();
    const auto hidden = render();
    Require(hidden.telemetry.gaussian_visible_count == 0 &&
                hidden.telemetry.gaussian_draw_count == (gpu ? 1U : 0U) &&
                Channel(hidden, 32, 32, 0) == 0,
        "hidden Gaussian retained old stream");
    world.Remove(handle);
    request.snapshot = snapshot();
    Require(render().telemetry.gaussian_candidate_count == 0, "removed Gaussian remained");
    g.visible = true;
    const auto replacement = world.CreateGaussian(g);
    request.snapshot = snapshot();
    Require(render().telemetry.gaussian_visible_count == 3, "reintroduced Gaussian did not render");
    g.opacities.assign(3, 0);
    world.UpdateGaussian(replacement, g);
    request.snapshot = snapshot();
    Require(render().telemetry.gaussian_visible_count == 0, "all-rejected frame reused stale indirect count");
    Require(backend->statistics().validation_messages == 0, "Metal validation failed");
    std::cout << (gpu ? "GPU" : "CPU") << " Metal Gaussian image, AOV, composition, cache and in-flight tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}

int RunContracts() {
  using namespace merlin;
  using namespace merlin::render;
  try {
    auto scene = std::make_shared<extraction::FrameSnapshot>();
    scene->source_id = 7;
    extraction::GaussianRecord record;
    record.gaussian = 0x100000002ULL;
    record.revision = record.positions_revision = record.covariance_revision =
        record.opacity_revision = record.radiance_revision = 1;
    record.positions = std::make_shared<const std::vector<Vec3>>(3, Vec3{0, 0, 0.5F});
    record.covariances = std::make_shared<const std::vector<Covariance3>>(
        3, Covariance3{0.01F, 0, 0, 0.01F, 0, 0.001F});
    record.opacities = std::make_shared<const std::vector<float>>(3, 0.5F);
    record.spherical_harmonics_coefficients = std::make_shared<const std::vector<Vec3>>(3, Vec3{});
    scene->gaussians.assign({record});
    RenderRequest request;
    request.snapshot = scene;
    request.width = request.height = 64;
    request.products = {{Aov::Color, false}};
    request.gpu_driven_gaussian.mode = GpuDrivenGaussianMode::Require;
    BackendCreateInfo info;
    info.frames_in_flight = 3;
    info.enable_validation = true;
    metal::Backend backend(info, {});
    auto render = [&] { return backend.Resolve(backend.Submit(request)); };
    const auto first = render();
    const auto single_scratch_bytes = backend.metal_statistics().gaussian_scratch_live_bytes;
    Require(first.color.pixels.empty() && first.telemetry.gaussian_gpu_raster_instance_count == 3 &&
                first.telemetry.readback_bytes == 64,
        "GPU-only products must still resolve small telemetry without image readback");
    for (unsigned i = 0; i < 4; ++i) render(); // Warm each frame's telemetry buffer.
    const auto warmed = render();
    Require(warmed.telemetry.allocation_count == 0 && warmed.telemetry.upload_bytes == 0,
        "static GPU frame allocated or uploaded");
    auto edited = std::make_shared<extraction::FrameSnapshot>(*scene);
    record.particle_base_revision = 1;
    record.revision = record.opacity_revision = 2;
    record.particle_ranges = {{1, 1}};
    record.opacities = std::make_shared<const std::vector<float>>(std::initializer_list<float>{0.5F, 0, 0.5F});
    edited->gaussians.assign({record});
    const auto original = backend.Submit(request);
    request.snapshot = edited;
    const auto changed = backend.Submit(request);
    const auto exporter_token = backend.Submit(request);
    auto lease = backend.AcquireAovImage(exporter_token, Aov::Color);
    Require(backend.Resolve(changed).telemetry.gaussian_attribute_upload_bytes == sizeof(float),
        "localized opacity edit uploaded unchanged attributes");
    Require(backend.Resolve(original).telemetry.gaussian_gpu_raster_instance_count == 3,
        "unresolved original frame lost its attributes");
    Require(backend.Resolve(exporter_token).telemetry.gaussian_gpu_raster_instance_count == 2,
        "exported GPU frame lost its edited image");
    backend.ReleaseAovImage(std::move(lease.lease));
    Require(backend.metal_statistics().gaussian_attribute_device_copy_bytes == 3 * sizeof(float),
        "immutable range edit did not report device copy");
    auto transformed = std::make_shared<extraction::FrameSnapshot>(*edited);
    record.transform.values[12] = 0.125F;
    transformed->gaussians.assign({record});
    request.snapshot = transformed;
    Require(render().telemetry.gaussian_attribute_upload_bytes == 0,
        "transform edit uploaded attributes");
    transformed = std::make_shared<extraction::FrameSnapshot>(*transformed);
    transformed->source_id = 8;
    request.snapshot = transformed;
    Require(render().telemetry.gaussian_attribute_upload_bytes == 156,
        "new source reused foreign residency");
    const auto stats = backend.metal_statistics();
    Require(stats.gaussian_resident_live_bytes != 0 && stats.gaussian_scratch_live_bytes != 0 &&
                stats.gaussian_compute_dispatch_count != 0,
        "missing Gaussian memory/dispatch statistics");

    // A render error after successful compute encoding must release the frame
    // lease too, so another context can reuse the single allowed scratch set.
    metal::BackendOptions single_options;
    single_options.gaussian_scratch_budget_bytes = single_scratch_bytes;
    metal::Backend abandoned(info, single_options);
    auto invalid = std::make_shared<extraction::FrameSnapshot>(*scene);
    invalid->draws.push_back({});
    request.snapshot = invalid;
    bool draw_rejected = false;
    try { (void)abandoned.Submit(request); }
    catch (const RendererError& error) {
      draw_rejected = error.code() == RendererErrorCode::InvalidRequest &&
          error.operation() == "encode Metal draw";
    }
    Require(draw_rejected && abandoned.metal_statistics().gaussian_resident_live_bytes == 0,
        "post-compute render failure retained unpublished attributes");
    request.snapshot = scene;
    Require(abandoned.Resolve(abandoned.Submit(request)).telemetry.gaussian_attribute_upload_bytes == 156,
        "post-compute render failure retained scratch or published attributes");

    // Both budgets are independently enforceable; Prefer discards an unfinished
    // GPU command and renders the CPU reference, Require rejects, then recovery
    // with an empty/smaller scene remains possible on the same backend.
    for (bool residency : {false, true}) {
      metal::BackendOptions options;
      if (residency) options.gaussian_residency_budget_bytes = 1;
      else options.gaussian_scratch_budget_bytes = 1;
      metal::Backend limited(info, options);
      request.snapshot = scene;
      request.products = {{Aov::Color, true}};
      request.gpu_driven_gaussian.mode = GpuDrivenGaussianMode::Prefer;
      const auto fallback = limited.Resolve(limited.Submit(request));
      Require(fallback.telemetry.gaussian_gpu_fallback_count == 1 &&
                  fallback.telemetry.gaussian_upload_bytes != 0 &&
                  fallback.telemetry.gaussian_gpu_raster_instance_count == 0 &&
                  !fallback.color.pixels.empty(), "budget Prefer fallback failed");
      request.gpu_driven_gaussian.mode = GpuDrivenGaussianMode::Require;
      bool rejected = false;
      try { (void)limited.Submit(request); }
      catch (const RendererError& error) { rejected = error.code() == RendererErrorCode::ResourceExhausted; }
      Require(rejected, "budget Require did not report resource exhaustion");
      request.gpu_driven_gaussian.mode = GpuDrivenGaussianMode::Disabled;
      Require(limited.Resolve(limited.Submit(request)).color.pixels == fallback.color.pixels,
          "abandoned GPU command broke subsequent reference frames");
      Require(limited.metal_statistics().gaussian_resident_live_bytes == 0 &&
                  limited.metal_statistics().gaussian_scratch_live_bytes == 0,
          "abandoned partial GPU allocation leaked");
      if (residency) {
        request.snapshot = std::make_shared<extraction::FrameSnapshot>();
        request.gpu_driven_gaussian.mode = GpuDrivenGaussianMode::Require;
        Require(limited.Resolve(limited.Submit(request)).telemetry.gaussian_gpu_raster_instance_count == 0,
            "empty GPU frame could not recover after exhausted residency");
      }
    }
    std::cout << "Metal GPU Gaussian residency, leases, telemetry and budget recovery passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

// Optional local measurement, deliberately excluded from timing-sensitive CI.
int RunTileOverflow() {
  using namespace merlin;
  metal::BackendFactory factory;
  if (!factory.availability().available) return 77;
  try {
    render::BackendCreateInfo info;
    info.enable_validation = true;
    auto backend = factory.Create(info);
    RenderWorld world;
    extraction::SceneExtractor extractor;
    GaussianDescriptor gaussian;
    gaussian.positions.assign(300, {0, 0, 0.5F});
    gaussian.covariances.assign(300, {0.01F, 0, 0, 0.01F, 0, 0.0001F});
    gaussian.opacities.assign(300, 0.05F);
    gaussian.spherical_harmonics_coefficients.assign(300, {0, 0, 0});
    world.CreateGaussian(gaussian);
    extractor.Apply(world, world.Commit());
    render::RenderRequest request;
    request.snapshot = extractor.snapshot();
    request.width = request.height = 64;
    request.products = {{Aov::Color, true}, {Aov::Depth, true},
        {Aov::PrimId, true}, {Aov::InstanceId, true}};
    request.gpu_driven_gaussian.mode = render::GpuDrivenGaussianMode::Require;
    request.gpu_driven_gaussian.raster = render::GaussianRasterPath::SortedStream;
    const auto sorted = backend->Resolve(backend->Submit(request));
    request.gpu_driven_gaussian.raster = render::GaussianRasterPath::Tiled;
    const auto tiled = backend->Resolve(backend->Submit(request));
    Require(tiled.telemetry.gaussian_gpu_tile_raster_frame_count == 1,
        "large tile frame did not select compute raster");
    request.gpu_driven_gaussian.tile_pair_capacity = 256;
    const auto overflow = backend->Resolve(backend->Submit(request));
    Require(overflow.telemetry.gaussian_gpu_tile_raster_frame_count == 0 &&
                overflow.telemetry.gaussian_gpu_tile_raster_overflow_fallback_count == 1 &&
                overflow.telemetry.gaussian_gpu_tile_requested_pair_count >
                    overflow.telemetry.gaussian_gpu_tile_pair_capacity &&
                overflow.telemetry.gaussian_gpu_raster_instance_count == 300,
        "tile overflow did not keep the complete sorted draw");
    Require(overflow.color.pixels == sorted.color.pixels &&
                overflow.depth.pixels == sorted.depth.pixels &&
                overflow.prim_id.pixels == sorted.prim_id.pixels &&
                overflow.instance_id.pixels == sorted.instance_id.pixels,
        "tile overflow changed the sorted image");
    request.gpu_driven_gaussian.mode = render::GpuDrivenGaussianMode::Prefer;
    request.gpu_driven_gaussian.tile_pair_capacity =
        std::numeric_limits<std::uint32_t>::max();
    const auto unsupported = backend->Resolve(backend->Submit(request));
    Require(unsupported.telemetry.gaussian_gpu_fallback_count == 1 &&
                unsupported.telemetry.gaussian_gpu_sorted_count == 300 &&
                unsupported.color.pixels == sorted.color.pixels,
        "unsupported tile allocation did not select GPU sorted fallback");
    auto cold_backend = factory.Create(info);
    const auto cold_unsupported = cold_backend->Resolve(cold_backend->Submit(request));
    Require(cold_unsupported.telemetry.gaussian_gpu_fallback_count == 1 &&
                cold_unsupported.color.pixels == sorted.color.pixels,
        "cold unsupported tile allocation did not select GPU sorted fallback");
    request.gpu_driven_gaussian.mode = render::GpuDrivenGaussianMode::Require;
    bool rejected = false;
    try {
      (void)backend->Submit(request);
    } catch (const render::RendererError& error) {
      rejected = error.code() == render::RendererErrorCode::Unsupported;
    }
    Require(rejected, "required unsupported tile capacity was accepted");
    std::cout << "Metal tile selection and overflow image passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

int RunTileScale() {
  using namespace merlin;
  metal::BackendFactory factory;
  if (!factory.availability().available) return 77;
  try {
    std::cout << "path,particles,phase,frame,wall_ns,gpu_ns,prepare_ns,sort_ns,tile_ns,raster_ns,selected,allocations,readback_bytes,max_color_delta\n";
    for (const std::uint32_t count : {65536U, 1048576U}) {
      extraction::GaussianRecord record;
      record.gaussian = 0x100000001ULL;
      record.revision = record.positions_revision = record.covariance_revision =
          record.opacity_revision = record.radiance_revision = 1;
      auto positions = std::make_shared<std::vector<Vec3>>();
      positions->reserve(count);
      for (std::uint32_t i = 0; i < count; ++i)
        positions->push_back({float(i % 1024) / 512 - 1,
            float((i / 1024) % 1024) / 512 - 1, 0.25F + float(i % 256) / 512});
      record.positions = positions;
      record.covariances = std::make_shared<const std::vector<Covariance3>>(
          count, Covariance3{0.000001F, 0, 0, 0.000001F, 0, 0.000001F});
      record.opacities = std::make_shared<const std::vector<float>>(count, 0.5F);
      record.spherical_harmonics_coefficients =
          std::make_shared<const std::vector<Vec3>>(count, Vec3{});
      std::vector<render::RenderResult> references;
      for (bool tiled : {false, true}) {
        auto backend = factory.Create({});
        auto scene = std::make_shared<extraction::FrameSnapshot>();
        scene->source_id = 1;
        scene->gaussians.assign({record});
        render::RenderRequest request;
        request.width = 512;
        request.height = 512;
        request.products = {{Aov::Color, true}, {Aov::Depth, true},
            {Aov::PrimId, true}, {Aov::InstanceId, true}};
        request.gpu_driven_gaussian.mode = render::GpuDrivenGaussianMode::Require;
        request.gpu_driven_gaussian.raster = tiled
            ? render::GaussianRasterPath::Tiled : render::GaussianRasterPath::SortedStream;
        // Five unreported camera frames warm GPU clocks and scratch pools
        // before the five measured camera/image comparisons.
        for (unsigned i = 0; i < 11; ++i) {
          if (i) {
            scene = std::make_shared<extraction::FrameSnapshot>(*scene);
            scene->view.values[12] = float(i) / 1024;
          }
          request.snapshot = scene;
          const auto begin = std::chrono::steady_clock::now();
          auto result = backend->Resolve(backend->Submit(request));
          const auto wall = std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - begin).count();
          int max_delta = 0;
          if (tiled) {
            Require(result.telemetry.gaussian_gpu_tile_raster_frame_count == 1,
                "scale frame did not select tile raster");
            const auto& expected = references[i];
            Require(result.depth.pixels == expected.depth.pixels &&
                        result.prim_id.pixels == expected.prim_id.pixels &&
                        result.instance_id.pixels == expected.instance_id.pixels,
                "scale tile depth or IDs differ from sorted stream");
            for (std::size_t j = 0; j < result.color.pixels.size(); ++j)
              max_delta = std::max(max_delta, std::abs(int(result.color.pixels[j]) -
                  int(expected.color.pixels[j])));
            Require(max_delta <= 2, "scale tile color differs from sorted stream");
          } else {
            references.push_back(result);
          }
          if (i > 0 && i < 6) continue;
          std::cout << (tiled ? "tiled" : "sorted") << ',' << count << ','
                    << (i ? "camera" : "cold") << ',' << i << ',' << wall << ','
                    << result.timings.gpu_execution_ns << ','
                    << result.timings.gaussian_gpu_preparation_ns << ','
                    << result.timings.gaussian_gpu_sort_ns << ','
                    << result.timings.gaussian_gpu_tile_ns << ','
                    << result.timings.gaussian_raster_ns << ','
                    << result.telemetry.gaussian_gpu_tile_raster_frame_count << ','
                    << result.telemetry.allocation_count << ','
                    << result.telemetry.readback_bytes << ',' << max_delta << '\n';
        }
      }
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

int Benchmark() {
  using namespace merlin;
  using namespace merlin::render;
  metal::BackendFactory factory;
  if (!factory.availability().available) return 77;
  try {
    std::cout << "path,particles,phase,frame,wall_ns,gpu_ns,record_ns,cpu_prepare_ns,attribute_bytes,prepared_bytes,allocations,readback_bytes,gpu_prepare_ns,gpu_sort_ns,gpu_raster_ns\n";
    for (const std::uint32_t count : {65536U, 1048576U}) {
      extraction::GaussianRecord record;
      record.gaussian = 0x100000001ULL;
      record.revision = record.positions_revision = record.covariance_revision =
          record.opacity_revision = record.radiance_revision = 1;
      auto positions = std::make_shared<std::vector<Vec3>>();
      positions->reserve(count);
      // Binary-exact deterministic grid/depth keys, entirely public input.
      for (std::uint32_t i = 0; i < count; ++i)
        positions->push_back({float(i % 1024) / 512 - 1, float((i / 1024) % 1024) / 512 - 1,
            0.25F + float(i % 256) / 512});
      record.positions = positions;
      record.covariances = std::make_shared<const std::vector<Covariance3>>(
          count, Covariance3{0.000001F, 0, 0, 0.000001F, 0, 0.000001F});
      record.opacities = std::make_shared<const std::vector<float>>(count, 0.5F);
      record.spherical_harmonics_coefficients = std::make_shared<const std::vector<Vec3>>(count, Vec3{});
      for (bool gpu : {false, true}) {
        auto backend = factory.Create({});
        std::cerr << "benchmark device=" << backend->capabilities().device_name
                  << " particles=" << count << " path=" << (gpu ? "gpu" : "cpu") << '\n';
        auto scene = std::make_shared<extraction::FrameSnapshot>();
        scene->source_id = 1;
        scene->gaussians.assign({record});
        RenderRequest request;
        request.width = 1024;
        request.height = 768;
        request.products = {{Aov::Color, false}};
        request.gpu_driven_gaussian.mode = gpu ? GpuDrivenGaussianMode::Require : GpuDrivenGaussianMode::Disabled;
        for (std::string_view phase : {"cold", "static", "camera", "edit"}) {
          for (unsigned i = 0; i < (phase == "cold" ? 1U : 12U); ++i) {
            if (phase == "camera") {
              scene = std::make_shared<extraction::FrameSnapshot>(*scene);
              scene->view.values[12] = float(i + 1) / 1024;
            } else if (phase == "edit") {
              scene = std::make_shared<extraction::FrameSnapshot>(*scene);
              auto changed = scene->gaussians[0];
              auto opacity = std::make_shared<std::vector<float>>(*changed.opacities);
              (*opacity)[i] = 0.25F;
              changed.opacities = opacity;
              changed.particle_base_revision = changed.revision++;
              ++changed.opacity_revision;
              changed.particle_ranges = {{i, 1}};
              scene->gaussians.assign({changed});
            }
            request.snapshot = scene;
            const auto begin = std::chrono::steady_clock::now();
            auto result = backend->Resolve(backend->Submit(request));
            const auto wall = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - begin).count();
            const auto& t = result.telemetry;
            if (gpu && phase == "camera")
              Require(t.gaussian_attribute_upload_bytes == 0 && t.gaussian_upload_bytes == 0 &&
                          t.allocation_count == 0 && result.timings.gaussian_preparation_ns == 0,
                  "scale camera frame traversed or uploaded particles");
            if (gpu && phase == "edit")
              Require(t.gaussian_attribute_upload_bytes == sizeof(float), "scale edit was not range-only");
            std::cout << (gpu ? "gpu" : "cpu") << ',' << count << ',' << phase << ',' << i << ','
                      << wall << ',' << result.timings.gpu_execution_ns << ',' << result.timings.command_recording_ns << ','
                      << result.timings.gaussian_preparation_ns << ',' << t.gaussian_attribute_upload_bytes << ','
                      << t.gaussian_upload_bytes << ',' << t.allocation_count << ',' << t.readback_bytes << ','
                      << result.timings.gaussian_gpu_preparation_ns << ','
                      << result.timings.gaussian_gpu_sort_ns << ','
                      << result.timings.gaussian_raster_ns << '\n';
          }
        }
      }
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--benchmark") return Benchmark();
  if (argc == 2 && std::string_view(argv[1]) == "--tile-scale") return RunTileScale();
  const int cpu = RunImages(false);
  if (cpu) return cpu;
  const int gpu = RunImages(true);
  if (gpu) return gpu;
  const int overflow = RunTileOverflow();
  if (overflow) return overflow;
  return RunContracts();
}
