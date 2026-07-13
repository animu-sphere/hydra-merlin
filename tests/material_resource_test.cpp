// Exercises the v0.5.0 MaterialIR, texture/sampler revision, shader variant,
// and structured fallback contract end to end. Requires a Vulkan device and
// exits 77 when the backend is unavailable.

#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/vulkan/renderer.hpp>

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <vector>

namespace {

std::size_t CoveredPixels(const merlin::vulkan::RenderResult& result) {
  std::size_t covered{};
  for (const auto depth : result.depth.pixels) {
    if (depth < 1.0F) {
      ++covered;
    }
  }
  return covered;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: material_resource_test SHADER_DIR\n";
    return 1;
  }
  const std::filesystem::path shader_dir = argv[1];
  const merlin::vulkan::ShaderPaths shaders{
      shader_dir / "triangle.vert.spv", shader_dir / "triangle.frag.spv"};

  std::optional<merlin::vulkan::Renderer> renderer;
  try {
    renderer.emplace();
  } catch (const std::exception& error) {
    std::cerr << "skip: Vulkan renderer unavailable: " << error.what() << '\n';
    return 77;
  }

  merlin::RenderWorld world;
  merlin::extraction::SceneExtractor extractor;
  const auto render = [&] {
    extractor.Apply(world, world.Commit());
    return renderer->Render(*extractor.snapshot(), 64, 64, shaders);
  };

  merlin::MeshDescriptor mesh;
  mesh.label = "textured-quad";
  mesh.positions = {{-0.8F, -0.8F, 0.2F}, {0.8F, -0.8F, 0.2F},
                    {0.8F, 0.8F, 0.2F}, {-0.8F, 0.8F, 0.2F}};
  mesh.normals.assign(4, {0.0F, 0.0F, 1.0F});
  mesh.texcoords = {{0.0F, 0.0F}, {1.0F, 0.0F},
                    {1.0F, 1.0F}, {0.0F, 1.0F}};
  mesh.indices = {0, 1, 2, 0, 2, 3};
  const auto mesh_handle = world.CreateMesh(mesh);

  merlin::TextureDescriptor texture;
  texture.label = "rgba-checker";
  texture.width = 2;
  texture.height = 2;
  texture.pixels = {255, 0,   0,   255, 0,   255, 0,   255,
                    0,   0,   255, 255, 255, 255, 255, 255};
  const auto texture_handle = world.CreateTexture(texture);

  merlin::SamplerDescriptor sampler;
  sampler.label = "nearest-repeat";
  sampler.min_filter = merlin::FilterMode::Nearest;
  sampler.mag_filter = merlin::FilterMode::Nearest;
  const auto sampler_handle = world.CreateSampler(sampler);

  merlin::MaterialDescriptor material;
  material.label = "textured";
  material.features = merlin::MaterialFeature::BaseColorTexture;
  material.base_color_texture =
      merlin::TextureBinding{texture_handle, sampler_handle, 0};
  const auto material_handle = world.CreateMaterial(material);

  merlin::InstanceDescriptor instance;
  instance.mesh = mesh_handle;
  instance.material = material_handle;
  world.CreateInstance(instance);

  const auto first = render();
  const auto geometry_bytes =
      mesh.positions.size() * sizeof(merlin::extraction::DrawVertex) +
      mesh.indices.size() * sizeof(std::uint32_t);
  assert(first.counters.upload_bytes == geometry_bytes + texture.pixels.size());
  assert(first.counters.texture_cache_misses == 1);
  assert(first.counters.sampler_cache_misses == 1);
  assert(first.counters.pipeline_creation_count == 1);
  const auto opaque_coverage = CoveredPixels(first);
  assert(opaque_coverage > 1000);

  // Parameter values travel through push constants and never enter the
  // feature/state pipeline key.
  material.parameters.base_color = {0.5F, 0.75F, 1.0F, 1.0F};
  world.UpdateMaterial(material_handle, material,
                       merlin::ChangeAspect::MaterialParameters);
  const auto value_edit = render();
  assert(value_edit.counters.upload_bytes == 0);
  assert(value_edit.counters.pipeline_creation_count == 0);
  assert(value_edit.counters.texture_cache_hits == 1);
  assert(value_edit.counters.sampler_cache_hits == 1);

