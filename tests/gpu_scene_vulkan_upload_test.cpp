#include <merlin/render/gpu_scene_packing.hpp>
#include <merlin/vulkan/renderer.hpp>

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using merlin::extraction::DrawRecord;
using merlin::extraction::DrawVertex;
using merlin::extraction::FrameSnapshot;
using merlin::extraction::GeometryRecord;
using merlin::extraction::InstanceRecord;
using merlin::extraction::MaterialRecord;
using merlin::render::GpuGeometryPlacement;
using merlin::render::GpuInstanceIdentity;
using merlin::render::GpuMaterialBinding;
using merlin::render::GpuScenePackingCapacities;
using merlin::render::GpuScenePackingInputs;
using merlin::render::GpuScenePackingState;

std::shared_ptr<FrameSnapshot> MakeSnapshot() {
  auto snapshot = std::make_shared<FrameSnapshot>();
  snapshot->source_id = 41;
  snapshot->revision = 1;

  GeometryRecord geometry;
  geometry.mesh = 101;
  geometry.vertex_revision = 2;
  geometry.index_revision = 3;
  geometry.has_normals = true;
  geometry.vertices = std::make_shared<const std::vector<DrawVertex>>(
      std::vector<DrawVertex>{{{-0.5F, -0.5F, 0.2F}},
                              {{0.5F, -0.5F, 0.2F}},
                              {{0.0F, 0.5F, 0.2F}}});
  geometry.indices = std::make_shared<const std::vector<std::uint32_t>>(
      std::vector<std::uint32_t>{0, 1, 2});
  snapshot->geometries.assign({geometry});

  MaterialRecord material;
  material.material = 202;
  material.revision = 4;
  material.parameters.base_color = {0.2F, 0.6F, 0.9F, 1.0F};
  snapshot->materials.assign({material});

  InstanceRecord instance;
  instance.instance = 303;
  instance.mesh = geometry.mesh;
  instance.material = material.material;
  instance.revision = 5;
  snapshot->instances.assign({instance});

  DrawRecord draw;
  draw.geometry_index = 0;
  draw.material_index = 0;
  draw.instance_index = 0;
  draw.instance = instance.instance;
  draw.draw = 0x1234567887654321ULL;
  draw.revision = 6;
  snapshot->draws.assign({draw});
  return snapshot;
}

merlin::vulkan::ShaderPaths MakeShaders(const std::filesystem::path& root) {
  return {root / "triangle.vert.spv",
          root / "triangle.frag.spv",
          root / "triangle.bindless.vert.spv",
          root / "triangle.bindless.frag.spv",
          root / "environment.hdr",
          root / "gaussian.vert.spv",
          root / "gaussian.frag.spv",
          root / "gaussian-id.frag.spv",
          root / "gaussian-id.vert.spv"};
}

