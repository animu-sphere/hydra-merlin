// Exercises the Vulkan Gaussian path end to end: GPU projection/culling
// dispatch and counter readback beside the CPU-sorted reference raster,
// prepared-stream upload, procedural ellipse rasterization, alpha compositing,
// Mesh depth composition, ID output, and steady-state frame-local reuse.

#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/vulkan/renderer.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>

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

}  // namespace

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

    request.gpu_driven_gaussian_preparation =
        merlin::vulkan::GpuDrivenGaussianPreparationMode::Prefer;
    request.shaders.gaussian_prepare_compute =
        shader_dir / "missing-gaussian-prepare.comp.spv";
    const auto fallback = renderer->Resolve(renderer->Submit(request));
    Require(fallback.counters.gaussian_gpu_preparation_fallback_count == 1 &&
                fallback.counters.gaussian_gpu_preparation_dispatch_count == 0,
            "preferred GPU Gaussian preparation did not retain CPU fallback");
    Require(fallback.counters.gaussian_visible_count == 2 &&
                fallback.counters.gaussian_draw_count == 2,
            "GPU Gaussian preparation fallback lost the reference raster");
    request.shaders.gaussian_prepare_compute.clear();
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
    Require(renderer->statistics().validation_messages == 0,
            "Gaussian rasterization produced Vulkan validation diagnostics");
  } catch (const std::exception& error) {
    std::cerr << "failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