  // A feature edit selects a distinct cached variant without touching texture
  // or geometry residency.
  material.features = merlin::MaterialFeature::None;
  world.UpdateMaterial(material_handle, material,
                       merlin::ChangeAspect::MaterialFeatures);
  const auto feature_edit = render();
  assert(feature_edit.counters.upload_bytes == 0);
  assert(feature_edit.counters.pipeline_creation_count == 1);

  material.features = merlin::MaterialFeature::BaseColorTexture;
  world.UpdateMaterial(material_handle, material,
                       merlin::ChangeAspect::MaterialFeatures);
  const auto cached_variant = render();
  assert(cached_variant.counters.pipeline_creation_count == 0);

  // Same-handle texture and sampler revisions replace only their corresponding
  // GPU resources. Material and geometry caches remain reusable.
  texture.pixels = {255, 255, 0, 255, 255, 0, 255, 255,
                    255, 255, 0, 255, 255, 0, 255, 255};
  world.UpdateTexture(texture_handle, texture);
  const auto texture_edit = render();
  assert(texture_edit.counters.upload_bytes == texture.pixels.size());
  assert(texture_edit.counters.texture_cache_misses == 1);
  assert(texture_edit.counters.sampler_cache_hits == 1);
  assert(texture_edit.counters.pipeline_creation_count == 0);

  sampler.min_filter = merlin::FilterMode::Linear;
  sampler.mag_filter = merlin::FilterMode::Linear;
  world.UpdateSampler(sampler_handle, sampler);
  const auto sampler_edit = render();
  assert(sampler_edit.counters.upload_bytes == 0);
  assert(sampler_edit.counters.texture_cache_hits == 1);
  assert(sampler_edit.counters.sampler_cache_misses == 1);

  // Alpha mask is a state variant: transparent texels discard every AOV write.
  texture.pixels = {255, 255, 255, 255, 255, 255, 255, 0,
                    255, 255, 255, 255, 255, 255, 255, 0};
  world.UpdateTexture(texture_handle, texture);
  material.alpha_mode = merlin::AlphaMode::Masked;
  material.parameters.alpha_cutoff = 0.5F;
  world.UpdateMaterial(material_handle, material,
                       merlin::ChangeAspect::MaterialFeatures |
                           merlin::ChangeAspect::MaterialParameters);
  const auto masked = render();
  assert(masked.counters.pipeline_creation_count == 1);
  assert(CoveredPixels(masked) < opaque_coverage);

  // Back-face culling is the default state; the double-sided feature selects
  // a variant that disables it without changing the material values.
  mesh.indices = {0, 2, 1, 0, 3, 2};
  world.UpdateMesh(mesh_handle, mesh, merlin::ChangeAspect::Topology);
  material.alpha_mode = merlin::AlphaMode::Opaque;
  world.UpdateMaterial(material_handle, material,
                       merlin::ChangeAspect::MaterialFeatures);
  const auto culled = render();
  assert(CoveredPixels(culled) == 0);
  material.double_sided = true;
  world.UpdateMaterial(material_handle, material,
                       merlin::ChangeAspect::MaterialFeatures);
  const auto double_sided = render();
  assert(double_sided.counters.pipeline_creation_count == 1);
  assert(CoveredPixels(double_sided) == opaque_coverage);

  // Removing a still-bound texture is normalized into a structured constant-
  // color fallback; stale resource handles never reach the backend.
  world.Remove(texture_handle);
  const auto fallback = render();
  assert(!extractor.snapshot()->material_fallbacks.empty());
  assert(extractor.snapshot()->material_fallbacks.front().code ==
         merlin::extraction::MaterialFallbackCode::MissingTexture);
  assert(fallback.counters.texture_cache_misses == 0);
  assert(CoveredPixels(fallback) == opaque_coverage);

  std::cout << "MaterialIR texture/sampler and variant contract verified\n";
  return 0;
}