merlin::vulkan::RenderResult Submit(
    merlin::vulkan::Renderer& renderer,
    const std::shared_ptr<const FrameSnapshot>& snapshot,
    const merlin::vulkan::ShaderPaths& shaders,
    std::shared_ptr<const merlin::render::GpuScenePackedFrameUpdate> update) {
  merlin::vulkan::RenderRequest request;
  request.snapshot = snapshot;
  request.width = 32;
  request.height = 32;
  request.shaders = shaders;
  request.products = {{merlin::Aov::Color, true},
                      {merlin::Aov::PrimId, true},
                      {merlin::Aov::InstanceId, true}};
  request.gpu_scene_update = std::move(update);
  return renderer.Resolve(renderer.Submit(request));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: gpu_scene_vulkan_upload_test SHADER_DIR\n";
    return 1;
  }

  constexpr GpuScenePackingCapacities capacities{4, 4, 4, 4};
  merlin::vulkan::RendererOptions options;
  options.gpu_scene_capacities = capacities;
  options.descriptor_backend =
      merlin::vulkan::DescriptorBackendRequest::Bindless;
  std::optional<merlin::vulkan::Renderer> renderer;
  try {
    renderer.emplace(options);
  } catch (const std::exception& error) {
    std::cerr << "skip: Vulkan renderer unavailable: " << error.what() << '\n';
    return 77;
  }

  auto oversized_options = options;
  oversized_options.gpu_scene_capacities = GpuScenePackingCapacities{
      1, std::numeric_limits<std::uint32_t>::max(), 1, 1};
  try {
    merlin::vulkan::Renderer oversized_renderer(oversized_options);
    assert(false && "oversized GPU Scene storage buffer was accepted");
  } catch (const merlin::vulkan::RendererError& error) {
    assert(error.code() == merlin::vulkan::RendererErrorCode::Unsupported);
    assert(error.operation() == "create GPU Scene buffers");
  }

  const auto snapshot = MakeSnapshot();
  GpuScenePackingState packing(capacities);
  const std::vector placements{GpuGeometryPlacement{64, 256}};
  const std::vector identities{GpuInstanceIdentity{17, 23}};
  const std::vector bindings{GpuMaterialBinding{}};
  const auto inputs = GpuScenePackingInputs{placements, identities, bindings};
  auto first_update = std::make_shared<merlin::render::GpuScenePackedFrameUpdate>(
      packing.Apply(*snapshot, 0, 0, inputs));
  const auto expected_gpu_scene_bytes =
      sizeof(merlin::render::GpuGeometry) +
      sizeof(merlin::render::GpuInstance) +
      sizeof(merlin::render::GpuMaterial) + sizeof(merlin::render::GpuDraw);
  assert(first_update->copy_bytes == expected_gpu_scene_bytes);

  const auto shaders = MakeShaders(argv[1]);
  auto invalid = std::make_shared<merlin::render::GpuScenePackedFrameUpdate>(
      *first_update);
  invalid->geometries.ranges[0].first_slot = capacities.geometries;
  invalid->geometry_plan.dirty_ranges[0].first_slot = capacities.geometries;
  try {
    (void)Submit(*renderer, snapshot, shaders, invalid);
    assert(false && "out-of-capacity GPU Scene range was accepted");
  } catch (const merlin::vulkan::RendererError& error) {
    assert(error.code() == merlin::vulkan::RendererErrorCode::InvalidRequest);
  }

  auto invalid_draw_map =
      std::make_shared<merlin::render::GpuScenePackedFrameUpdate>(
          *first_update);
  invalid_draw_map->draw_slot_indices =
      std::make_shared<const std::vector<std::uint32_t>>(
          std::vector<std::uint32_t>{capacities.draws});
  try {
    (void)Submit(*renderer, snapshot, shaders, invalid_draw_map);
    assert(false && "out-of-capacity GPU Scene draw slot was accepted");
  } catch (const merlin::vulkan::RendererError& error) {
    assert(error.code() == merlin::vulkan::RendererErrorCode::InvalidRequest);
  }

  auto invalid_draw_reference =
      std::make_shared<merlin::render::GpuScenePackedFrameUpdate>(
          *first_update);
  invalid_draw_reference->draws.ranges[0].records[0].geometry_index =
      capacities.geometries;
  try {
    (void)Submit(*renderer, snapshot, shaders, invalid_draw_reference);
    assert(false && "out-of-capacity GPU Scene draw reference was accepted");
  } catch (const merlin::vulkan::RendererError& error) {
    assert(error.code() == merlin::vulkan::RendererErrorCode::InvalidRequest);
  }

  auto invalid_material_reference =
      std::make_shared<merlin::render::GpuScenePackedFrameUpdate>(
          *first_update);
  invalid_material_reference->materials.ranges[0]
      .records[0]
      .base_color_texture_index = options.bindless_texture_capacity;
  invalid_material_reference->materials.ranges[0]
      .records[0]
      .base_color_sampler_index = 0;
  try {
    (void)Submit(*renderer, snapshot, shaders, invalid_material_reference);
    assert(false && "out-of-capacity bindless material reference was accepted");
  } catch (const merlin::vulkan::RendererError& error) {
    assert(error.code() == merlin::vulkan::RendererErrorCode::InvalidRequest);
  }

  // A rejected native upload must not advance table continuity or prevent the
  // original packed update from being retried.
  merlin::vulkan::RenderResult first;
  try {
    first = Submit(*renderer, snapshot, shaders, first_update);
  } catch (const std::exception& error) {
    std::cerr << "gpu-scene: first table draw failed: " << error.what()
              << '\n';
    return 1;
  }
  assert(first.counters.gpu_scene_upload_bytes == expected_gpu_scene_bytes);
  assert(first.counters.gpu_scene_copy_range_count == 4);
  assert(first.counters.gpu_scene_upload_ring_reserved_bytes ==
         expected_gpu_scene_bytes);
  assert(first.counters.gpu_scene_upload_ring_growth_count == 1);
  assert(first.counters.upload_bytes >= expected_gpu_scene_bytes);
  if (first.counters.gpu_scene_draw_count != snapshot->draws.size()) {
    throw std::runtime_error(
        "basic Forward did not consume the persistent GPU Scene tables");
  }
  bool found_table_identity{};
  for (std::size_t i = 0; i < first.prim_id.pixels.size(); ++i) {
    if (first.prim_id.pixels[i] == identities[0].object_id) {
      assert(first.instance_id.pixels[i] == identities[0].instance_id);
      found_table_identity = true;
      break;
    }
  }
  if (!found_table_identity) {
    throw std::runtime_error(
        "table-backed Forward did not write packed object/instance IDs");
  }

  const auto statistics = renderer->statistics();
  assert(statistics.gpu_scene_buffers);
  assert(statistics.gpu_scene_capacity_bytes ==
         capacities.geometries * sizeof(merlin::render::GpuGeometry) +
             capacities.instances * sizeof(merlin::render::GpuInstance) +
             capacities.materials * sizeof(merlin::render::GpuMaterial) +
             capacities.draws * sizeof(merlin::render::GpuDraw));
  assert(statistics.gpu_scene_upload_ring.capacity_bytes != 0);

  auto static_update =
      std::make_shared<merlin::render::GpuScenePackedFrameUpdate>(packing.Apply(
          *snapshot, first.completion_value, first.completion_value, {}));
  assert(static_update->copy_bytes == 0);
  const auto steady = Submit(*renderer, snapshot, shaders, static_update);
  assert(steady.counters.gpu_scene_upload_bytes == 0);
  assert(steady.counters.gpu_scene_copy_range_count == 0);
  assert(steady.counters.gpu_scene_upload_ring_reserved_bytes == 0);
  assert(steady.counters.upload_bytes == 0);
  if (steady.counters.gpu_scene_draw_count != snapshot->draws.size()) {
    throw std::runtime_error(
        "static Forward frame stopped consuming the resident GPU Scene");
  }

  auto mixed_update =
      std::make_shared<merlin::render::GpuScenePackedFrameUpdate>(
          *static_update);
  mixed_update->geometry_plan.full_reconciliation = true;
  const auto mixed = Submit(*renderer, snapshot, shaders, mixed_update);
  assert(mixed.counters.gpu_scene_upload_bytes == 0);

  auto gap_snapshot = std::make_shared<FrameSnapshot>(*snapshot);
  gap_snapshot->revision = 2;
  auto gap = std::make_shared<merlin::render::GpuScenePackedFrameUpdate>(
      *first_update);
  const auto make_gap = [](auto& plan) {
    plan.base_revision = 0;
    plan.revision = 2;
    plan.full_reconciliation = false;
  };
  make_gap(gap->geometry_plan);
  make_gap(gap->instance_plan);
  make_gap(gap->material_plan);
  make_gap(gap->draw_plan);
  try {
    (void)Submit(*renderer, gap_snapshot, shaders, gap);
    assert(false && "discontinuous GPU Scene update was accepted");
  } catch (const merlin::vulkan::RendererError& error) {
    assert(error.code() == merlin::vulkan::RendererErrorCode::InvalidRequest);
  }

  auto partial_rebuild =
      std::make_shared<merlin::render::GpuScenePackedFrameUpdate>(
          *static_update);
  const auto make_partial_rebuild = [](auto& plan) {
    plan.base_revision = 0;
    plan.revision = 2;
    plan.full_reconciliation = true;
  };
  make_partial_rebuild(partial_rebuild->geometry_plan);
  make_partial_rebuild(partial_rebuild->instance_plan);
  make_partial_rebuild(partial_rebuild->material_plan);
  make_partial_rebuild(partial_rebuild->draw_plan);
  try {
    (void)Submit(*renderer, gap_snapshot, shaders, partial_rebuild);
    assert(false && "partial discontinuous GPU Scene rebuild was accepted");
  } catch (const merlin::vulkan::RendererError& error) {
    assert(error.code() == merlin::vulkan::RendererErrorCode::InvalidRequest);
  }

  auto source_less_snapshot = MakeSnapshot();
  source_less_snapshot->source_id = 0;
  source_less_snapshot->revision = 0;
  GpuScenePackingState source_less_packing(capacities);
  merlin::vulkan::Renderer source_less_renderer(options);
  const auto source_less_first_update =
      std::make_shared<merlin::render::GpuScenePackedFrameUpdate>(
          source_less_packing.Apply(*source_less_snapshot, 0, 0, inputs));
  const auto source_less_first = Submit(source_less_renderer,
                                        source_less_snapshot, shaders,
                                        source_less_first_update);
  assert(source_less_first.counters.gpu_scene_upload_bytes ==
         expected_gpu_scene_bytes);
  const auto source_less_repeat_update =
      std::make_shared<merlin::render::GpuScenePackedFrameUpdate>(
          source_less_packing.Apply(
              *source_less_snapshot, source_less_first.completion_value,
              source_less_first.completion_value, inputs));
  const auto source_less_repeat = Submit(source_less_renderer,
                                         source_less_snapshot, shaders,
                                         source_less_repeat_update);
  assert(source_less_repeat.counters.gpu_scene_upload_bytes ==
         expected_gpu_scene_bytes);

  std::cout << "Vulkan GPU Scene dirty-range upload tests passed\n";
}
