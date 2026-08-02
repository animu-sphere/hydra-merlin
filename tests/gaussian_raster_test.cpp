// Exercises the v0.14.1 Vulkan Gaussian MVP end to end: prepared-stream
// upload, procedural ellipse rasterization, alpha compositing, Mesh depth
// composition, ID output, and steady-state frame-local upload reuse.

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

    const auto first = renderer->Resolve(renderer->Submit(request));
    Require(first.counters.gaussian_visible_count == 2,
            "prepared stream did not retain the visible Gaussians");
    Require(first.counters.gaussian_draw_count == 1,
            "Gaussian stream was not submitted as one procedural draw");
    Require(first.counters.gaussian_upload_bytes == 104,
            "Gaussian GPU instance upload size drifted");
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
    Require(steady.counters.gaussian_draw_count == 1,
            "static Gaussian frame lost its procedural draw");

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
    Require(partial.counters.gaussian_draw_count == 1,
            "partially updated Gaussian stream was not drawn");
    Require(renderer->statistics().validation_messages == 0,
            "Gaussian rasterization produced Vulkan validation diagnostics");
  } catch (const std::exception& error) {
    std::cerr << "failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
