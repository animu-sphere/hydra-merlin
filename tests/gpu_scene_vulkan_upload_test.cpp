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

std::shared_ptr<FrameSnapshot> MakeMultiDrawSnapshot() {
  auto snapshot = MakeSnapshot();
  snapshot->source_id = 42;
  snapshot->revision = 2;
  auto first_instance = snapshot->instances[0];
  first_instance.transform.values[12] = -0.5F;
  snapshot->instances.replace(0, first_instance);

  auto second_instance = first_instance;
  second_instance.instance = 304;
  second_instance.revision = 7;
  second_instance.transform.values[12] = 0.5F;
  snapshot->instances.push_back(second_instance);

  auto second_draw = snapshot->draws[0];
  second_draw.instance_index = 1;
  second_draw.instance = second_instance.instance;
  second_draw.draw = 0x2234567887654321ULL;
  second_draw.revision = 8;
  snapshot->draws.push_back(second_draw);
  return snapshot;
}

std::shared_ptr<FrameSnapshot> MakeMixedPipelineSnapshot() {
  auto snapshot = MakeMultiDrawSnapshot();
  snapshot->source_id = 43;
  snapshot->revision = 3;

  auto second_material = snapshot->materials[0];
  second_material.material = 203;
  second_material.revision = 9;
  second_material.double_sided = true;
  second_material.parameters.base_color = {0.9F, 0.3F, 0.2F, 1.0F};
  snapshot->materials.push_back(second_material);

  auto second_instance = snapshot->instances[1];
  second_instance.material = second_material.material;
  second_instance.revision = 10;
  snapshot->instances.replace(1, second_instance);

  auto second_draw = snapshot->draws[1];
  second_draw.material_index = 1;
  second_draw.revision = 11;
  snapshot->draws.replace(1, second_draw);
  return snapshot;
}

