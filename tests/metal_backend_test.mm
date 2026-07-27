#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/metal/backend.hpp>

#import <QuartzCore/CAMetalLayer.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

std::uint8_t CenterChannel(const merlin::render::RenderResult &result,
                           std::uint32_t channel) {
  const auto x = result.color.product.width / 2U;
  const auto y = result.color.product.height / 2U;
  return result.color
      .pixels[static_cast<std::size_t>(y) * result.color.row_pitch_bytes +
              static_cast<std::size_t>(x) * 4U + channel];
}

merlin::render::RenderResult
Render(merlin::render::Backend &backend,
       std::shared_ptr<const merlin::extraction::FrameSnapshot> snapshot) {
  merlin::render::RenderRequest request;
  request.snapshot = std::move(snapshot);
  request.width = 64;
  request.height = 64;
  request.products = {
      {merlin::Aov::Color, true},
      {merlin::Aov::Depth, true},
      {merlin::Aov::PrimId, true},
      {merlin::Aov::InstanceId, true},
  };
  const auto token = backend.Submit(request);
  assert(token);
  return backend.Resolve(token);
}

} // namespace

int main() {
  merlin::metal::BackendOptions backend_options;
  backend_options.texture_capacity = 1;
  backend_options.sampler_capacity = 1;
  backend_options.heap_capacity_bytes = 8U * 1024U * 1024U;
  merlin::metal::BackendFactory factory(backend_options);
  const auto availability = factory.availability();
  if (!availability.available) {
    std::cerr << "skip: " << availability.detail << '\n';
    return 77;
  }

  merlin::render::BackendCreateInfo create_info;
  create_info.enable_validation = true;
  create_info.frames_in_flight = 3;
  std::array<merlin::render::BackendFactory *, 1> factories{&factory};
  merlin::render::BackendSelection selection;
  auto backend =
      merlin::render::CreateBackend(create_info, factories, &selection);
  assert(selection.selected == merlin::render::BackendKind::Metal);
  assert(selection.reason == "platform preference");
  assert(backend->capabilities().backend == merlin::render::BackendKind::Metal);
  assert(backend->capabilities().cpu_readback);
  assert(!backend->capabilities().external_presentation);

  merlin::RenderWorld world;
  merlin::extraction::SceneExtractor extractor;
  merlin::MeshDescriptor mesh;
  mesh.label = "metal-quad";
  mesh.positions = {{-0.8F, -0.8F, 0.2F},
                    {0.8F, -0.8F, 0.2F},
                    {0.8F, 0.8F, 0.2F},
                    {-0.8F, 0.8F, 0.2F}};
  mesh.normals.assign(4, {0.0F, 0.0F, 1.0F});
  mesh.texcoords = {{0.0F, 1.0F}, {1.0F, 1.0F}, {1.0F, 0.0F}, {0.0F, 0.0F}};
  mesh.indices = {0, 1, 2, 0, 2, 3};
  const auto mesh_handle = world.CreateMesh(mesh);

  merlin::TextureDescriptor texture;
  texture.label = "red";
  texture.width = 1;
  texture.height = 1;
  texture.pixels = {255, 32, 16, 255};
  const auto texture_handle = world.CreateTexture(texture);
  merlin::SamplerDescriptor sampler;
  sampler.label = "nearest";
  sampler.min_filter = merlin::FilterMode::Nearest;
  sampler.mag_filter = merlin::FilterMode::Nearest;
  const auto sampler_handle = world.CreateSampler(sampler);

  merlin::MaterialDescriptor material;
  material.label = "unlit-textured";
  material.parameters.base_color = {0.5F, 1.0F, 1.0F, 1.0F};
  material.features = merlin::MaterialFeature::BaseColorTexture;
  material.base_color_texture =
      merlin::TextureBinding{texture_handle, sampler_handle, 0};
  const auto material_handle = world.CreateMaterial(material);
  merlin::InstanceDescriptor instance;
  instance.mesh = mesh_handle;
  instance.material = material_handle;
  const auto instance_handle = world.CreateInstance(instance);

  extractor.Apply(world, world.Commit());
  auto result = Render(*backend, extractor.snapshot());
  assert(result.rendered_aovs.size() == 4);
  assert(result.cpu_readback_aovs.size() == 4);
  assert(result.color.pixels.size() == 64U * 64U * 4U);
  assert(CenterChannel(result, 0) >= 126 && CenterChannel(result, 0) <= 129);
  assert(CenterChannel(result, 1) >= 31 && CenterChannel(result, 1) <= 33);
  assert(CenterChannel(result, 2) >= 15 && CenterChannel(result, 2) <= 17);
  const auto center = 32U * 64U + 32U;
  assert(result.depth.pixels[center] < 1.0F);
  assert(result.prim_id.pixels[center] ==
         static_cast<std::uint32_t>(mesh_handle.value()));
  assert(result.instance_id.pixels[center] ==
         static_cast<std::uint32_t>(instance_handle.value()));

  // Keep a pair of independently changing revisions whose old XOR cache key
  // collides. Metal must compare both revisions instead of treating the
  // collision as a geometry cache hit.
  for (int update = 0; update < 3; ++update) {
    mesh.positions[0].x += 0.01F;
    world.UpdateMesh(mesh_handle, mesh, merlin::ChangeAspect::Points);
  }
  extractor.Apply(world, world.Commit());
  const auto collision_baseline = extractor.snapshot()->geometries.front();
  const auto collision_baseline_result =
      Render(*backend, extractor.snapshot());
  assert(collision_baseline_result.telemetry.geometry_cache_misses == 1);

  for (int update = 0; update < 2; ++update) {
    mesh.positions[0].x += 0.01F;
    world.UpdateMesh(mesh_handle, mesh, merlin::ChangeAspect::Points);
  }
  world.UpdateMesh(mesh_handle, mesh, merlin::ChangeAspect::Topology);
  extractor.Apply(world, world.Commit());
  mesh.positions[0].x += 0.01F;
  world.UpdateMesh(mesh_handle, mesh, merlin::ChangeAspect::Points);
  extractor.Apply(world, world.Commit());
  const auto collision_update = extractor.snapshot()->geometries.front();
  assert(collision_baseline.vertex_revision == 4);
  assert(collision_baseline.index_revision == 1);
  assert(collision_update.vertex_revision == 8);
  assert(collision_update.index_revision == 7);
  assert((collision_baseline.vertex_revision ^
          (collision_baseline.index_revision << 1U)) ==
         (collision_update.vertex_revision ^
          (collision_update.index_revision << 1U)));
  const auto collision_result = Render(*backend, extractor.snapshot());
  assert(collision_result.telemetry.geometry_cache_misses == 1);

  const auto submit = [&] {
    merlin::render::RenderRequest request;
    request.snapshot = extractor.snapshot();
    request.width = 64;
    request.height = 64;
    request.products = {{merlin::Aov::Color, false}};
    return backend->Submit(request);
  };
  const auto first_pending = submit();
  const auto middle_pending = submit();
  const auto last_pending = submit();
  (void)backend->Resolve(middle_pending);
  // Resolving the middle token frees its context. Submission must find it even
  // though the round-robin cursor first encounters an unresolved context.
  const auto replacement_pending = submit();
  (void)backend->Resolve(first_pending);
  (void)backend->Resolve(last_pending);
  (void)backend->Resolve(replacement_pending);

  // Every frame context is now seeded. A stable snapshot performs no target
  // allocation or argument-table update when a context is reused.
  const auto before_static =
      dynamic_cast<merlin::metal::Backend &>(*backend).metal_statistics();
  const auto static_result = Render(*backend, extractor.snapshot());
  const auto after_static =
      dynamic_cast<merlin::metal::Backend &>(*backend).metal_statistics();
  assert(static_result.telemetry.allocation_count == 0);
  assert(after_static.argument_buffer_update_count ==
         before_static.argument_buffer_update_count);

  const auto texture_slot = after_static.texture_slots.in_use;
  texture.pixels = {32, 255, 64, 255};
  world.UpdateTexture(texture_handle, texture);
  extractor.Apply(world, world.Commit());
  const auto updated = Render(*backend, extractor.snapshot());
  const auto after_update =
      dynamic_cast<merlin::metal::Backend &>(*backend).metal_statistics();
  assert(after_update.texture_slots.in_use == texture_slot);
  assert(CenterChannel(updated, 0) >= 15 && CenterChannel(updated, 0) <= 17);
  assert(CenterChannel(updated, 1) >= 254);
  assert(after_update.heap_peak_resident_bytes >=
         after_update.heap_resident_bytes);

  texture.pixels = {255, 255, 255, 0};
  world.UpdateTexture(texture_handle, texture);
  material.alpha_mode = merlin::AlphaMode::Masked;
  world.UpdateMaterial(material_handle, material,
                       merlin::ChangeAspect::MaterialFeatures);
  extractor.Apply(world, world.Commit());
  const auto masked = Render(*backend, extractor.snapshot());
  assert(CenterChannel(masked, 0) >= 4 && CenterChannel(masked, 0) <= 6);
  assert(masked.depth.pixels[center] == 1.0F);
  assert(masked.prim_id.pixels[center] ==
         std::numeric_limits<std::uint32_t>::max());
  assert(masked.instance_id.pixels[center] ==
         std::numeric_limits<std::uint32_t>::max());

  merlin::TextureDescriptor extra_texture;
  extra_texture.label = "capacity-exhaustion";
  extra_texture.width = 1;
  extra_texture.height = 1;
  extra_texture.pixels = {1, 2, 3, 255};
  const auto extra_texture_handle = world.CreateTexture(extra_texture);
  extractor.Apply(world, world.Commit());
  bool capacity_rejected{};
  try {
    (void)Render(*backend, extractor.snapshot());
  } catch (const merlin::render::RendererError &error) {
    capacity_rejected =
        error.code() == merlin::render::RendererErrorCode::ResourceExhausted &&
        error.operation() == "allocate Metal texture slot" &&
        error.detail().find("capacity=1") != std::string::npos;
  }
  assert(capacity_rejected);
  world.Remove(extra_texture_handle);

  merlin::TextureDescriptor replacement_texture = texture;
  replacement_texture.label = "replacement";
  replacement_texture.pixels = {64, 128, 255, 255};
  const auto replacement_texture_handle =
      world.CreateTexture(replacement_texture);
  merlin::SamplerDescriptor replacement_sampler = sampler;
  replacement_sampler.label = "replacement-nearest";
  const auto replacement_sampler_handle =
      world.CreateSampler(replacement_sampler);
  material.alpha_mode = merlin::AlphaMode::Opaque;
  material.base_color_texture = merlin::TextureBinding{
      replacement_texture_handle, replacement_sampler_handle, 0};
  world.UpdateMaterial(material_handle, material,
                       merlin::ChangeAspect::MaterialFeatures |
                           merlin::ChangeAspect::MaterialResources);
  world.Remove(texture_handle);
  world.Remove(sampler_handle);
  extractor.Apply(world, world.Commit());
  const auto replacement = Render(*backend, extractor.snapshot());
  assert(CenterChannel(replacement, 2) >= 254);
  const auto replacement_stats =
      dynamic_cast<merlin::metal::Backend &>(*backend).metal_statistics();
  assert(replacement_stats.texture_slots.in_use == 1);
  assert(replacement_stats.texture_slots.reuses >= 1);
  assert(replacement_stats.sampler_slots.in_use == 1);
  assert(replacement_stats.sampler_slots.reuses >= 1);
  assert(backend->statistics().validation_messages == 0);

  CAMetalLayer *layer = [CAMetalLayer layer];
  merlin::metal::BackendOptions presentation_options;
  merlin::metal::PresentationOptions presentation;
  presentation.layer =
      reinterpret_cast<std::uintptr_t>((__bridge void *)layer);
  presentation.color_space =
      merlin::metal::PresentationColorSpace::DisplayP3;
  presentation_options.presentation = presentation;
  merlin::metal::Backend presentation_backend(create_info,
                                               presentation_options);
  assert(presentation_backend.capabilities().external_presentation);
  const auto presentation_target =
      presentation_backend.default_presentation_target();
  assert(presentation_target);
  presentation_backend.ResizePresentationTarget(*presentation_target, 64, 64);
  presentation_backend.ResizePresentationTarget(*presentation_target, 128, 64);
  assert(presentation_backend.statistics().presentation_recreates == 1);

  bool foreign_target_rejected{};
  try {
    presentation_backend.ResizePresentationTarget(
        merlin::render::PresentationTarget(presentation_target->owner() + 1,
                                           presentation_target->value()),
        64, 64);
  } catch (const merlin::render::RendererError &error) {
    foreign_target_rejected =
        error.code() == merlin::render::RendererErrorCode::InvalidRequest &&
        error.operation() == "resize Metal presentation";
  }
  assert(foreign_target_rejected);

  presentation.dynamic_range =
      merlin::metal::PresentationDynamicRange::Extended;
  presentation_options.presentation = presentation;
  bool hdr_rejected{};
  try {
    merlin::metal::Backend unsupported_hdr(create_info,
                                           presentation_options);
  } catch (const merlin::render::RendererError &error) {
    hdr_rejected =
        error.code() == merlin::render::RendererErrorCode::Unsupported &&
        error.operation() == "create Metal presentation";
  }
  assert(hdr_rejected);
  return 0;
}
