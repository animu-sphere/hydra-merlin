// Verifies the Vulkan-facing bindless residency and shader path: stable slots,
// dirty-only writes, and completion-safe replacement. Conventional Forward is
// covered separately as the image reference/fallback. Requires a descriptor-
// indexing Vulkan device and exits 77 when that path is unavailable.

#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/vulkan/renderer.hpp>

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: bindless_residency_test SHADER_DIR\n";
    return 1;
  }
  const std::filesystem::path shader_dir = argv[1];
  const merlin::vulkan::ShaderPaths shaders{
      shader_dir / "triangle.vert.spv", shader_dir / "triangle.frag.spv",
      shader_dir / "triangle.bindless.vert.spv",
      shader_dir / "triangle.bindless.frag.spv"};

  std::optional<merlin::vulkan::Renderer> renderer;
  try {
    renderer.emplace(merlin::vulkan::RendererOptions{true, 2});
  } catch (const std::exception& error) {
    std::cerr << "skip: Vulkan renderer unavailable: " << error.what() << '\n';
    return 77;
  }
  if (!renderer->statistics().bindless_resource_tables) {
    std::cerr << "skip: descriptor-indexing resource tables unavailable\n";
    return 77;
  }

  merlin::RenderWorld world;
  merlin::extraction::SceneExtractor extractor;

  merlin::MeshDescriptor mesh;
  mesh.positions = {{-0.7F, -0.7F, 0.0F}, {0.7F, -0.7F, 0.0F},
                    {0.0F, 0.7F, 0.0F}};
  mesh.normals.assign(3, {0.0F, 0.0F, 1.0F});
  mesh.texcoords = {{0.0F, 0.0F}, {1.0F, 0.0F}, {0.5F, 1.0F}};
  mesh.indices = {0, 1, 2};
  const auto mesh_handle = world.CreateMesh(mesh);

  merlin::TextureDescriptor texture;
  texture.width = 1;
  texture.height = 1;
  texture.pixels = {255U, 0U, 0U, 255U};
  const auto texture_handle = world.CreateTexture(texture);

  merlin::SamplerDescriptor sampler;
  sampler.min_filter = merlin::FilterMode::Nearest;
  sampler.mag_filter = merlin::FilterMode::Nearest;
  const auto sampler_handle = world.CreateSampler(sampler);

  merlin::MaterialDescriptor material;
  material.features = merlin::MaterialFeature::BaseColorTexture;
  material.base_color_texture =
      merlin::TextureBinding{texture_handle, sampler_handle, 0};
  const auto material_handle = world.CreateMaterial(material);
  merlin::InstanceDescriptor instance;
  instance.mesh = mesh_handle;
  instance.material = material_handle;
  world.CreateInstance(instance);

  const auto apply = [&] {
    extractor.Apply(world, world.Commit());
    auto request = merlin::vulkan::RenderRequest{};
    request.snapshot = extractor.snapshot();
    request.width = 32;
    request.height = 32;
    request.shaders = shaders;
    return request;
  };

  auto first_request = apply();
  const auto first_token = renderer->Submit(first_request);
  auto first_stats = renderer->statistics();
  assert(first_stats.bindless_texture_slots.reserved_slots == 4);
  assert(first_stats.bindless_texture_slots.current_use == 5);
  assert(first_stats.bindless_samplers.slots.current_use == 1);
  assert(first_stats.bindless_samplers.unique_sampler_count == 1);

  // Replace both resources while frame 1 is still in flight. The old slots
  // enter retirement and the new frame receives distinct slots.
  texture.pixels = {0U, 255U, 0U, 255U};
  world.UpdateTexture(texture_handle, texture);
  sampler.min_filter = merlin::FilterMode::Linear;
  sampler.mag_filter = merlin::FilterMode::Linear;
  world.UpdateSampler(sampler_handle, sampler);
  auto second_request = apply();
  const auto second_token = renderer->Submit(second_request);
  const auto in_flight = renderer->statistics();
  assert(in_flight.bindless_texture_slots.current_use == 5);
  assert(in_flight.bindless_texture_slots.retiring_slots == 1);
  assert(in_flight.bindless_texture_slots.allocation_count == 2);
  assert(in_flight.bindless_texture_slots.reuse_count == 0);
  assert(in_flight.bindless_samplers.slots.current_use == 1);
  assert(in_flight.bindless_samplers.slots.retiring_slots == 1);
  assert(in_flight.bindless_samplers.slots.allocation_count == 2);

  const auto first = renderer->Resolve(first_token);
  assert(first.counters.bindless_sampled_image_descriptor_update_count == 6);
  assert(first.counters.bindless_sampler_descriptor_update_count == 2);

  // Resolving frame 1 makes its old slots collectable and immediately rewrites
  // those descriptor elements to fallback before destroying the old objects.
  // The next unchanged submission consequently performs no bindless work.
  const auto third_token = renderer->Submit(second_request);
  const auto second = renderer->Resolve(second_token);
  const auto third = renderer->Resolve(third_token);
  assert(second.counters.bindless_sampled_image_descriptor_update_count == 1);
  assert(second.counters.bindless_sampler_descriptor_update_count == 1);
  assert(third.counters.bindless_sampled_image_descriptor_update_count == 0);
  assert(third.counters.bindless_sampler_descriptor_update_count == 0);
  assert(third.counters.texture_reconcile_count == 0);
  assert(third.counters.sampler_reconcile_count == 0);

  auto collected = renderer->statistics();
  assert(collected.bindless_texture_slots.retiring_slots == 0);
  assert(collected.bindless_texture_slots.retirement_collection_count == 1);
  assert(collected.bindless_samplers.slots.retiring_slots == 0);
  assert(collected.bindless_samplers.slots.retirement_collection_count == 1);

  // A later replacement can now reuse the generation-advanced free slot.
  texture.pixels = {0U, 0U, 255U, 255U};
  world.UpdateTexture(texture_handle, texture);
  const auto fourth = renderer->Render(*apply().snapshot, 32, 32, shaders);
  assert(fourth.counters.bindless_sampled_image_descriptor_update_count == 2);
  const auto reused = renderer->statistics();
  assert(reused.bindless_texture_slots.reuse_count == 1);
  assert(reused.bindless_texture_slots.generation_mismatch_count == 0);
  assert(renderer->statistics().validation_messages == 0);

  std::cout << "Bindless Vulkan residency and retirement verified\n";
  return 0;
}
