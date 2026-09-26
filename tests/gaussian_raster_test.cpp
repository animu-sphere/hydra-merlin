// Exercises the Vulkan Gaussian path end to end: GPU projection/culling
// dispatch, the verified GPU radix sort, sorted-stream indirect raster and its
// image parity with the CPU-sorted reference raster, counter readback,
// prepared-stream upload, procedural ellipse rasterization, alpha compositing,
// Mesh depth composition, ID output, and steady-state frame-local reuse.

#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/vulkan/renderer.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::uint8_t Channel(const merlin::vulkan::RenderResult& result,
    std::uint32_t x, std::uint32_t y,
    std::uint32_t channel) {
  const auto index = static_cast<std::size_t>(y) *
                         result.color.row_pitch_bytes +
                     static_cast<std::size_t>(x) * 4U + channel;
  return result.color.pixels.at(index);
}

std::size_t PixelIndex(const merlin::vulkan::RenderResult& result,
    std::uint32_t x, std::uint32_t y) {
  return static_cast<std::size_t>(y) *
             (result.depth.row_pitch_bytes / sizeof(float)) +
         x;
}

bool ThrowsRendererError(const std::function<void()>& action,
    merlin::vulkan::RendererErrorCode code) {
  try {
    action();
  } catch (const merlin::vulkan::RendererError& error) {
    return error.code() == code;
  }
  return false;
}

// The GPU sort must produce the CPU reference order, verify its own keys,
// and retain every visible prepared record exactly once.
void RequireVerifiedSort(const merlin::vulkan::RenderResult& result,
    const char* message) {
  Require(result.counters.gaussian_gpu_sorted_count ==
                  result.counters.gaussian_gpu_preparation_visible_count &&
              result.counters.gaussian_gpu_sorted_count ==
                  result.counters.gaussian_visible_count &&
              result.counters.gaussian_gpu_sort_reference_divergence_count ==
                  0 &&
              result.counters.gaussian_gpu_sort_fallback_count == 0,
      message);
}

struct ImageDifference {
  std::uint32_t max_color_channel{};
  std::size_t color_pixels{};
  std::size_t depth_pixels{};
  std::size_t prim_id_pixels{};
  std::size_t instance_id_pixels{};
};

// Pixel-wise comparison against the CPU-sorted reference. Both frames share
// the viewport, so the readback pitches match.
ImageDifference Compare(const merlin::vulkan::RenderResult& reference,
    const merlin::vulkan::RenderResult& candidate) {
  Require(reference.color.pixels.size() == candidate.color.pixels.size() &&
              reference.depth.pixels.size() ==
                  candidate.depth.pixels.size() &&
              reference.prim_id.pixels.size() ==
                  candidate.prim_id.pixels.size() &&
              reference.instance_id.pixels.size() ==
                  candidate.instance_id.pixels.size(),
      "compared Gaussian frames do not share a readback shape");
  ImageDifference difference;
  for (std::size_t i = 0; i < reference.color.pixels.size(); i += 4U) {
    std::uint32_t pixel_max{};
    for (std::size_t channel = 0; channel < 4U; ++channel) {
      const auto lhs = reference.color.pixels[i + channel];
      const auto rhs = candidate.color.pixels[i + channel];
      pixel_max = std::max<std::uint32_t>(pixel_max,
          lhs > rhs ? lhs - rhs : rhs - lhs);
    }
    difference.max_color_channel =
        std::max(difference.max_color_channel, pixel_max);
    difference.color_pixels += pixel_max != 0U ? 1U : 0U;
  }
  for (std::size_t i = 0; i < reference.depth.pixels.size(); ++i) {
    difference.depth_pixels +=
        reference.depth.pixels[i] != candidate.depth.pixels[i] ? 1U : 0U;
    difference.prim_id_pixels +=
        reference.prim_id.pixels[i] != candidate.prim_id.pixels[i] ? 1U : 0U;
    difference.instance_id_pixels +=
        reference.instance_id.pixels[i] != candidate.instance_id.pixels[i]
            ? 1U
            : 0U;
  }
  return difference;
}

