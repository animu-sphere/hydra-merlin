#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>

#include <cassert>
#include <algorithm>
#include <stdexcept>
#include <type_traits>

namespace {

void CheckPersistentTable() {
  merlin::extraction::PersistentTable<std::uint64_t> table;
  static_assert(std::is_same_v<decltype(table[0]), const std::uint64_t&>);
  std::vector<std::uint64_t> expected;
  for (std::uint64_t value = 0; value < 512; ++value) {
    const auto index = expected.empty()
                           ? 0U
                           : static_cast<std::size_t>((value * 37U) %
                                                      (expected.size() + 1U));
    table.insert(table.begin() + static_cast<std::ptrdiff_t>(index), value);
    expected.insert(expected.begin() + static_cast<std::ptrdiff_t>(index),
                    value);
  }
  assert(std::equal(table.begin(), table.end(), expected.begin(),
                    expected.end()));
  auto reverse = table.end();
  for (auto expected_value = expected.rbegin();
       expected_value != expected.rend(); ++expected_value) {
    --reverse;
    assert(*reverse == *expected_value);
  }
  assert(reverse == table.begin());

  merlin::extraction::PersistentTable<std::uint64_t> sorted;
  for (std::uint64_t value = 0; value < 512; value += 2) {
    sorted.push_back(value);
  }
  for (std::uint64_t value = 0; value <= 512; ++value) {
    const auto index = sorted.lower_bound_index(
        value, [](std::uint64_t record, std::uint64_t candidate) {
          return record < candidate;
        });
    assert(index == std::min<std::size_t>((value + 1U) / 2U, sorted.size()));
  }

  const auto previous = table;
  const auto shared_identity = table.record_identity(0);
  table.replace(256, 10'000U);
  expected[256] = 10'000U;
  assert(table[256] == 10'000U);
  assert(previous[256] != 10'000U);
  assert(table.record_identity(0) == shared_identity);

  for (std::size_t remaining = table.size(); remaining > 32U; --remaining) {
    const auto index = (remaining * 19U) % remaining;
    table.erase(table.begin() + static_cast<std::ptrdiff_t>(index));
    expected.erase(expected.begin() + static_cast<std::ptrdiff_t>(index));
  }
  assert(std::equal(table.begin(), table.end(), expected.begin(),
                    expected.end()));

  const auto before_unordered_erase = table;
  const auto displaced_identity = table.record_identity(table.size() - 1U);
  const auto removed_value = table[3];
  const auto displaced_value = table.back();
  table.erase_unordered(3);
  assert(table.size() + 1U == before_unordered_erase.size());
  assert(table[3] == displaced_value);
  assert(table.record_identity(3) == displaced_identity);
  assert(before_unordered_erase[3] == removed_value);
  assert(before_unordered_erase.back() == displaced_value);
}

template <typename Table, typename HandleOf>
std::size_t FindRecord(const Table& table, std::uint64_t handle,
                       HandleOf handle_of) {
  for (std::size_t index = 0; index < table.size(); ++index) {
    if (handle_of(table[index]) == handle) {
      return index;
    }
  }
  return table.size();
}

std::size_t FindDraw(
    const merlin::extraction::FrameSnapshot& snapshot,
    merlin::InstanceHandle instance) {
  return FindRecord(snapshot.draws, instance.value(),
                    [](const merlin::extraction::DrawRecord& draw) {
                      return draw.instance;
                    });
}

merlin::MeshDescriptor Triangle() {
  merlin::MeshDescriptor mesh;
  mesh.positions = {{-0.5F, -0.5F, 0.0F}, {0.5F, -0.5F, 0.0F},
                    {0.0F, 0.5F, 0.0F}};
  mesh.indices = {0, 1, 2};
  return mesh;
}

void CheckIncrementalStructuralEdits() {
  merlin::RenderWorld world;
  const auto material_a = world.CreateMaterial({});
  const auto material_b = world.CreateMaterial({});
  const auto material_c = world.CreateMaterial({});
  const auto mesh_a = world.CreateMesh(Triangle());
  const auto mesh_b = world.CreateMesh(Triangle());
  const auto mesh_c = world.CreateMesh(Triangle());
  merlin::InstanceDescriptor descriptor;
  descriptor.mesh = mesh_a;
  descriptor.material = material_a;
  const auto instance_a = world.CreateInstance(descriptor);
  descriptor.mesh = mesh_b;
  descriptor.material = material_b;
  const auto instance_b = world.CreateInstance(descriptor);
  descriptor.mesh = mesh_c;
  descriptor.material = material_c;
  const auto instance_c = world.CreateInstance(descriptor);

  merlin::extraction::SceneExtractor extractor;
  extractor.Apply(world, world.Commit());
  const auto initial = extractor.snapshot();
  const auto draw_a = FindDraw(*initial, instance_a);
  assert(draw_a != initial->draws.size());
  const auto draw_a_identity = initial->draws.record_identity(draw_a);
  const auto mesh_c_identity = initial->geometries.record_identity(2);

  // Dense tables use swap removal. Removing the middle mesh moves only the
  // final geometry record and rebuilds draws that reference those two meshes.
  world.Remove(mesh_b);
  extractor.Apply(world, world.Commit());
  const auto mesh_removed = extractor.snapshot();
  assert(mesh_removed->build_counters.visited_records == 1);
  assert(mesh_removed->build_counters.copied_records == 0);
  assert(mesh_removed->build_counters.rebuilt_draws == 2);
  assert(mesh_removed->build_counters.fully_rebuilt_tables == 0);
  assert(mesh_removed->delta->geometries.upserts ==
         std::vector<std::uint64_t>{mesh_c.value()});
  assert(mesh_removed->delta->geometries.upsert_indices ==
         std::vector<std::uint32_t>{1});
  assert(mesh_removed->geometries.size() == 2);
  assert(mesh_removed->geometries[1].mesh == mesh_c.value());
  assert(mesh_removed->geometries.record_identity(1) == mesh_c_identity);
  assert(initial->geometries[1].mesh == mesh_b.value());
  assert(initial->geometries[2].mesh == mesh_c.value());
  assert(FindDraw(*mesh_removed, instance_b) == mesh_removed->draws.size());
  const auto moved_mesh_draw = FindDraw(*mesh_removed, instance_c);
  assert(moved_mesh_draw != mesh_removed->draws.size());
  assert(mesh_removed->draws[moved_mesh_draw].geometry_index == 1);
  assert(mesh_removed->draws.record_identity(
             FindDraw(*mesh_removed, instance_a)) == draw_a_identity);

  // The same bounded update applies to material indices and their draws.
  const auto material_c_identity = initial->materials.record_identity(2);
  world.Remove(material_b);
  extractor.Apply(world, world.Commit());
  const auto material_removed = extractor.snapshot();
  assert(material_removed->build_counters.visited_records == 1);
  assert(material_removed->build_counters.copied_records == 0);
  assert(material_removed->build_counters.rebuilt_draws == 2);
  assert(material_removed->build_counters.fully_rebuilt_tables == 0);
  assert(material_removed->delta->materials.upserts ==
         std::vector<std::uint64_t>{material_c.value()});
  assert(material_removed->delta->materials.upsert_indices ==
         std::vector<std::uint32_t>{1});
  assert(material_removed->materials.size() == 2);
  assert(material_removed->materials[1].material == material_c.value());
  assert(material_removed->materials.record_identity(1) ==
         material_c_identity);
  const auto moved_material_draw = FindDraw(*material_removed, instance_c);
  assert(moved_material_draw != material_removed->draws.size());
  assert(material_removed->draws[moved_material_draw].material_index == 1);

  // New resources append to the dense tables and rebuild only the new draw.
  const auto material_d = world.CreateMaterial({});
  const auto mesh_d = world.CreateMesh(Triangle());
  descriptor.mesh = mesh_d;
  descriptor.material = material_d;
  const auto instance_d = world.CreateInstance(descriptor);
  extractor.Apply(world, world.Commit());
  const auto added = extractor.snapshot();
  assert(added->build_counters.visited_records == 3);
  assert(added->build_counters.copied_records == 3);
  assert(added->build_counters.rebuilt_draws == 1);
  assert(added->build_counters.fully_rebuilt_tables == 0);
  assert(added->delta->geometries.upsert_indices ==
         std::vector<std::uint32_t>{2});
  assert(added->delta->materials.upsert_indices ==
         std::vector<std::uint32_t>{2});
  assert(added->delta->instances.upsert_indices ==
         std::vector<std::uint32_t>{3});
  const auto added_draw = FindDraw(*added, instance_d);
  assert(added_draw != added->draws.size());
  assert(added->draws[added_draw].geometry_index == 2);
  assert(added->draws[added_draw].material_index == 2);
  assert(added->draws[added_draw].instance_index == 3);

  // Instance swap removal changes only the removed and displaced draws.
  const auto instance_d_identity = added->instances.record_identity(3);
  world.Remove(instance_a);
  extractor.Apply(world, world.Commit());
  const auto instance_removed = extractor.snapshot();
  assert(instance_removed->build_counters.visited_records == 1);
  assert(instance_removed->build_counters.copied_records == 0);
  assert(instance_removed->build_counters.rebuilt_draws == 2);
  assert(instance_removed->build_counters.fully_rebuilt_tables == 0);
  assert(instance_removed->delta->instances.upserts ==
         std::vector<std::uint64_t>{instance_d.value()});
  assert(instance_removed->delta->instances.upsert_indices ==
         std::vector<std::uint32_t>{0});
  assert(FindDraw(*instance_removed, instance_a) ==
         instance_removed->draws.size());
  assert(instance_removed->instances[0].instance == instance_d.value());
  assert(instance_removed->instances.record_identity(0) ==
         instance_d_identity);
  const auto displaced_draw = FindDraw(*instance_removed, instance_d);
  assert(displaced_draw != instance_removed->draws.size());
  assert(instance_removed->draws[displaced_draw].instance_index == 0);
  assert(added->instances[0].instance == instance_a.value());
  assert(added->instances[3].instance == instance_d.value());
}

void CheckLocalizedBindingInvalidation() {
  merlin::RenderWorld world;
  merlin::TextureDescriptor texture;
  texture.width = 1;
  texture.height = 1;
  texture.pixels = {255, 255, 255, 255};
  const auto texture_a = world.CreateTexture(texture);
  const auto texture_b = world.CreateTexture(texture);
  const auto texture_c = world.CreateTexture(texture);
  const auto sampler_a = world.CreateSampler({});
  const auto sampler_b = world.CreateSampler({});
  const auto sampler_c = world.CreateSampler({});

  const auto make_material = [&](merlin::TextureHandle texture_handle,
                                 merlin::SamplerHandle sampler_handle) {
    merlin::MaterialDescriptor material;
    material.features |= merlin::MaterialFeature::BaseColorTexture;
    material.base_color_texture =
        merlin::TextureBinding{texture_handle, sampler_handle, 0};
    return world.CreateMaterial(material);
  };
  const auto material_a = make_material(texture_a, sampler_a);
  const auto material_b = make_material(texture_b, sampler_b);
  const auto material_c = make_material(texture_c, sampler_c);
  const auto unrelated_material = world.CreateMaterial({});

  merlin::extraction::SceneExtractor extractor;
  extractor.Apply(world, world.Commit());
  const auto initial = extractor.snapshot();
  const auto material_a_index = FindRecord(
      initial->materials, material_a.value(),
      [](const merlin::extraction::MaterialRecord& material) {
        return material.material;
      });
  const auto unrelated_index = FindRecord(
      initial->materials, unrelated_material.value(),
      [](const merlin::extraction::MaterialRecord& material) {
        return material.material;
      });
  const auto material_a_identity =
      initial->materials.record_identity(material_a_index);
  const auto unrelated_identity =
      initial->materials.record_identity(unrelated_index);

  world.Remove(texture_b);
  extractor.Apply(world, world.Commit());
  const auto texture_removed = extractor.snapshot();
  assert(texture_removed->delta->materials.upserts ==
         std::vector<std::uint64_t>({material_b.value(),
                                     material_c.value()}));
  assert(texture_removed->delta->materials.upsert_indices ==
         std::vector<std::uint32_t>({1, 2}));
  assert(texture_removed->delta->textures.upserts ==
         std::vector<std::uint64_t>{texture_c.value()});
  assert(texture_removed->delta->textures.upsert_indices ==
         std::vector<std::uint32_t>{1});
  assert(texture_removed->build_counters.visited_records == 3);
  assert(texture_removed->build_counters.copied_records == 3);
  assert(texture_removed->build_counters.fully_rebuilt_tables == 0);
  assert(texture_removed->materials.record_identity(material_a_index) ==
         material_a_identity);
  assert(texture_removed->materials.record_identity(unrelated_index) ==
         unrelated_identity);
  const auto material_b_index = FindRecord(
      texture_removed->materials, material_b.value(),
      [](const merlin::extraction::MaterialRecord& material) {
        return material.material;
      });
  const auto material_c_index = FindRecord(
      texture_removed->materials, material_c.value(),
      [](const merlin::extraction::MaterialRecord& material) {
        return material.material;
      });
  assert(!texture_removed->materials[material_b_index]
              .base_color_texture.has_value());
  assert(texture_removed->materials[material_c_index]
             .base_color_texture->texture_index == 1);
  assert(initial->materials[material_c_index]
             .base_color_texture->texture_index == 2);

  world.Remove(sampler_b);
  extractor.Apply(world, world.Commit());
  const auto sampler_removed = extractor.snapshot();
  assert(sampler_removed->delta->materials.upserts ==
         std::vector<std::uint64_t>({material_b.value(),
                                     material_c.value()}));
  assert(sampler_removed->delta->samplers.upserts ==
         std::vector<std::uint64_t>{sampler_c.value()});
  assert(sampler_removed->delta->samplers.upsert_indices ==
         std::vector<std::uint32_t>{1});
  assert(sampler_removed->build_counters.visited_records == 3);
  assert(sampler_removed->build_counters.copied_records == 3);
  assert(sampler_removed->build_counters.fully_rebuilt_tables == 0);
  assert(sampler_removed->materials.record_identity(material_a_index) ==
         material_a_identity);
  assert(sampler_removed->materials.record_identity(unrelated_index) ==
         unrelated_identity);
  assert(sampler_removed->materials[material_c_index]
             .base_color_texture->sampler_index == 1);
  assert(texture_removed->materials[material_c_index]
             .base_color_texture->sampler_index == 2);
}

}  // namespace

int main() {
  CheckPersistentTable();
  CheckIncrementalStructuralEdits();
  CheckLocalizedBindingInvalidation();
  merlin::RenderWorld world;
  merlin::MeshDescriptor mesh;
  mesh.positions = {{-0.5F, -0.5F, 0.0F}, {0.5F, -0.5F, 0.0F},
                    {0.0F, 0.5F, 0.0F}};
  mesh.indices = {0, 1, 2};
  mesh.normals = {{0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 1.0F},
                  {0.0F, 0.0F, 1.0F}};
  mesh.colors = {{1.0F, 0.0F, 0.0F, 0.5F}, {0.0F, 1.0F, 0.0F, 0.75F},
                 {0.0F, 0.0F, 1.0F, 1.0F}};
  mesh.texcoords = {{0.0F, 0.0F}, {1.0F, 0.0F}, {0.5F, 1.0F}};
  const auto mesh_handle = world.CreateMesh(mesh);
  merlin::MaterialDescriptor material;
  material.parameters.base_color = {0.25F, 0.5F, 0.75F, 1.0F};
  const auto material_handle = world.CreateMaterial(material);
  merlin::InstanceDescriptor instance;
  instance.mesh = mesh_handle;
  instance.material = material_handle;
  const auto instance_handle = world.CreateInstance(instance);
  instance.transform.values[12] = 0.25F;
  const auto second_instance_handle = world.CreateInstance(instance);

  merlin::extraction::SceneExtractor extractor;
  extractor.Apply(world, world.Commit());
  const auto first = extractor.snapshot();
  assert(first->revision == 1);
  assert(first->source_id != 0);
  assert(first->delta.has_value());
  assert(first->delta->base_revision == 0);
  assert(first->delta->geometries.upserts ==
         std::vector<std::uint64_t>{mesh_handle.value()});
  assert(first->delta->instances.upserts.size() == 2);

  // Two instances share one mesh: geometry is recorded once and referenced by
  // both draw records.
  assert(first->geometries.size() == 1);
  assert(first->geometries.front().mesh == mesh_handle.value());
  assert(first->geometries.front().vertices->size() == 3);
  assert(first->geometries.front().indices->size() == 3);
  assert(first->geometries.front().vertices->front().normal.z == 1.0F);
  assert(first->geometries.front().vertices->front().color.w == 0.5F);
  assert(first->geometries.front().vertices->back().texcoord.y == 1.0F);
  assert(first->geometries.front().points_revision == 1);
  assert(first->geometries.front().primvar_revision == 1);
  assert(first->geometries.front().topology_revision == 1);
  assert(first->geometries.front().material_partition_revision == 1);
  assert(first->geometries.front().vertex_revision == 1);
  assert(first->geometries.front().index_revision == 1);
  assert(first->materials.size() == 1);
  assert(first->instances.size() == 2);
  assert(first->instances.front().transform_revision == 1);
  assert(first->instances.front().visibility_revision == 1);
  assert(first->instances.front().material_binding_revision == 1);
  assert(first->draws.size() == 2);
  assert(first->build_counters.visited_records == 6);
  assert(first->build_counters.copied_records == 4);
  assert(first->build_counters.rebuilt_draws == 2);
  assert(first->build_counters.fully_rebuilt_tables == 4);
  assert(first->draws.front().geometry_index == 0);
  assert(first->draws.back().geometry_index == 0);
  assert(first->instances[first->draws.front().instance_index].instance ==
         instance_handle.value());
  assert(first->instances[first->draws.back().instance_index].instance ==
         second_instance_handle.value());
  assert(first->instances[first->draws.back().instance_index]
             .transform.values[12] == 0.25F);
  assert(first->materials[first->draws.front().material_index]
             .parameters.base_color.z == 0.75F);

  // A transform-only edit must not rebuild geometry payloads: the new snapshot
  // shares the same immutable vertex/index arrays.
  instance.transform.values[12] = 0.5F;
  world.UpdateInstance(second_instance_handle, instance,
                       merlin::ChangeAspect::Transform);
  extractor.Apply(world, world.Commit());
  const auto transformed = extractor.snapshot();
  assert(transformed->build_counters.visited_records == 1);
  assert(transformed->build_counters.copied_records == 1);
  assert(transformed->build_counters.rebuilt_draws == 0);
  assert(transformed->build_counters.fully_rebuilt_tables == 0);
  assert(transformed->geometries.record_identity(0) ==
         first->geometries.record_identity(0));
  assert(transformed->instances.record_identity(0) ==
         first->instances.record_identity(0));
  assert(transformed->instances.record_identity(1) !=
         first->instances.record_identity(1));
  assert(transformed->draws.record_identity(0) ==
         first->draws.record_identity(0));
  assert(transformed->draws.record_identity(1) ==
         first->draws.record_identity(1));
  assert(transformed->revision == 2);
  assert(transformed->source_id == first->source_id);
  assert(transformed->delta->base_revision == first->revision);
  assert(transformed->delta->geometries.upserts.empty());
  assert(transformed->delta->geometries.removals.empty());
  assert(transformed->delta->instances.upserts ==
         std::vector<std::uint64_t>{second_instance_handle.value()});
  assert(transformed->geometries.front().vertices ==
         first->geometries.front().vertices);
  assert(transformed->geometries.front().indices ==
         first->geometries.front().indices);
  assert(transformed->geometries.front().points_revision ==
         first->geometries.front().points_revision);
  assert(transformed->instances[transformed->draws.back().instance_index]
             .transform_revision == 2);
  assert(transformed->instances[transformed->draws.back().instance_index]
             .visibility_revision == 1);
  assert(transformed->instances[transformed->draws.back().instance_index]
             .transform.values[12] == 0.5F);
  // The previous snapshot stays immutable across later applies.
  assert(first->instances[first->draws.back().instance_index]
             .transform.values[12] == 0.25F);

  // Points-only mesh edit refreshes the vertex payload but shares topology.
  mesh.positions[0].x = -0.75F;
  world.UpdateMesh(mesh_handle, mesh, merlin::ChangeAspect::Points,
                   std::vector<merlin::ElementRange>{{0, 1}});
  extractor.Apply(world, world.Commit());
  const auto moved = extractor.snapshot();
  assert(moved->build_counters.visited_records == 1);
  assert(moved->build_counters.copied_records == 1);
  assert(moved->build_counters.rebuilt_draws == 0);
  assert(moved->build_counters.fully_rebuilt_tables == 0);
  assert(moved->instances.record_identity(0) ==
         transformed->instances.record_identity(0));
  assert(moved->instances.record_identity(1) ==
         transformed->instances.record_identity(1));
  assert(moved->draws.record_identity(0) ==
         transformed->draws.record_identity(0));
  assert(moved->delta->base_revision == transformed->revision);
  assert(moved->delta->geometries.upserts ==
         std::vector<std::uint64_t>{mesh_handle.value()});
  assert(moved->delta->instances.upserts.empty());
  assert(moved->geometries.front().vertices !=
         transformed->geometries.front().vertices);
  assert(moved->geometries.front().indices ==
         transformed->geometries.front().indices);
  assert(moved->geometries.front().points_revision >
         transformed->geometries.front().points_revision);
  assert(moved->geometries.front().topology_revision ==
         transformed->geometries.front().topology_revision);
  assert(moved->geometries.front().primvar_revision ==
         transformed->geometries.front().primvar_revision);
  assert(moved->geometries.front().vertex_ranges ==
         std::vector<merlin::ElementRange>({{0, 1}}));
  assert(moved->geometries.front().vertex_base_revision ==
         transformed->geometries.front().vertex_revision);
  assert(moved->geometries.front().vertices->front().position.x == -0.75F);

  // Primvar-only edits replace the packed vertex payload while retaining
  // topology and position values.
  mesh.colors[0].x = 0.25F;
  world.UpdateMesh(mesh_handle, mesh, merlin::ChangeAspect::Primvars,
                   std::vector<merlin::ElementRange>{{0, 1}});
  extractor.Apply(world, world.Commit());
  const auto recolored = extractor.snapshot();
  assert(recolored->geometries.front().vertices !=
         moved->geometries.front().vertices);
  assert(recolored->geometries.front().indices ==
         moved->geometries.front().indices);
  assert(recolored->geometries.front().vertices->front().color.x == 0.25F);
  assert(recolored->geometries.front().points_revision ==
         moved->geometries.front().points_revision);
  assert(recolored->geometries.front().primvar_revision >
         moved->geometries.front().primvar_revision);

  // A known-empty topology range advances the authored topology revision but
  // preserves the unchanged derived index payload and its backend revision.
  world.UpdateMesh(mesh_handle, mesh, merlin::ChangeAspect::Topology,
                   std::nullopt, std::vector<merlin::ElementRange>{});
  extractor.Apply(world, world.Commit());
  const auto topology_metadata = extractor.snapshot();
  assert(topology_metadata->geometries.front().topology_revision >
         recolored->geometries.front().topology_revision);
  assert(topology_metadata->geometries.front().index_revision ==
         recolored->geometries.front().index_revision);
  assert(topology_metadata->geometries.front().indices ==
         recolored->geometries.front().indices);

  world.UpdateMesh(mesh_handle, mesh,
                   merlin::ChangeAspect::MaterialPartition);
  extractor.Apply(world, world.Commit());
  const auto partitioned = extractor.snapshot();
  assert(partitioned->geometries.front().material_partition_revision >
         topology_metadata->geometries.front().material_partition_revision);
  assert(partitioned->geometries.front().vertices ==
         topology_metadata->geometries.front().vertices);

  // Visibility-only edit drops the draw but keeps geometry and instance data.
  instance.transform.values[12] = 0.25F;
  instance.visible = false;
  world.UpdateInstance(instance_handle, instance,
                       merlin::ChangeAspect::Visibility |
                           merlin::ChangeAspect::Transform);
  extractor.Apply(world, world.Commit());
  const auto hidden = extractor.snapshot();
  assert(hidden->build_counters.visited_records == 1);
  assert(hidden->build_counters.copied_records == 1);
  assert(hidden->build_counters.rebuilt_draws == 1);
  assert(hidden->build_counters.fully_rebuilt_tables == 0);
  assert(hidden->draws.size() == 1);
  assert(hidden->geometries.size() == 1);
  assert(hidden->instances.size() == 2);
  assert(hidden->geometries.front().vertices ==
         partitioned->geometries.front().vertices);

  instance.visible = true;
  world.UpdateInstance(instance_handle, instance);
  extractor.Apply(world, world.Commit());
  assert(extractor.snapshot()->draws.size() == 2);

  // Removing the mesh retires the geometry record and every dependent draw.
  world.Remove(mesh_handle);
  extractor.Apply(world, world.Commit());
  const auto removed = extractor.snapshot();
  assert(removed->delta->geometries.removals ==
         std::vector<std::uint64_t>{mesh_handle.value()});
  assert(removed->geometries.empty());
  assert(removed->draws.empty());
  assert(removed->instances.size() == 2);

  merlin::CameraDescriptor first_camera;
  first_camera.view.values[12] = 1.0F;
  const auto first_camera_handle = world.CreateCamera(first_camera);
  merlin::CameraDescriptor second_camera;
  second_camera.view.values[12] = 2.0F;
  const auto second_camera_handle = world.CreateCamera(second_camera);
  extractor.Apply(world, world.Commit());
  assert(extractor.snapshot()->view.values[12] == 0.0F);
  extractor.SetActiveCamera(second_camera_handle);
  assert(extractor.snapshot()->view.values[12] == 2.0F);
  extractor.SetActiveCamera(first_camera_handle);
  assert(extractor.snapshot()->view.values[12] == 1.0F);

  bool old_revision_rejected = false;
  try {
    extractor.Apply(world, {1, {}});
  } catch (const std::invalid_argument&) {
    old_revision_rejected = true;
  }
  assert(old_revision_rejected);

  merlin::RenderWorld material_world;
  merlin::TextureDescriptor texture;
  texture.width = 1;
  texture.height = 1;
  texture.pixels = {255, 255, 255, 255};
  const auto texture_handle = material_world.CreateTexture(texture);
  const auto sampler_handle =
      material_world.CreateSampler(merlin::SamplerDescriptor{});
  merlin::MaterialDescriptor blended;
  blended.alpha_mode = merlin::AlphaMode::Blended;
  blended.features |= merlin::MaterialFeature::BaseColorTexture;
  blended.base_color_texture =
      merlin::TextureBinding{texture_handle, sampler_handle, 0};
  const auto blended_handle = material_world.CreateMaterial(blended);
  merlin::LightDescriptor light;
  const auto light_handle = material_world.CreateLight(light);
  merlin::extraction::SceneExtractor material_extractor;
  material_extractor.Apply(material_world, material_world.Commit());
  const auto material_snapshot = material_extractor.snapshot();
  assert(material_snapshot->source_id != first->source_id);
  assert(material_snapshot->delta->textures.upserts ==
         std::vector<std::uint64_t>{texture_handle.value()});
  assert(material_snapshot->delta->samplers.upserts ==
         std::vector<std::uint64_t>{sampler_handle.value()});
  assert(material_snapshot->textures.size() == 1);
  assert(material_snapshot->samplers.size() == 1);
  assert(material_snapshot->materials.size() == 1);
  assert(material_snapshot->materials.front().material == blended_handle.value());
  assert(material_snapshot->materials.front().base_color_texture.has_value());
  assert(material_snapshot->materials.front().alpha_mode ==
         merlin::AlphaMode::Opaque);
  assert(material_snapshot->lights.front().light == light_handle.value());
  assert(material_snapshot->material_fallbacks.front().code ==
         merlin::extraction::MaterialFallbackCode::UnsupportedAlphaBlend);

  blended.parameters.roughness = 0.25F;
  material_world.UpdateMaterial(blended_handle, blended,
                                merlin::ChangeAspect::MaterialParameters);
  material_extractor.Apply(material_world, material_world.Commit());
  const auto parameter_snapshot = material_extractor.snapshot();
  assert(parameter_snapshot->delta->materials.upserts ==
         std::vector<std::uint64_t>{blended_handle.value()});
  assert(parameter_snapshot->delta->textures.upserts.empty());
  assert(parameter_snapshot->materials.front().parameter_revision == 2);
  assert(parameter_snapshot->materials.front().feature_revision == 1);

  material_world.Remove(texture_handle);
  material_extractor.Apply(material_world, material_world.Commit());
  const auto missing_texture = material_extractor.snapshot();
  assert(missing_texture->delta->textures.removals ==
         std::vector<std::uint64_t>{texture_handle.value()});
  assert(missing_texture->delta->materials.upserts ==
         std::vector<std::uint64_t>{blended_handle.value()});
  assert(!missing_texture->materials.front().base_color_texture.has_value());
  assert(std::any_of(
      missing_texture->material_fallbacks.begin(),
      missing_texture->material_fallbacks.end(), [](const auto& fallback) {
        return fallback.code ==
               merlin::extraction::MaterialFallbackCode::MissingTexture;
      }));
  return 0;
}