std::shared_ptr<FrameSnapshot> MakePipelineBatchSnapshot(
    std::uint32_t draw_count, bool alternate_pipeline_state) {
  auto snapshot = MakeSnapshot();
  snapshot->source_id = 44;
  snapshot->revision = 4;

  auto double_sided = snapshot->materials[0];
  double_sided.material = 203;
  double_sided.revision = 9;
  double_sided.double_sided = true;
  snapshot->materials.assign({snapshot->materials[0], double_sided});

  const auto base_instance = snapshot->instances[0];
  const auto base_draw = snapshot->draws[0];
  std::vector<InstanceRecord> instances;
  std::vector<DrawRecord> draws;
  instances.reserve(draw_count);
  draws.reserve(draw_count);
  for (std::uint32_t i = 0; i < draw_count; ++i) {
    const auto material_index = alternate_pipeline_state ? i % 2U : 0U;
    auto instance = base_instance;
    instance.instance = 1000U + i;
    instance.material = snapshot->materials[material_index].material;
    instance.revision = 100U + i;
    instances.push_back(instance);

    auto draw = base_draw;
    draw.material_index = material_index;
    draw.instance_index = i;
    draw.instance = instance.instance;
    draw.draw = 0x4000000000000000ULL + i;
    draw.revision = 200U + i;
    draws.push_back(draw);
  }
  snapshot->instances.assign(std::move(instances));
  snapshot->draws.assign(std::move(draws));
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
    std::shared_ptr<const merlin::render::GpuScenePackedFrameUpdate> update,
    merlin::vulkan::GpuDrivenIndexedMode gpu_driven =
        merlin::vulkan::GpuDrivenIndexedMode::Disabled,
    std::uint32_t visibility_mask = ~std::uint32_t{}) {
  merlin::vulkan::RenderRequest request;
  request.snapshot = snapshot;
  request.width = 32;
  request.height = 32;
  request.shaders = shaders;
  request.products = {{merlin::Aov::Color, true},
                      {merlin::Aov::PrimId, true},
                      {merlin::Aov::InstanceId, true}};
  request.gpu_scene_update = std::move(update);
  if (gpu_driven != merlin::vulkan::GpuDrivenIndexedMode::Disabled) {
    request.gpu_driven_indexed.mode = gpu_driven;
    request.gpu_driven_indexed.visibility_mask = visibility_mask;
  }
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
  options.enable_validation = true;
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
  // The first renderer-resident mesh occupies the beginning of both arenas;
  // GPU-driven indirect commands consume these block-local byte offsets.
  const std::vector placements{GpuGeometryPlacement{0, 0}};
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
    first = Submit(*renderer, snapshot, shaders, first_update,
                   merlin::vulkan::GpuDrivenIndexedMode::Require);
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
  assert(first.counters.gpu_driven_candidate_draw_count ==
         snapshot->draws.size());
  assert(first.counters.gpu_driven_visible_draw_count ==
         snapshot->draws.size());
  assert(first.counters.gpu_driven_visibility_mask_culled_count == 0);
  assert(first.counters.gpu_driven_frustum_culled_count == 0);
  assert(first.counters.gpu_driven_indirect_draw_count == 1);
  assert(first.counters.gpu_driven_candidate_upload_bytes ==
         snapshot->draws.size() * sizeof(std::uint32_t));
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

  auto culled_update =
      std::make_shared<merlin::render::GpuScenePackedFrameUpdate>(packing.Apply(
          *snapshot, first.completion_value, first.completion_value, {}));
  const auto culled = Submit(
      *renderer, snapshot, shaders, culled_update,
      merlin::vulkan::GpuDrivenIndexedMode::Require, 0U);
  assert(culled.counters.gpu_driven_candidate_draw_count == 1);
  assert(culled.counters.gpu_driven_visible_draw_count == 0);
  assert(culled.counters.gpu_driven_visibility_mask_culled_count == 1);
  assert(culled.counters.gpu_driven_frustum_culled_count == 0);
  assert(culled.counters.gpu_driven_candidate_upload_bytes == 0);
  assert(culled.counters.upload_bytes == 0);
  assert(culled.counters.gpu_scene_draw_count == 0);

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
          *snapshot, culled.completion_value, culled.completion_value, {}));
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

  auto fallback_options = options;
  fallback_options.descriptor_backend =
      merlin::vulkan::DescriptorBackendRequest::Conventional;
  merlin::vulkan::Renderer fallback_renderer(fallback_options);
  const auto fallback = Submit(
      fallback_renderer, snapshot, shaders, first_update,
      merlin::vulkan::GpuDrivenIndexedMode::Prefer);
  assert(fallback.counters.gpu_driven_fallback_count == 1);
  assert(fallback.counters.gpu_driven_candidate_draw_count == 0);
  assert(fallback.counters.draw_count == snapshot->draws.size());

  const auto multi_snapshot = MakeMultiDrawSnapshot();
  GpuScenePackingState multi_packing(capacities);
  const std::vector multi_placements{GpuGeometryPlacement{0, 0}};
  const std::vector multi_identities{
      GpuInstanceIdentity{17, 23}, GpuInstanceIdentity{18, 24}};
  const std::vector multi_bindings{GpuMaterialBinding{}};
  auto multi_update =
      std::make_shared<merlin::render::GpuScenePackedFrameUpdate>(
          multi_packing.Apply(
              *multi_snapshot, 0, 0,
              GpuScenePackingInputs{multi_placements, multi_identities,
                                    multi_bindings}));
  assert(multi_update->draw_slot_indices->size() == 2);
  assert((*multi_update->draw_slot_indices)[1] != 0);
  const auto multi = Submit(
      *renderer, multi_snapshot, shaders, multi_update,
      merlin::vulkan::GpuDrivenIndexedMode::Require);
  assert(multi.counters.gpu_driven_candidate_upload_bytes ==
         multi_snapshot->draws.size() * sizeof(std::uint32_t));
  assert(multi.counters.gpu_driven_visible_draw_count == 2);
  bool found_first_identity{};
  bool found_second_identity{};
  for (const auto id : multi.prim_id.pixels) {
    found_first_identity = found_first_identity || id == 17;
    found_second_identity = found_second_identity || id == 18;
  }
  if (!found_first_identity || !found_second_identity) {
    throw std::runtime_error(
        "GPU-driven Forward did not preserve non-zero firstInstance identity");
  }

  const auto mixed_pipeline_snapshot = MakeMixedPipelineSnapshot();
  GpuScenePackingState mixed_pipeline_packing(capacities);
  const std::vector mixed_pipeline_bindings{GpuMaterialBinding{},
                                             GpuMaterialBinding{}};
  auto mixed_pipeline_update =
      std::make_shared<merlin::render::GpuScenePackedFrameUpdate>(
          mixed_pipeline_packing.Apply(
              *mixed_pipeline_snapshot, 0, 0,
              GpuScenePackingInputs{multi_placements, multi_identities,
                                    mixed_pipeline_bindings}));
  const auto mixed_pipeline = Submit(
      *renderer, mixed_pipeline_snapshot, shaders, mixed_pipeline_update,
      merlin::vulkan::GpuDrivenIndexedMode::Require);
  assert(mixed_pipeline.counters.gpu_driven_candidate_draw_count == 2);
  assert(mixed_pipeline.counters.gpu_driven_visible_draw_count == 2);
  assert(mixed_pipeline.counters.gpu_driven_indirect_draw_count == 2);
  assert(mixed_pipeline.counters.gpu_driven_candidate_upload_bytes ==
         2 * sizeof(std::uint32_t));
  assert(mixed_pipeline.counters.gpu_driven_fallback_count == 0);
  found_first_identity = false;
  found_second_identity = false;
  for (const auto id : mixed_pipeline.prim_id.pixels) {
    found_first_identity = found_first_identity || id == 17;
    found_second_identity = found_second_identity || id == 18;
  }
  if (!found_first_identity || !found_second_identity) {
    throw std::runtime_error(
        "mixed-pipeline GPU-driven batches did not preserve draw identity");
  }

  constexpr std::uint32_t alternating_batch_count = 32;
  constexpr GpuScenePackingCapacities alternating_capacities{
      2, alternating_batch_count, 2, alternating_batch_count};
  auto alternating_options = options;
  alternating_options.gpu_scene_capacities = alternating_capacities;
  merlin::vulkan::Renderer alternating_renderer(alternating_options);
  const auto alternating_snapshot =
      MakePipelineBatchSnapshot(alternating_batch_count, true);
  GpuScenePackingState alternating_packing(alternating_capacities);
  const std::vector alternating_placements{GpuGeometryPlacement{0, 0}};
  std::vector<GpuInstanceIdentity> alternating_identities;
  alternating_identities.reserve(alternating_batch_count);
  for (std::uint32_t i = 0; i < alternating_batch_count; ++i) {
    alternating_identities.push_back({1000U + i, 2000U + i});
  }
  const std::vector alternating_bindings{GpuMaterialBinding{},
                                          GpuMaterialBinding{}};
  auto alternating_update =
      std::make_shared<merlin::render::GpuScenePackedFrameUpdate>(
          alternating_packing.Apply(
              *alternating_snapshot, 0, 0,
              GpuScenePackingInputs{alternating_placements,
                                    alternating_identities,
                                    alternating_bindings}));
  const auto alternating = Submit(
      alternating_renderer, alternating_snapshot, shaders,
      alternating_update, merlin::vulkan::GpuDrivenIndexedMode::Require);
  assert(alternating.counters.gpu_driven_candidate_draw_count ==
         alternating_batch_count);
  assert(alternating.counters.gpu_driven_visible_draw_count ==
         alternating_batch_count);
  assert(alternating.counters.gpu_driven_indirect_draw_count ==
         alternating_batch_count);
  assert(alternating.counters.gpu_driven_fallback_count == 0);

  merlin::vulkan::Renderer single_batch_renderer(alternating_options);
  const auto single_batch_snapshot =
      MakePipelineBatchSnapshot(alternating_batch_count, false);
  GpuScenePackingState single_batch_packing(alternating_capacities);
  auto single_batch_update =
      std::make_shared<merlin::render::GpuScenePackedFrameUpdate>(
          single_batch_packing.Apply(
              *single_batch_snapshot, 0, 0,
              GpuScenePackingInputs{alternating_placements,
                                    alternating_identities,
                                    alternating_bindings}));
  const auto single_batch = Submit(
      single_batch_renderer, single_batch_snapshot, shaders,
      single_batch_update, merlin::vulkan::GpuDrivenIndexedMode::Require);
  assert(single_batch.counters.gpu_driven_indirect_draw_count == 1);
  assert(alternating.counters.buffer_allocation_count ==
         single_batch.counters.buffer_allocation_count);
  assert(alternating.counters.descriptor_pool_creation_count ==
         single_batch.counters.descriptor_pool_creation_count);

  // Exercise parallel compaction across multiple compute workgroups in one
  // native batch. Every third candidate is rejected by the visibility mask;
  // the remaining commands reserve distinct compacted output slots atomically.
  constexpr std::uint32_t parallel_candidate_count = 130;
  constexpr auto parallel_culled_count =
      (parallel_candidate_count + 2U) / 3U;
  constexpr GpuScenePackingCapacities parallel_capacities{
      2, parallel_candidate_count, 2, parallel_candidate_count};
  auto parallel_options = options;
  parallel_options.gpu_scene_capacities = parallel_capacities;
  merlin::vulkan::Renderer parallel_renderer(parallel_options);
  auto parallel_snapshot =
      MakePipelineBatchSnapshot(parallel_candidate_count, false);
  parallel_snapshot->source_id = 45;
  parallel_snapshot->revision = 5;
  GpuScenePackingState parallel_packing(parallel_capacities);
  std::vector<GpuInstanceIdentity> parallel_identities;
  parallel_identities.reserve(parallel_candidate_count);
  for (std::uint32_t i = 0; i < parallel_candidate_count; ++i) {
    parallel_identities.push_back(
        {3000U + i, 4000U + i,
         i % 3U == 0U ? 0U : ~std::uint32_t{}});
  }
  const std::vector parallel_bindings{GpuMaterialBinding{},
                                      GpuMaterialBinding{}};
  auto parallel_update =
      std::make_shared<merlin::render::GpuScenePackedFrameUpdate>(
          parallel_packing.Apply(
              *parallel_snapshot, 0, 0,
              GpuScenePackingInputs{
                  alternating_placements, parallel_identities,
                  parallel_bindings}));
  const auto parallel = Submit(
      parallel_renderer, parallel_snapshot, shaders, parallel_update,
      merlin::vulkan::GpuDrivenIndexedMode::Require);
  assert(parallel.counters.gpu_driven_candidate_draw_count ==
         parallel_candidate_count);
  assert(parallel.counters.gpu_driven_visible_draw_count ==
         parallel_candidate_count - parallel_culled_count);
  assert(parallel.counters.gpu_driven_visibility_mask_culled_count ==
         parallel_culled_count);
  assert(parallel.counters.gpu_driven_frustum_culled_count == 0);
  assert(parallel.counters.gpu_driven_indirect_draw_count == 1);
  assert(parallel.counters.gpu_driven_fallback_count == 0);

  std::cout << "Vulkan GPU Scene dirty-range upload tests passed\n";
}
