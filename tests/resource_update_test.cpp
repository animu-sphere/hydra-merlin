// Exercises the v0.2.0 resource-granular GPU scene contract and v0.3.0 ID AOVs
// end to end:
// zero steady-state upload/allocation/pipeline work, sub-resource dirty-range
// uploads, geometry sharing across instances, and deterministic retirement of
// removed resources. Requires a Vulkan device; exits 77 (CTest skip) when the
// renderer cannot be created.

#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/vulkan/renderer.hpp>

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace {

constexpr std::uint64_t kVertexBytes = sizeof(merlin::extraction::DrawVertex);
constexpr std::uint64_t kIndexBytes = sizeof(std::uint32_t);

merlin::MeshDescriptor Triangle() {
  merlin::MeshDescriptor mesh;
  mesh.label = "triangle";
  mesh.positions = {{0.0F, -0.5F, 0.2F}, {0.5F, 0.5F, 0.2F},
                    {-0.5F, 0.5F, 0.2F}};
  mesh.indices = {0, 1, 2};
  return mesh;
}

merlin::MeshDescriptor Quad() {
  merlin::MeshDescriptor mesh;
  mesh.label = "quad";
  mesh.positions = {{-0.9F, -0.9F, 0.5F}, {-0.3F, -0.9F, 0.5F},
                    {-0.3F, -0.3F, 0.5F}, {-0.9F, -0.3F, 0.5F}};
  mesh.indices = {0, 1, 2, 0, 2, 3};
  return mesh;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: resource_update_test SHADER_DIR\n";
    return 1;
  }
  const std::filesystem::path shader_dir = argv[1];
  const merlin::vulkan::ShaderPaths shaders{
      shader_dir / "triangle.vert.spv", shader_dir / "triangle.frag.spv",
      shader_dir / "triangle.bindless.vert.spv",
      shader_dir / "triangle.bindless.frag.spv",
      shader_dir / "environment.hdr"};

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

  const auto triangle = world.CreateMesh(Triangle());
  const auto quad = world.CreateMesh(Quad());
  merlin::MaterialDescriptor material;
  material.parameters.base_color = {0.9F, 0.4F, 0.1F, 1.0F};
  const auto primary_material = world.CreateMaterial(material);
  material.parameters.base_color = {0.1F, 0.4F, 0.9F, 1.0F};
  const auto secondary_material = world.CreateMaterial(material);

  merlin::InstanceDescriptor instance;
  instance.mesh = triangle;
  instance.material = primary_material;
  const auto first_triangle = world.CreateInstance(instance);
  instance.material = secondary_material;
  instance.transform.values[12] = 0.4F;
  const auto second_triangle = world.CreateInstance(instance);
  instance.mesh = quad;
  instance.material = primary_material;
  instance.transform = {};
  const auto quad_instance = world.CreateInstance(instance);

  constexpr std::uint64_t triangle_vertex_bytes = 3U * kVertexBytes;
  constexpr std::uint64_t triangle_index_bytes = 3U * kIndexBytes;
  constexpr std::uint64_t quad_vertex_bytes = 4U * kVertexBytes;
  constexpr std::uint64_t quad_index_bytes = 6U * kIndexBytes;

  // First frame: both meshes upload once even though the triangle geometry is
  // shared by two instances.
  const auto first = render();
  assert(first.counters.draw_count == 3);
  assert(first.counters.geometry_cache_misses == 2);
  assert(first.counters.geometry_cache_hits == 0);
  assert(first.counters.geometry_reconcile_count == 2);
  assert(first.counters.upload_bytes ==
         triangle_vertex_bytes + triangle_index_bytes + quad_vertex_bytes +
             quad_index_bytes);
  assert(first.counters.vertex_upload_bytes ==
         triangle_vertex_bytes + quad_vertex_bytes);
  assert(first.counters.index_upload_bytes ==
         triangle_index_bytes + quad_index_bytes);
  assert(first.counters.texture_upload_bytes == 0);
  assert(first.counters.upload_ring_reserved_bytes ==
         triangle_vertex_bytes + 16U + quad_vertex_bytes + 32U);
  assert(first.counters.buffer_suballocation_count == 4);
  assert(first.counters.geometry_range_reuse_count == 0);
  assert(first.counters.geometry_arena_growth_count == 2);
  assert(first.counters.geometry_arena_growth_bytes == 512U * 1024U);
  assert(first.counters.upload_ring_growth_count == 1);
  assert(first.counters.upload_ring_growth_bytes == 256U * 1024U);
  assert(first.counters.pipeline_creation_count == 1);
  std::unordered_set<std::uint32_t> visible_prim_ids;
  std::unordered_set<std::uint32_t> visible_instance_ids;
  for (std::size_t i = 0; i < first.depth.pixels.size(); ++i) {
    if (first.depth.pixels[i] < 1.0F) {
      assert(first.prim_id.pixels[i] !=
             std::numeric_limits<std::uint32_t>::max());
      assert(first.instance_id.pixels[i] !=
             std::numeric_limits<std::uint32_t>::max());
      visible_prim_ids.insert(first.prim_id.pixels[i]);
      visible_instance_ids.insert(first.instance_id.pixels[i]);
    }
  }
  assert(visible_prim_ids.size() == 2);
  assert(visible_instance_ids.size() == 3);

  // Warm static frame: zero upload bytes, zero allocations, zero pipeline
  // creation.
  const auto steady = render();
  assert(steady.counters.upload_bytes == 0);
  assert(steady.counters.vertex_upload_bytes == 0);
  assert(steady.counters.index_upload_bytes == 0);
  assert(steady.counters.texture_upload_bytes == 0);
  assert(steady.counters.upload_ring_reserved_bytes == 0);
  assert(steady.counters.allocation_count == 0);
  assert(steady.counters.buffer_suballocation_count == 0);
  assert(steady.counters.pipeline_creation_count == 0);
  assert(steady.counters.scene_cache_hits == 1);
  assert(steady.counters.geometry_cache_hits == 2);
  assert(steady.counters.geometry_cache_misses == 0);
  assert(steady.counters.geometry_reconcile_count == 0);

  const auto baseline_statistics = renderer->statistics();
  assert(baseline_statistics.scene_uploads == 1);
  assert(baseline_statistics.pending_geometry_retirements == 0);
  assert(baseline_statistics.geometry_arena_blocks == 2);
  assert(baseline_statistics.vertex_arena.capacity_bytes == 256U * 1024U);
  assert(baseline_statistics.vertex_arena.resident_bytes ==
         triangle_vertex_bytes + quad_vertex_bytes);
  assert(baseline_statistics.vertex_arena.free_bytes +
             baseline_statistics.vertex_arena.resident_bytes ==
         baseline_statistics.vertex_arena.capacity_bytes);
  assert(baseline_statistics.vertex_arena.active_ranges == 2);
  assert(baseline_statistics.vertex_arena.blocks == 1);
  assert(baseline_statistics.vertex_arena.growth_count == 1);
  assert(baseline_statistics.index_arena.capacity_bytes == 256U * 1024U);
  assert(baseline_statistics.index_arena.resident_bytes == 48);
  assert(baseline_statistics.index_arena.free_bytes +
             baseline_statistics.index_arena.resident_bytes ==
         baseline_statistics.index_arena.capacity_bytes);
  assert(baseline_statistics.index_arena.active_ranges == 2);
  assert(baseline_statistics.index_arena.blocks == 1);
  assert(baseline_statistics.index_arena.growth_count == 1);
  assert(baseline_statistics.upload_ring.capacity_bytes == 256U * 1024U);
  assert(baseline_statistics.upload_ring.in_flight_bytes == 0);
  assert(baseline_statistics.upload_ring.active_regions == 0);
  assert(baseline_statistics.upload_ring.reservation_count == 1);
  assert(baseline_statistics.upload_ring.growth_count == 1);

  // Transform-only edit: no geometry work at all.
  instance = world.Get(second_triangle);
  instance.transform.values[12] = -0.4F;
  world.UpdateInstance(second_triangle, instance,
                       merlin::ChangeAspect::Transform);
  const auto transformed = render();
  assert(transformed.counters.draw_count == 3);
  assert(transformed.counters.upload_bytes == 0);
  assert(transformed.counters.allocation_count == 0);
  assert(transformed.counters.geometry_cache_misses == 0);
  assert(transformed.counters.geometry_reconcile_count == 0);
  assert(transformed.counters.pipeline_creation_count == 0);

  // Visibility-only edit: the draw disappears without touching geometry.
  instance = world.Get(quad_instance);
  instance.visible = false;
  world.UpdateInstance(quad_instance, instance,
                       merlin::ChangeAspect::Visibility);
  const auto hidden = render();
  assert(hidden.counters.draw_count == 2);
  assert(hidden.counters.upload_bytes == 0);
  assert(hidden.counters.geometry_cache_misses == 0);
  assert(hidden.counters.geometry_reconcile_count == 0);
  assert(renderer->statistics().geometry_range_retirements ==
         baseline_statistics.geometry_range_retirements);

  // Material-only edit: colors change, geometry resources do not.
  material = world.Get(primary_material);
  material.parameters.base_color = {0.2F, 0.8F, 0.2F, 1.0F};
  world.UpdateMaterial(primary_material, material);
  const auto recolored = render();
  assert(recolored.counters.upload_bytes == 0);
  assert(recolored.counters.geometry_cache_misses == 0);
  assert(recolored.counters.geometry_reconcile_count == 0);

  // Points-only edit with the same vertex count: the vertex range is updated
  // in place and topology is untouched.
  auto moved_triangle = Triangle();
  moved_triangle.positions[0].x = 0.1F;
  world.UpdateMesh(triangle, moved_triangle, merlin::ChangeAspect::Points,
                   std::vector<merlin::ElementRange>{{0, 1}});
  const auto moved = render();
  assert(moved.counters.upload_bytes == kVertexBytes);
  assert(moved.counters.vertex_upload_bytes == kVertexBytes);
  assert(moved.counters.index_upload_bytes == 0);
  assert(moved.counters.upload_ring_reserved_bytes == kVertexBytes);
  assert(moved.counters.geometry_cache_misses == 1);
  assert(moved.counters.geometry_cache_hits == 1);
  assert(moved.counters.geometry_reconcile_count == 1);
  assert(moved.counters.buffer_suballocation_count == 0);
  assert(moved.counters.buffer_range_release_count == 0);
  assert(moved.counters.geometry_range_reuse_count == 1);
  assert(moved.counters.allocation_count == 0);

  // Topology edit with a different vertex and index count reallocates both
  // packed vertex and index ranges; the unchanged quad is untouched.
  auto rebuilt_triangle = moved_triangle;
  rebuilt_triangle.positions.push_back({0.9F, -0.9F, 0.2F});
  rebuilt_triangle.indices = {0, 1, 2, 1, 3, 2};
  world.UpdateMesh(triangle, rebuilt_triangle);
  const auto rebuilt = render();
  assert(rebuilt.counters.upload_bytes ==
         4U * kVertexBytes + 6U * kIndexBytes);
  assert(rebuilt.counters.vertex_upload_bytes == 4U * kVertexBytes);
  assert(rebuilt.counters.index_upload_bytes == 6U * kIndexBytes);
  assert(rebuilt.counters.upload_ring_reserved_bytes ==
         4U * kVertexBytes + 32U);
  assert(rebuilt.counters.geometry_cache_misses == 1);
  assert(rebuilt.counters.geometry_reconcile_count == 1);
  assert(rebuilt.counters.buffer_suballocation_count == 2);
  assert(rebuilt.counters.buffer_range_release_count == 2);
  assert(rebuilt.counters.geometry_range_reuse_count == 0);
  assert(rebuilt.counters.geometry_arena_growth_count == 0);
  assert(rebuilt.counters.upload_ring_growth_count == 0);
  const auto after_rebuild = renderer->statistics();
  assert(after_rebuild.geometry_range_retirements ==
         baseline_statistics.geometry_range_retirements + 2);
  assert(after_rebuild.pending_geometry_retirements == 0);
  assert(after_rebuild.geometry_arena_blocks == 2);

  // Removing the quad retires its ranges exactly once the retire frame
  // completes; the surviving triangle stays resident.
  world.Remove(quad_instance);
  world.Remove(quad);
  const auto removed = render();
  assert(removed.counters.draw_count == 2);
  assert(removed.counters.upload_bytes == 0);
  assert(removed.counters.geometry_cache_hits == 1);
  assert(removed.counters.geometry_cache_misses == 0);
  assert(removed.counters.geometry_reconcile_count == 1);
  assert(removed.counters.buffer_range_release_count == 2);
  assert(removed.counters.scene_cache_misses == 1);
  const auto after_removal = renderer->statistics();
  assert(after_removal.geometry_range_retirements ==
         after_rebuild.geometry_range_retirements + 2);
  assert(after_removal.pending_geometry_retirements == 0);

  // A recreated mesh may reuse the removed slot index, but its generation
  // makes the handle value distinct, so the cache can never alias the retired
  // resource.
  const auto recreated = world.CreateMesh(Quad());
  assert(recreated.value() != quad.value());
  instance = world.Get(first_triangle);
  instance.mesh = recreated;
  world.UpdateInstance(first_triangle, instance);
  const auto reused = render();
  assert(reused.counters.geometry_cache_misses == 1);
  assert(reused.counters.geometry_reconcile_count == 1);
  assert(reused.counters.upload_bytes == quad_vertex_bytes + quad_index_bytes);

  // Emptying the world renders cleanly and releases every slot.
  world.Remove(first_triangle);
  world.Remove(second_triangle);
  world.Remove(triangle);
  world.Remove(recreated);
  const auto empty = render();
  assert(empty.counters.draw_count == 0);
  assert(empty.counters.upload_bytes == 0);
  const auto final_statistics = renderer->statistics();
  assert(final_statistics.pending_geometry_retirements == 0);
  assert(final_statistics.geometry_arena_blocks == 2);
  assert(final_statistics.vertex_arena.resident_bytes == 0);
  assert(final_statistics.vertex_arena.free_bytes ==
         final_statistics.vertex_arena.capacity_bytes);
  assert(final_statistics.vertex_arena.largest_free_span_bytes ==
         final_statistics.vertex_arena.capacity_bytes);
  assert(final_statistics.vertex_arena.active_ranges == 0);
  assert(final_statistics.vertex_arena.retiring_ranges == 0);
  assert(final_statistics.index_arena.resident_bytes == 0);
  assert(final_statistics.index_arena.free_bytes ==
         final_statistics.index_arena.capacity_bytes);
  assert(final_statistics.index_arena.largest_free_span_bytes ==
         final_statistics.index_arena.capacity_bytes);
  assert(final_statistics.index_arena.active_ranges == 0);
  assert(final_statistics.index_arena.retiring_ranges == 0);
  assert(final_statistics.upload_ring.in_flight_bytes == 0);
  assert(final_statistics.upload_ring.active_regions == 0);

  // FrameSnapshot is a public backend boundary. A directly constructed empty
  // geometry record must not produce a zero-sized Vulkan allocation or copy.
  merlin::extraction::FrameSnapshot empty_snapshot;
  empty_snapshot.revision = extractor.snapshot()->revision + 1U;
  merlin::extraction::GeometryRecord empty_record;
  empty_record.mesh = std::numeric_limits<std::uint64_t>::max();
  empty_record.points_revision = 1;
  empty_record.primvar_revision = 1;
  empty_record.topology_revision = 1;
  empty_record.vertex_revision = 1;
  empty_record.index_revision = 1;
  empty_record.vertices =
      std::make_shared<const std::vector<merlin::extraction::DrawVertex>>();
  empty_record.indices =
      std::make_shared<const std::vector<std::uint32_t>>();
  empty_snapshot.geometries.push_back(std::move(empty_record));
  const auto empty_geometry =
      renderer->Render(empty_snapshot, 64, 64, shaders);
  assert(empty_geometry.counters.draw_count == 0);
  assert(empty_geometry.counters.upload_bytes == 0);
  assert(empty_geometry.counters.allocation_count == 0);
  assert(empty_geometry.counters.buffer_suballocation_count == 0);
  assert(empty_geometry.counters.geometry_cache_misses == 1);
  assert(renderer->statistics().geometry_arena_blocks == 2);

  empty_snapshot.geometries.clear();
  ++empty_snapshot.revision;
  const auto removed_empty_geometry =
      renderer->Render(empty_snapshot, 64, 64, shaders);
  assert(removed_empty_geometry.counters.buffer_range_release_count == 0);

  // Skipping a snapshot revision invalidates its one-step delta. The renderer
  // must fall back to full reconciliation and a full payload upload rather
  // than applying a changed range against the wrong resident base revision.
  merlin::RenderWorld gap_world;
  merlin::extraction::SceneExtractor gap_extractor;
  auto gap_mesh = Triangle();
  const auto changed_mesh = gap_world.CreateMesh(gap_mesh);
  gap_world.CreateMesh(Quad());
  gap_extractor.Apply(gap_world, gap_world.Commit());
  const auto gap_first = renderer->Render(
      *gap_extractor.snapshot(), 64, 64, shaders);
  assert(gap_first.counters.geometry_reconcile_count == 2);

  gap_mesh.positions[0].x = 0.1F;
  gap_world.UpdateMesh(changed_mesh, gap_mesh, merlin::ChangeAspect::Points,
                       std::vector<merlin::ElementRange>{{0, 1}});
  gap_extractor.Apply(gap_world, gap_world.Commit());
  gap_mesh.positions[0].x = 0.2F;
  gap_world.UpdateMesh(changed_mesh, gap_mesh, merlin::ChangeAspect::Points,
                       std::vector<merlin::ElementRange>{{0, 1}});
  gap_extractor.Apply(gap_world, gap_world.Commit());
  const auto gap_fallback = renderer->Render(
      *gap_extractor.snapshot(), 64, 64, shaders);
  assert(gap_fallback.counters.geometry_reconcile_count == 2);
  assert(gap_fallback.counters.geometry_cache_misses == 1);
  assert(gap_fallback.counters.geometry_cache_hits == 1);
  assert(gap_fallback.counters.upload_bytes == 3U * kVertexBytes);

  // Independent SceneExtractors may issue identical handle and resource
  // revision values. Source changes must replace residency instead of treating
  // those coincident identifiers as cache hits.
  merlin::RenderWorld source_world_a;
  merlin::extraction::SceneExtractor source_extractor_a;
  const auto source_mesh_a = source_world_a.CreateMesh(Triangle());
  source_extractor_a.Apply(source_world_a, source_world_a.Commit());
  const auto source_snapshot_a = source_extractor_a.snapshot();
  const auto source_frame_a =
      renderer->Render(*source_snapshot_a, 64, 64, shaders);
  assert(source_frame_a.counters.geometry_cache_misses == 1);

  merlin::RenderWorld source_world_b;
  merlin::extraction::SceneExtractor source_extractor_b;
  const auto source_mesh_b = source_world_b.CreateMesh(Quad());
  source_extractor_b.Apply(source_world_b, source_world_b.Commit());
  const auto source_snapshot_b = source_extractor_b.snapshot();
  assert(source_mesh_a.value() == source_mesh_b.value());
  assert(source_snapshot_a->revision == source_snapshot_b->revision);
  assert(source_snapshot_a->source_id != source_snapshot_b->source_id);
  const auto source_frame_b =
      renderer->Render(*source_snapshot_b, 64, 64, shaders);
  assert(source_frame_b.counters.geometry_cache_hits == 0);
  assert(source_frame_b.counters.geometry_cache_misses == 1);
  assert(source_frame_b.counters.geometry_reconcile_count == 2);
  assert(source_frame_b.counters.upload_bytes ==
         quad_vertex_bytes + quad_index_bytes);

  // A middle removal swap-moves the final dense geometry record. Its delta
  // index must let the backend reconcile that unchanged record and then apply
  // a later localized edit without assuming handle-sorted snapshot tables.
  merlin::RenderWorld dense_world;
  merlin::extraction::SceneExtractor dense_extractor;
  const auto dense_a = dense_world.CreateMesh(Triangle());
  const auto dense_b = dense_world.CreateMesh(Quad());
  auto dense_c_descriptor = Triangle();
  const auto dense_c = dense_world.CreateMesh(dense_c_descriptor);
  dense_extractor.Apply(dense_world, dense_world.Commit());
  (void)renderer->Render(*dense_extractor.snapshot(), 64, 64, shaders);

  dense_world.Remove(dense_b);
  dense_extractor.Apply(dense_world, dense_world.Commit());
  const auto dense_removed_snapshot = dense_extractor.snapshot();
  assert(dense_removed_snapshot->geometries[1].mesh == dense_c.value());
  assert(dense_removed_snapshot->delta->geometries.upserts ==
         std::vector<std::uint64_t>{dense_c.value()});
  assert(dense_removed_snapshot->delta->geometries.upsert_indices ==
         std::vector<std::uint32_t>{1});
  const auto dense_removed = renderer->Render(
      *dense_removed_snapshot, 64, 64, shaders);
  assert(dense_removed.counters.geometry_reconcile_count == 2);
  assert(dense_removed.counters.geometry_cache_misses == 0);
  assert(dense_removed.counters.upload_bytes == 0);

  dense_c_descriptor.positions[0].x = 0.25F;
  dense_world.UpdateMesh(dense_c, dense_c_descriptor,
                         merlin::ChangeAspect::Points,
                         std::vector<merlin::ElementRange>{{0, 1}});
  dense_extractor.Apply(dense_world, dense_world.Commit());
  const auto dense_edited = renderer->Render(
      *dense_extractor.snapshot(), 64, 64, shaders);
  assert(dense_edited.counters.geometry_reconcile_count == 1);
  assert(dense_edited.counters.geometry_cache_misses == 1);
  assert(dense_edited.counters.upload_bytes == kVertexBytes);
  (void)dense_a;

  std::cout << "resource update contract verified: uploads="
            << final_statistics.scene_uploads << " retirements="
            << final_statistics.geometry_range_retirements << '\n';
  return 0;
}
