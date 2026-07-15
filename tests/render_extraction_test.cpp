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
}

}  // namespace

int main() {
  CheckPersistentTable();
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
