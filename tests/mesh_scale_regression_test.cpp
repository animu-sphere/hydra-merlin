#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

namespace {

merlin::MeshDescriptor Triangle() {
  merlin::MeshDescriptor mesh;
  mesh.positions = {{-0.5F, -0.5F, 0.0F}, {0.5F, -0.5F, 0.0F},
                    {0.0F, 0.5F, 0.0F}};
  mesh.colors = {{1.0F, 1.0F, 1.0F, 1.0F},
                 {1.0F, 1.0F, 1.0F, 1.0F},
                 {1.0F, 1.0F, 1.0F, 1.0F}};
  mesh.indices = {0, 1, 2};
  return mesh;
}

}  // namespace

int main() {
  // One million indexed triangles exercise 32-bit topology and packed vertex
  // extraction without requiring three million duplicate points.
  {
    constexpr std::uint32_t columns = 1000;
    constexpr std::uint32_t rows = 500;
    merlin::MeshDescriptor mesh;
    mesh.positions.reserve(static_cast<std::size_t>(columns + 1U) *
                           (rows + 1U));
    for (std::uint32_t y = 0; y <= rows; ++y) {
      for (std::uint32_t x = 0; x <= columns; ++x) {
        mesh.positions.push_back(
            {static_cast<float>(x), static_cast<float>(y), 0.0F});
      }
    }
    mesh.indices.reserve(static_cast<std::size_t>(columns) * rows * 6U);
    for (std::uint32_t y = 0; y < rows; ++y) {
      for (std::uint32_t x = 0; x < columns; ++x) {
        const auto a = y * (columns + 1U) + x;
        const auto b = a + 1U;
        const auto c = a + columns + 1U;
        const auto d = c + 1U;
        mesh.indices.insert(mesh.indices.end(), {a, b, d, a, d, c});
      }
    }
    merlin::RenderWorld world;
    const auto mesh_handle = world.CreateMesh(std::move(mesh));
    const auto material = world.CreateMaterial({});
    merlin::InstanceDescriptor instance;
    instance.mesh = mesh_handle;
    instance.material = material;
    world.CreateInstance(instance);
    merlin::extraction::SceneExtractor extractor;
    extractor.Apply(world, world.Commit());
    const auto snapshot = extractor.snapshot();
    assert(snapshot->geometries.size() == 1);
    assert(snapshot->geometries.front().indices->size() == 3'000'000U);
    assert(snapshot->draws.size() == 1);
  }

  // Ten thousand independently keyed small meshes cover deterministic map and
  // draw-list construction for DCC scenes dominated by small objects.
  {
    constexpr std::uint32_t mesh_count = 10'000;
    merlin::RenderWorld world;
    const auto material = world.CreateMaterial({});
    std::vector<merlin::InstanceHandle> instances;
    instances.reserve(mesh_count);
    for (std::uint32_t i = 0; i < mesh_count; ++i) {
      const auto mesh = world.CreateMesh(Triangle());
      merlin::InstanceDescriptor instance;
      instance.mesh = mesh;
      instance.material = material;
      instance.transform.values[12] = static_cast<float>(i);
      instances.push_back(world.CreateInstance(instance));
    }
    merlin::extraction::SceneExtractor extractor;
    extractor.Apply(world, world.Commit());
    const auto before = extractor.snapshot();
    assert(before->geometries.size() == mesh_count);
    assert(before->draws.size() == mesh_count);

    // A localized transform edit copies one instance record and shares all
    // geometry, material, and draw records independent of total scene size.
    constexpr std::size_t changed_index = mesh_count / 2U;
    auto changed = world.Get(instances[changed_index]);
    changed.transform.values[13] = 1.0F;
    world.UpdateInstance(instances[changed_index], changed,
                         merlin::ChangeAspect::Transform);
    extractor.Apply(world, world.Commit());
    const auto after = extractor.snapshot();
    assert(after->build_counters.visited_records == 1);
    assert(after->build_counters.copied_records == 1);
    assert(after->build_counters.rebuilt_draws == 0);
    assert(after->build_counters.fully_rebuilt_tables == 0);
    assert(after->geometries.record_identity(0) ==
           before->geometries.record_identity(0));
    assert(after->geometries.record_identity(mesh_count - 1U) ==
           before->geometries.record_identity(mesh_count - 1U));
    assert(after->instances.record_identity(changed_index) !=
           before->instances.record_identity(changed_index));
    assert(after->instances.record_identity(0) ==
           before->instances.record_identity(0));
    assert(after->draws.record_identity(0) ==
           before->draws.record_identity(0));
    assert(after->draws.record_identity(mesh_count - 1U) ==
           before->draws.record_identity(mesh_count - 1U));
  }

  // One million independently handled prims sharing immutable geometry cover
  // localized snapshot work at production scene cardinality. The first batch
  // changes 100 transforms; the second replaces 50 prims, for 100 structural
  // edits split evenly between removals and additions while retaining exactly
  // one million live prims.
  {
    constexpr std::size_t prim_count = 1'000'000;
    constexpr std::size_t localized_edit_count = 100;
    constexpr std::size_t replacement_count = 50;
    merlin::RenderWorld world;
    const auto mesh = world.CreateMesh(Triangle());
    merlin::TextureDescriptor texture_descriptor;
    texture_descriptor.width = 1;
    texture_descriptor.height = 1;
    texture_descriptor.pixels = {255, 255, 255, 255};
    const auto texture = world.CreateTexture(texture_descriptor);
    merlin::SamplerDescriptor sampler_descriptor;
    const auto sampler = world.CreateSampler(sampler_descriptor);
    merlin::MaterialDescriptor material_descriptor;
    material_descriptor.features = merlin::MaterialFeature::BaseColorTexture;
    material_descriptor.base_color_texture =
        merlin::TextureBinding{texture, sampler, 0};
    const auto material = world.CreateMaterial(material_descriptor);
    merlin::InstanceDescriptor descriptor;
    descriptor.mesh = mesh;
    descriptor.material = material;
    std::vector<merlin::InstanceHandle> instances;
    instances.reserve(prim_count);
    for (std::size_t index = 0; index < prim_count; ++index) {
      instances.push_back(world.CreateInstance(descriptor));
    }

    merlin::extraction::SceneExtractor extractor;
    extractor.Apply(world, world.Commit());
    const auto initial = extractor.snapshot();
    assert(initial->geometries.size() == 1U);
    assert(initial->instances.size() == prim_count);
    assert(initial->draws.size() == prim_count);

    // Texture and sampler values are resource-granular: changing either one
    // must not visit the million dependent instances or rebuild their draws.
    texture_descriptor.pixels = {64, 128, 255, 255};
    world.UpdateTexture(texture, texture_descriptor);
    extractor.Apply(world, world.Commit());
    const auto texture_edit = extractor.snapshot();
    assert(texture_edit->build_counters.visited_records == 1U);
    assert(texture_edit->build_counters.copied_records == 1U);
    assert(texture_edit->build_counters.rebuilt_draws == 0U);
    assert(texture_edit->build_counters.fully_rebuilt_tables == 0U);
    assert(texture_edit->delta->textures.upserts ==
           std::vector<std::uint64_t>{texture.value()});
    assert(texture_edit->delta->materials.upserts.empty());
    assert(texture_edit->delta->instances.upserts.empty());
    assert(texture_edit->instances.record_identity(0) ==
           initial->instances.record_identity(0));
    assert(texture_edit->instances.record_identity(prim_count - 1U) ==
           initial->instances.record_identity(prim_count - 1U));
    assert(texture_edit->draws.record_identity(0) ==
           initial->draws.record_identity(0));
    assert(texture_edit->draws.record_identity(prim_count - 1U) ==
           initial->draws.record_identity(prim_count - 1U));

    sampler_descriptor.min_filter = merlin::FilterMode::Nearest;
    sampler_descriptor.mag_filter = merlin::FilterMode::Nearest;
    world.UpdateSampler(sampler, sampler_descriptor);
    extractor.Apply(world, world.Commit());
    const auto sampler_edit = extractor.snapshot();
    assert(sampler_edit->build_counters.visited_records == 1U);
    assert(sampler_edit->build_counters.copied_records == 1U);
    assert(sampler_edit->build_counters.rebuilt_draws == 0U);
    assert(sampler_edit->build_counters.fully_rebuilt_tables == 0U);
    assert(sampler_edit->delta->samplers.upserts ==
           std::vector<std::uint64_t>{sampler.value()});
    assert(sampler_edit->delta->materials.upserts.empty());
    assert(sampler_edit->delta->instances.upserts.empty());
    assert(sampler_edit->instances.record_identity(0) ==
           texture_edit->instances.record_identity(0));
    assert(sampler_edit->instances.record_identity(prim_count - 1U) ==
           texture_edit->instances.record_identity(prim_count - 1U));
    assert(sampler_edit->draws.record_identity(0) ==
           texture_edit->draws.record_identity(0));
    assert(sampler_edit->draws.record_identity(prim_count - 1U) ==
           texture_edit->draws.record_identity(prim_count - 1U));

    std::vector<std::size_t> edited_indices;
    edited_indices.reserve(localized_edit_count);
    for (std::size_t edit = 0; edit < localized_edit_count; ++edit) {
      const auto index = prim_count / (localized_edit_count * 2U) +
                         edit * (prim_count / localized_edit_count);
      edited_indices.push_back(index);
      auto changed = world.Get(instances[index]);
      changed.transform.values[13] = 1.0F;
      world.UpdateInstance(instances[index], changed,
                           merlin::ChangeAspect::Transform);
    }
    extractor.Apply(world, world.Commit());
    const auto localized = extractor.snapshot();
    assert(localized->instances.size() == prim_count);
    assert(localized->draws.size() == prim_count);
    assert(localized->build_counters.visited_records ==
           localized_edit_count);
    assert(localized->build_counters.copied_records ==
           localized_edit_count);
    assert(localized->build_counters.rebuilt_draws == 0U);
    assert(localized->build_counters.fully_rebuilt_tables == 0U);
    assert(localized->delta->instances.upserts.size() ==
           localized_edit_count);
    assert(localized->delta->instances.removals.empty());
    for (std::size_t edit = 0; edit < localized_edit_count; ++edit) {
      const auto index = edited_indices[edit];
      assert(localized->delta->instances.upsert_indices[edit] == index);
      assert(localized->instances.record_identity(index) !=
             initial->instances.record_identity(index));
      assert(localized->draws.record_identity(index) ==
             initial->draws.record_identity(index));
    }
    assert(localized->instances.record_identity(0) ==
           initial->instances.record_identity(0));
    assert(localized->instances.record_identity(prim_count - 1U) ==
           initial->instances.record_identity(prim_count - 1U));

    std::vector<merlin::InstanceHandle> removed_instances;
    removed_instances.reserve(replacement_count);
    for (std::size_t replacement = 0; replacement < replacement_count;
         ++replacement) {
      const auto index = prim_count / (replacement_count * 4U) +
                         replacement * (prim_count / replacement_count);
      removed_instances.push_back(instances[index]);
      world.Remove(instances[index]);
    }
    std::vector<merlin::InstanceHandle> added_instances;
    added_instances.reserve(replacement_count);
    for (std::size_t replacement = 0; replacement < replacement_count;
         ++replacement) {
      added_instances.push_back(world.CreateInstance(descriptor));
    }
    extractor.Apply(world, world.Commit());
    const auto structural = extractor.snapshot();
    assert(structural->instances.size() == prim_count);
    assert(structural->draws.size() == prim_count);
    assert(structural->build_counters.visited_records ==
           replacement_count * 2U);
    assert(structural->build_counters.copied_records == replacement_count);
    assert(structural->build_counters.rebuilt_draws ==
           replacement_count * 3U);
    assert(structural->build_counters.fully_rebuilt_tables == 0U);
    assert(structural->delta->instances.removals.size() == replacement_count);
    // Dense swap removal also publishes the 50 displaced records alongside
    // the 50 authored additions so consumers can reconcile their new indices.
    assert(structural->delta->instances.upserts.size() ==
           replacement_count * 2U);
    for (const auto removed : removed_instances) {
      assert(std::binary_search(structural->delta->instances.removals.begin(),
                                structural->delta->instances.removals.end(),
                                removed.value()));
    }
    for (const auto added : added_instances) {
      assert(std::binary_search(structural->delta->instances.upserts.begin(),
                                structural->delta->instances.upserts.end(),
                                added.value()));
    }
    for (std::size_t upsert = 0;
         upsert < structural->delta->instances.upserts.size(); ++upsert) {
      const auto dense_index =
          structural->delta->instances.upsert_indices[upsert];
      assert(structural->instances[dense_index].instance ==
             structural->delta->instances.upserts[upsert]);
    }
    // Unaffected records and draws keep their identities, and displaced tail
    // records retain identity even though their dense indices changed.
    assert(structural->instances.record_identity(0) ==
           localized->instances.record_identity(0));
    assert(structural->draws.record_identity(0) ==
           localized->draws.record_identity(0));
    for (std::size_t displaced = 0; displaced < replacement_count;
         ++displaced) {
      const auto previous_index = prim_count - replacement_count + displaced;
      const auto handle = instances[previous_index].value();
      const auto found = std::lower_bound(
          structural->delta->instances.upserts.begin(),
          structural->delta->instances.upserts.end(), handle);
      assert(found != structural->delta->instances.upserts.end());
      assert(*found == handle);
      const auto upsert = static_cast<std::size_t>(
          found - structural->delta->instances.upserts.begin());
      const auto current_index =
          structural->delta->instances.upsert_indices[upsert];
      assert(structural->instances.record_identity(current_index) ==
             localized->instances.record_identity(previous_index));
    }
    // The prior million-prim snapshot remains immutable.
    assert(localized->instances.size() == prim_count);
    assert(localized->instances[edited_indices.front()].instance ==
           instances[edited_indices.front()].value());
  }

  // Repeated primvar edits must replace only the vertex payload and preserve
  // the immutable topology payload across every revision.
  {
    merlin::RenderWorld world;
    auto descriptor = Triangle();
    const auto mesh = world.CreateMesh(descriptor);
    merlin::extraction::SceneExtractor extractor;
    extractor.Apply(world, world.Commit());
    auto previous = extractor.snapshot();
    for (std::uint32_t revision = 0; revision < 256; ++revision) {
      descriptor.colors[0].x = static_cast<float>(revision) / 255.0F;
      world.UpdateMesh(mesh, descriptor, merlin::ChangeAspect::Primvars);
      extractor.Apply(world, world.Commit());
      const auto current = extractor.snapshot();
      assert(current->geometries.front().indices ==
             previous->geometries.front().indices);
      assert(current->geometries.front().vertices !=
             previous->geometries.front().vertices);
      previous = current;
    }
    assert(previous->revision == 257);
  }
  return 0;
}