void Report(const char* label, const ImageDifference& difference) {
  std::cout << label << ": max color channel delta "
            << difference.max_color_channel << " over "
            << difference.color_pixels << " pixels; depth "
            << difference.depth_pixels << ", primId "
            << difference.prim_id_pixels << ", instanceId "
            << difference.instance_id_pixels << " differing pixels\n";
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: gaussian_raster_test SHADER_DIR ENVIRONMENT_HDR\n";
    return 1;
  }
  const std::filesystem::path shader_dir = argv[1];
  const merlin::vulkan::ShaderPaths shaders{
      shader_dir / "triangle.vert.spv",
      shader_dir / "triangle.frag.spv",
      shader_dir / "triangle.bindless.vert.spv",
      shader_dir / "triangle.bindless.frag.spv",
      argv[2],
      shader_dir / "gaussian.vert.spv",
      shader_dir / "gaussian.frag.spv",
      shader_dir / "gaussian-id.frag.spv",
  };

  std::optional<merlin::vulkan::Renderer> renderer;
  try {
    merlin::vulkan::RendererOptions options;
    options.frames_in_flight = 2;
    options.enable_validation = true;
    options.descriptor_backend =
        merlin::vulkan::DescriptorBackendRequest::Conventional;
    renderer.emplace(options);
  } catch (const std::exception& error) {
    std::cerr << "skip: Vulkan renderer unavailable: " << error.what()
              << '\n';
    return 77;
  }

  try {
    merlin::RenderWorld world;
    merlin::MeshDescriptor mesh;
    mesh.label = "background-quad";
    mesh.positions = {{-0.8F, -0.8F, 0.8F}, {0.8F, -0.8F, 0.8F},
        {0.8F, 0.8F, 0.8F}, {-0.8F, 0.8F, 0.8F}};
    mesh.normals.assign(4, {0.0F, 0.0F, 1.0F});
    mesh.indices = {0, 1, 2, 0, 2, 3};
    const auto mesh_handle = world.CreateMesh(std::move(mesh));
    merlin::MaterialDescriptor material;
    material.parameters.base_color = {0.02F, 0.05F, 0.8F, 1.0F};
    material.double_sided = true;
    const auto material_handle = world.CreateMaterial(std::move(material));
    merlin::InstanceDescriptor instance;
    instance.mesh = mesh_handle;
    instance.material = material_handle;
    const auto instance_handle = world.CreateInstance(instance);

    merlin::GaussianDescriptor gaussian;
    gaussian.label = "red-splat";
    gaussian.positions = {{0.0F, 0.0F, 0.5F}, {0.55F, 0.55F, 0.6F}};
    gaussian.covariances = {
        {0.01F, 0.0F, 0.0F, 0.01F, 0.0F, 0.0001F},
        {0.001F, 0.0F, 0.0F, 0.001F, 0.0F, 0.0001F}};
    gaussian.opacities = {0.9F, 0.8F};
    gaussian.spherical_harmonics_coefficients = {
        {1.5F, -1.7F, -1.7F}, {0.0F, 0.0F, 1.0F}};
    const auto gaussian_handle = world.CreateGaussian(std::move(gaussian));

    merlin::extraction::SceneExtractor extractor;
    extractor.Apply(world, world.Commit());
    merlin::vulkan::RenderRequest request;
    request.snapshot = extractor.snapshot();
    request.width = 64;
    request.height = 64;
    request.shaders = shaders;
    request.products = {{merlin::Aov::Color, true},
        {merlin::Aov::Depth, true},
        {merlin::Aov::PrimId, true},
        {merlin::Aov::InstanceId, true}};
    request.gpu_driven_gaussian_preparation =
        merlin::vulkan::GpuDrivenGaussianPreparationMode::Require;
    request.gpu_driven_gaussian_sort =
        merlin::vulkan::GpuDrivenGaussianSortMode::Require;

    const auto first = renderer->Resolve(renderer->Submit(request));
    Require(first.counters.gaussian_visible_count == 2,
        "prepared stream did not retain the visible Gaussians");
    Require(first.counters.gaussian_gpu_preparation_dispatch_count == 1,
        "GPU Gaussian preparation did not dispatch per resource");
    Require(first.counters.gaussian_gpu_preparation_candidate_count == 2 &&
                first.counters.gaussian_gpu_preparation_visible_count == 2,
        "GPU Gaussian preparation counters lost visible particles");
    Require(
        first.counters.gaussian_gpu_preparation_opacity_culled_count == 0 &&
            first.counters.gaussian_gpu_preparation_frustum_culled_count == 0 &&
            first.counters.gaussian_gpu_preparation_invalid_culled_count == 0,
        "GPU Gaussian preparation unexpectedly rejected a visible particle");
    RequireVerifiedSort(first, "GPU Gaussian sort diverged from the reference");
    // Two candidates need one low-word and four high-word passes over one
    // padded workgroup: keys, then histogram/scan/scatter per pass, then
    // verification.
    Require(first.counters.gaussian_gpu_sort_key_count == 256 &&
                first.counters.gaussian_gpu_sort_pass_count == 5 &&
                first.counters.gaussian_gpu_sort_dispatch_count == 17,
        "GPU Gaussian sort did not follow its bounded dispatch plan");
    if (renderer->capabilities().timestamp_queries) {
      Require(first.cpu_timings.gaussian_gpu_sort_ns != 0,
          "GPU Gaussian sort did not publish device timing");
    }
    Require(first.counters.gaussian_draw_count == 2,
        "Gaussian color and ID streams were not submitted as two draws");
    Require(first.counters.gaussian_upload_bytes == 104,
        "Gaussian GPU instance upload size drifted");
    Require(first.counters.gaussian_attribute_upload_bytes == 104,
        "persistent Gaussian attribute upload size drifted");
    Require(first.counters.gaussian_attribute_copy_range_count == 4,
        "initial Gaussian attributes were not split by source aspect");
    Require(first.counters.gaussian_attribute_generation_count == 1,
        "initial Gaussian residency generation was not published");
    Require(first.counters.upload_bytes >=
                first.counters.gaussian_upload_bytes,
        "Gaussian upload was not included in total upload telemetry");

    const auto center = PixelIndex(first, 32, 32);
    Require(Channel(first, 32, 32, 0) > 140,
        "Gaussian radiance did not reach the color AOV");
    Require(Channel(first, 32, 32, 0) > Channel(first, 32, 32, 2),
        "Gaussian alpha composition did not dominate the blue mesh");
    Require(Channel(first, 48, 32, 2) > Channel(first, 48, 32, 0),
        "Gaussian conservative quad leaked outside the ellipse");
    Require(first.depth.pixels.at(center) < 0.81F &&
                first.depth.pixels.at(center) > 0.79F,
        "transparent Gaussian unexpectedly replaced Mesh depth");
    Require(first.prim_id.pixels.at(center) ==
                static_cast<std::uint32_t>(gaussian_handle.value()),
        "Gaussian coverage did not write its resource prim ID");
    Require(first.instance_id.pixels.at(center) == 0,
        "Gaussian coverage did not write its particle index");
    const auto mesh_only = PixelIndex(first, 48, 32);
    Require(first.prim_id.pixels.at(mesh_only) ==
                static_cast<std::uint32_t>(mesh_handle.value()),
        "Gaussian ID writes escaped the contributing ellipse");
    Require(first.instance_id.pixels.at(mesh_only) ==
                static_cast<std::uint32_t>(instance_handle.value()),
        "Gaussian particle IDs replaced uncovered Mesh identity");

    const auto steady = renderer->Resolve(renderer->Submit(request));
    Require(steady.counters.gaussian_preparation_cache_hits == 1,
        "static Gaussian frame missed the CPU preparation cache");
    Require(steady.counters.gaussian_upload_bytes == 0,
        "static Gaussian frame re-uploaded its prepared stream");
    Require(steady.counters.gaussian_attribute_upload_bytes == 0,
        "static Gaussian frame re-uploaded persistent attributes");
    Require(steady.counters.gaussian_attribute_copy_range_count == 0,
        "static Gaussian frame recorded an attribute copy");
    Require(steady.counters.gaussian_attribute_generation_count == 0,
        "static Gaussian frame published a new residency generation");
    Require(steady.counters.allocation_count == 0,
        "static Gaussian frame allocated a native resource");
    Require(steady.counters.gaussian_draw_count == 2,
        "static Gaussian frame lost a procedural draw");
    Require(steady.counters.gaussian_gpu_preparation_dispatch_count == 1 &&
                steady.counters.gaussian_gpu_preparation_visible_count == 2,
        "static Gaussian frame lost GPU preparation evidence");
    RequireVerifiedSort(steady, "static Gaussian frame lost its GPU sort");

    // Sorted-stream raster draws the verified GPU order indirectly. It has to
    // reproduce the CPU-sorted reference image above and upload no prepared
    // stream from the CPU.
    request.gpu_driven_gaussian_raster =
        merlin::vulkan::GpuDrivenGaussianRasterMode::Require;
    const auto gpu_raster = renderer->Resolve(renderer->Submit(request));
    RequireVerifiedSort(gpu_raster, "GPU raster frame lost its GPU sort");
    Require(gpu_raster.counters.gaussian_gpu_raster_dispatch_count == 1 &&
                gpu_raster.counters.gaussian_gpu_raster_instance_count == 2 &&
                gpu_raster.counters.gaussian_gpu_raster_indirect_draw_count ==
                    2 &&
                gpu_raster.counters.gaussian_gpu_raster_fallback_count == 0,
        "GPU Gaussian raster did not draw the sorted stream indirectly");
    Require(gpu_raster.counters.gaussian_draw_count == 2,
        "GPU Gaussian raster lost the color or ID draw");
    Require(gpu_raster.counters.gaussian_upload_bytes == 0,
        "GPU Gaussian raster uploaded a CPU-prepared stream");
    const auto gpu_difference = Compare(first, gpu_raster);
    Report("sorted-stream raster vs CPU reference", gpu_difference);
    Require(gpu_difference.max_color_channel <= 1U,
        "GPU Gaussian raster color diverged from the CPU reference");
    Require(gpu_difference.depth_pixels == 0 &&
                gpu_difference.prim_id_pixels == 0 &&
                gpu_difference.instance_id_pixels == 0,
        "GPU Gaussian raster depth or IDs diverged from the CPU reference");
    const auto gpu_steady = renderer->Resolve(renderer->Submit(request));
    Require(gpu_steady.counters.allocation_count == 0 &&
                gpu_steady.counters.upload_bytes == 0,
        "static GPU Gaussian raster frame allocated or uploaded");
    Require(gpu_steady.counters.gaussian_gpu_raster_instance_count == 2,
        "static GPU Gaussian raster frame lost its instances");
    const auto steady_difference = Compare(gpu_raster, gpu_steady);
    Require(steady_difference.color_pixels == 0 &&
                steady_difference.prim_id_pixels == 0 &&
                steady_difference.instance_id_pixels == 0,
        "static GPU Gaussian raster frames are not deterministic");

    // The raster consumes the GPU-sorted stream, so it falls back without the
    // sort when preferred and cannot be required alone.
    request.gpu_driven_gaussian_sort =
        merlin::vulkan::GpuDrivenGaussianSortMode::Disabled;
    request.gpu_driven_gaussian_raster =
        merlin::vulkan::GpuDrivenGaussianRasterMode::Prefer;
    const auto raster_fallback = renderer->Resolve(renderer->Submit(request));
    Require(raster_fallback.counters.gaussian_gpu_raster_fallback_count == 1 &&
                raster_fallback.counters.gaussian_gpu_raster_dispatch_count ==
                    0 &&
                raster_fallback.counters
                        .gaussian_gpu_raster_indirect_draw_count == 0 &&
                raster_fallback.counters.gaussian_draw_count == 2,
        "preferred GPU Gaussian raster did not retain the CPU-sorted draws");
    Require(Compare(first, raster_fallback).color_pixels == 0,
        "GPU Gaussian raster fallback changed the reference image");
    request.gpu_driven_gaussian_raster =
        merlin::vulkan::GpuDrivenGaussianRasterMode::Require;
    Require(ThrowsRendererError(
                [&] { (void)renderer->Submit(request); },
                merlin::vulkan::RendererErrorCode::Unsupported),
        "required Gaussian raster accepted a CPU-sorted frame");
    request.gpu_driven_gaussian_sort =
        merlin::vulkan::GpuDrivenGaussianSortMode::Require;

    // A missing gather artifact keeps the sort and falls back only the raster
    // when preferred, and fails the frame when required.
    request.shaders.gaussian_raster_gather_compute =
        shader_dir / "missing-gaussian-raster-gather.comp.spv";
    Require(ThrowsRendererError(
                [&] { (void)renderer->Submit(request); },
                merlin::vulkan::RendererErrorCode::InvalidRequest),
        "required Gaussian raster accepted a missing gather artifact");
    request.gpu_driven_gaussian_raster =
        merlin::vulkan::GpuDrivenGaussianRasterMode::Prefer;
    const auto gather_fallback = renderer->Resolve(renderer->Submit(request));
    Require(gather_fallback.counters.gaussian_gpu_raster_fallback_count == 1 &&
                gather_fallback.counters.gaussian_gpu_raster_dispatch_count ==
                    0 &&
                gather_fallback.counters.gaussian_draw_count == 2,
        "missing Gaussian raster gather did not fall back independently");
    RequireVerifiedSort(gather_fallback,
        "missing Gaussian raster gather disturbed the GPU sort");
    request.shaders.gaussian_raster_gather_compute.clear();
    request.gpu_driven_gaussian_raster =
        merlin::vulkan::GpuDrivenGaussianRasterMode::Disabled;

    request.gpu_driven_gaussian_preparation =
        merlin::vulkan::GpuDrivenGaussianPreparationMode::Prefer;
    request.gpu_driven_gaussian_sort =
        merlin::vulkan::GpuDrivenGaussianSortMode::Prefer;
    request.shaders.gaussian_prepare_compute =
        shader_dir / "missing-gaussian-prepare.comp.spv";
    const auto fallback = renderer->Resolve(renderer->Submit(request));
    Require(fallback.counters.gaussian_gpu_preparation_fallback_count == 1 &&
                fallback.counters.gaussian_gpu_preparation_dispatch_count == 0,
        "preferred GPU Gaussian preparation did not retain CPU fallback");
    Require(fallback.counters.gaussian_gpu_sort_fallback_count == 1 &&
                fallback.counters.gaussian_gpu_sort_dispatch_count == 0 &&
                fallback.cpu_timings.gaussian_gpu_sort_ns == 0,
        "preferred GPU Gaussian sort ran without GPU preparation");
    Require(fallback.counters.gaussian_visible_count == 2 &&
                fallback.counters.gaussian_draw_count == 2,
        "GPU Gaussian preparation fallback lost the reference raster");
    request.shaders.gaussian_prepare_compute.clear();
    request.gpu_driven_gaussian_preparation =
        merlin::vulkan::GpuDrivenGaussianPreparationMode::Require;

    // A missing sort artifact keeps GPU preparation and falls back only the
    // sort when preferred, and fails the frame when required.
    request.shaders.gaussian_sort_directory =
        shader_dir / "missing-gaussian-sort";
    const auto sort_fallback = renderer->Resolve(renderer->Submit(request));
    Require(sort_fallback.counters.gaussian_gpu_sort_fallback_count == 1 &&
                sort_fallback.counters.gaussian_gpu_sort_dispatch_count == 0 &&
                sort_fallback.counters.gaussian_gpu_preparation_dispatch_count ==
                    1,
        "missing Gaussian sort artifacts did not fall back independently");
    Require(sort_fallback.cpu_timings.gaussian_gpu_sort_ns == 0,
        "missing GPU Gaussian sort artifacts retained a device timing");
    request.gpu_driven_gaussian_sort =
        merlin::vulkan::GpuDrivenGaussianSortMode::Require;
    Require(ThrowsRendererError(
                [&] { (void)renderer->Submit(request); },
                merlin::vulkan::RendererErrorCode::InvalidRequest),
        "required Gaussian sort accepted missing artifacts");
    request.shaders.gaussian_sort_directory.clear();
    // The sort consumes GPU-prepared records, so it cannot be required alone.
    request.gpu_driven_gaussian_preparation =
        merlin::vulkan::GpuDrivenGaussianPreparationMode::Disabled;
    Require(ThrowsRendererError(
                [&] { (void)renderer->Submit(request); },
                merlin::vulkan::RendererErrorCode::Unsupported),
        "required Gaussian sort accepted a CPU-prepared frame");
    request.gpu_driven_gaussian_preparation =
        merlin::vulkan::GpuDrivenGaussianPreparationMode::Require;

    // The Gaussian vertex artifact is itself optional, so an omitted compute
    // artifact must resolve beside the packaged artifact that vertex path
    // resolves to rather than beside an empty path.
    const auto authored_gaussian_vertex = request.shaders.gaussian_vertex;
    request.shaders.gaussian_vertex.clear();
    const auto resolved = renderer->Resolve(renderer->Submit(request));
    Require(resolved.counters.gaussian_gpu_preparation_dispatch_count == 1 &&
                resolved.counters.gaussian_gpu_preparation_fallback_count == 0,
        "omitted Gaussian artifacts did not resolve the packaged compute "
        "artifact");
    RequireVerifiedSort(resolved,
        "omitted Gaussian artifacts did not resolve the "
        "packaged sort kernels");
    request.shaders.gaussian_vertex = authored_gaussian_vertex;

    // Z depth and camera distance are incomparable key domains, so a frame
    // authoring both re-keys every visible resource to Z depth. The GPU
    // dispatch has to adopt that same frame-wide policy: per-record authored
    // modes would feed the later global sort two different key domains.
    auto distance_descriptor = world.Get(gaussian_handle);
    distance_descriptor.label = "distance-sorted-splat";
    distance_descriptor.sorting_mode =
        merlin::GaussianSortingMode::CameraDistance;
    const auto distance_gaussian =
        world.CreateGaussian(std::move(distance_descriptor));
    extractor.Apply(world, world.Commit());
    request.snapshot = extractor.snapshot();
    const auto mixed_sorting = renderer->Resolve(renderer->Submit(request));
    Require(mixed_sorting.counters.gaussian_sorting_policy_fallback_count == 2,
        "mixed authored sorting policy was not diagnosed per resource");
    Require(mixed_sorting.counters.gaussian_gpu_preparation_dispatch_count == 2,
        "mixed sorting policy suppressed a GPU preparation dispatch");
    Require(mixed_sorting.counters.gaussian_gpu_preparation_visible_count ==
                mixed_sorting.counters.gaussian_visible_count,
        "mixed sorting policy diverged from the CPU reference partition");
    // Both resources share key domains and tie on depth; ascending resource
    // identity has to break those ties exactly as the CPU reference does.
    RequireVerifiedSort(mixed_sorting,
        "mixed sorting policy diverged from the reference "
        "order");
    world.Remove(distance_gaussian);
    extractor.Apply(world, world.Commit());
    request.snapshot = extractor.snapshot();

    auto secondary_descriptor = world.Get(gaussian_handle);
    secondary_descriptor.label = "fully-culled-splat";
    secondary_descriptor.positions.resize(1);
    secondary_descriptor.covariances.resize(1);
    secondary_descriptor.opacities = {0.0F};
    secondary_descriptor.spherical_harmonics_coefficients.resize(1);
    const auto secondary_gaussian =
        world.CreateGaussian(std::move(secondary_descriptor));
    extractor.Apply(world, world.Commit());
    request.snapshot = extractor.snapshot();
    const auto multiple_resources =
        renderer->Resolve(renderer->Submit(request));
    Require(
        multiple_resources.counters.gaussian_gpu_preparation_dispatch_count ==
                2 &&
            multiple_resources.counters
                    .gaussian_gpu_preparation_candidate_count == 3 &&
            multiple_resources.counters.gaussian_gpu_preparation_visible_count ==
                2 &&
            multiple_resources.counters
                    .gaussian_gpu_preparation_opacity_culled_count == 1,
        "per-resource GPU Gaussian dispatch did not preserve partitions");
    RequireVerifiedSort(multiple_resources,
        "a fully culled resource disturbed the GPU sort");
    world.Remove(secondary_gaussian);
    extractor.Apply(world, world.Commit());
    request.snapshot = extractor.snapshot();

    auto edited = world.Get(gaussian_handle);
    edited.spherical_harmonics_coefficients[1] = {0.0F, 1.0F, 0.0F};
    world.UpdateGaussian(
        gaussian_handle, std::move(edited),
        merlin::ChangeAspect::GaussianRadiance,
        std::vector<merlin::ElementRange>{{1, 1}});
    extractor.Apply(world, world.Commit());
    request.snapshot = extractor.snapshot();
    const auto partial = renderer->Resolve(renderer->Submit(request));
    Require(partial.counters.gaussian_upload_bytes == 52,
        "single-particle edit did not use a changed-range GPU upload");
    Require(partial.counters.gaussian_attribute_upload_bytes == 12,
        "single-particle SH edit did not retain raw range-only upload");
    Require(partial.counters.gaussian_attribute_copy_range_count == 1,
        "single-particle SH edit recorded more than one raw range");
    Require(partial.counters.gaussian_attribute_generation_count == 1,
        "single-particle edit did not advance residency generation");
    Require(partial.counters.gaussian_draw_count == 2,
        "partially updated Gaussian stream lost a procedural draw");

    edited = world.Get(gaussian_handle);
    edited.opacities[0] = 0.7F;
    world.UpdateGaussian(
        gaussian_handle, std::move(edited),
        merlin::ChangeAspect::GaussianOpacity,
        std::vector<merlin::ElementRange>{{0, 1}});
    extractor.Apply(world, world.Commit());
    request.snapshot = extractor.snapshot();
    const auto opacity = renderer->Resolve(renderer->Submit(request));
    Require(opacity.counters.gaussian_attribute_upload_bytes == sizeof(float),
        "single-particle opacity edit did not upload one raw scalar");
    Require(opacity.counters.gaussian_attribute_copy_range_count == 1,
        "single-particle opacity edit recorded extra raw ranges");

    edited = world.Get(gaussian_handle);
    edited.transform.values[12] = 0.05F;
    world.UpdateGaussian(gaussian_handle, std::move(edited),
        merlin::ChangeAspect::Transform);
    extractor.Apply(world, world.Commit());
    request.snapshot = extractor.snapshot();
    const auto transformed = renderer->Resolve(renderer->Submit(request));
    Require(transformed.counters.gaussian_attribute_upload_bytes == 0,
        "transform-only Gaussian edit re-uploaded source attributes");
    Require(transformed.counters.gaussian_attribute_generation_count == 1,
        "transform-only Gaussian edit did not advance record generation");

    edited = world.Get(gaussian_handle);
    edited.visible = false;
    world.UpdateGaussian(gaussian_handle, std::move(edited),
        merlin::ChangeAspect::Visibility);
    extractor.Apply(world, world.Commit());
    request.snapshot = extractor.snapshot();
    const auto hidden = renderer->Resolve(renderer->Submit(request));
    Require(hidden.counters.gaussian_visible_count == 0,
        "hidden Gaussian resource still reached the prepared stream");
    Require(hidden.counters.gaussian_gpu_preparation_dispatch_count == 0 &&
                hidden.counters.gaussian_gpu_preparation_candidate_count == 0,
        "hidden Gaussian resource still reached GPU preparation");
    Require(hidden.counters.gaussian_attribute_upload_bytes == 0,
        "visibility-only Gaussian edit re-uploaded source attributes");
    Require(hidden.counters.gaussian_attribute_generation_count == 1,
        "visibility-only Gaussian edit did not advance record generation");
    // Enough candidates for several sort workgroups, a two-level scan, and
    // two low-word passes. Three shared depths per resource make most keys
    // tie, so the order also proves the resource/particle tie break. This
    // runs last because it replaces the frame's prepared instance stream.
    std::vector<merlin::GaussianHandle> scale_gaussians;
    for (const std::uint32_t count : {2500U, 700U}) {
      merlin::GaussianDescriptor scale;
      scale.label = "sort-scale-splats";
      for (std::uint32_t particle = 0; particle < count; ++particle) {
        const auto column = static_cast<float>(particle % 50U);
        const auto row = static_cast<float>((particle / 50U) % 50U);
        scale.positions.push_back({-0.6F + column * 0.024F,
            -0.6F + row * 0.024F,
            0.3F + 0.2F * static_cast<float>(
                              (particle * 7U) % 3U)});
        scale.covariances.push_back(
            {0.0001F, 0.0F, 0.0F, 0.0001F, 0.0F, 0.0001F});
        scale.opacities.push_back(0.5F);
        scale.spherical_harmonics_coefficients.push_back(
            {0.2F, 0.4F, 0.6F});
      }
      scale_gaussians.push_back(world.CreateGaussian(std::move(scale)));
    }
    extractor.Apply(world, world.Commit());
    request.snapshot = extractor.snapshot();
    const auto scaled = renderer->Resolve(renderer->Submit(request));
    Require(scaled.counters.gaussian_gpu_preparation_visible_count == 3200,
        "sort-scale fixture lost visible Gaussians");
    RequireVerifiedSort(scaled, "multi-workgroup GPU sort diverged");
    // 3200 candidates pad to 3328 keys: 13 workgroups, a two-level scan, and
    // two low-word passes. Each of six passes records histogram, two scans,
    // one add, and scatter between two key segments and one verification.
    Require(scaled.counters.gaussian_gpu_sort_key_count == 3328 &&
                scaled.counters.gaussian_gpu_sort_pass_count == 6 &&
                scaled.counters.gaussian_gpu_sort_dispatch_count == 33,
        "multi-workgroup GPU sort did not follow its dispatch plan");
    // Thousands of overlapping splats that tie on depth make the composite
    // order-sensitive, so the indirect raster must follow the reference order
    // across workgroups, resources, and ties.
    request.gpu_driven_gaussian_raster =
        merlin::vulkan::GpuDrivenGaussianRasterMode::Require;
    const auto scaled_raster = renderer->Resolve(renderer->Submit(request));
    RequireVerifiedSort(scaled_raster, "scaled GPU raster lost its GPU sort");
    Require(scaled_raster.counters.gaussian_gpu_raster_instance_count ==
                    3200 &&
                scaled_raster.counters.gaussian_upload_bytes == 0,
        "scaled GPU raster did not draw every sorted record");
    const auto scaled_difference = Compare(scaled, scaled_raster);
    Report("scaled sorted-stream raster vs CPU reference", scaled_difference);
    Require(scaled_difference.max_color_channel <= 1U,
        "scaled GPU raster color diverged from the CPU reference");
    Require(scaled_difference.depth_pixels == 0 &&
                scaled_difference.prim_id_pixels == 0 &&
                scaled_difference.instance_id_pixels == 0,
        "scaled GPU raster depth or IDs diverged from the CPU reference");
    request.gpu_driven_gaussian_raster =
        merlin::vulkan::GpuDrivenGaussianRasterMode::Disabled;
    for (const auto handle : scale_gaussians) {
      world.Remove(handle);
    }
    extractor.Apply(world, world.Commit());
    request.snapshot = extractor.snapshot();

    Require(renderer->statistics().validation_messages == 0,
        "Gaussian rasterization produced Vulkan validation diagnostics");
  } catch (const std::exception& error) {
    std::cerr << "failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
