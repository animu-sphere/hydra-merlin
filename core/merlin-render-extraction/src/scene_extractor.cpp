#include <merlin/extraction/scene_extractor.hpp>

#include <merlin/core/render_world.hpp>

#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace merlin::extraction {
namespace {

std::atomic<std::uint64_t> g_snapshot_source{1};

std::uint64_t StableSortKey(std::uint64_t material_handle,
                            std::uint64_t mesh_handle) {
  // Opaque-only MVP: material first, then mesh. The instance handle is used as
  // the final tie breaker when the draw list is built.
  return (material_handle * 0x9e3779b185ebca87ULL) ^ mesh_handle;
}

struct MeshEntry {
  std::uint64_t points_revision{};
  std::uint64_t primvar_revision{};
  std::uint64_t topology_revision{};
  std::uint64_t material_partition_revision{};
  std::uint64_t vertex_revision{};
  std::uint64_t index_revision{};
  std::uint64_t vertex_base_revision{};
  std::uint64_t index_base_revision{};
  std::vector<ElementRange> vertex_ranges;
  std::vector<ElementRange> index_ranges;
  std::shared_ptr<const std::vector<DrawVertex>> vertices;
  std::shared_ptr<const std::vector<std::uint32_t>> indices;
  bool has_normals{};
  bool has_colors{};
  bool has_texcoords{};
};

struct MaterialEntry {
  std::uint64_t revision{};
  std::uint64_t parameter_revision{};
  std::uint64_t feature_revision{};
  MaterialDescriptor descriptor;
};

struct TextureEntry {
  std::uint64_t revision{};
  std::uint32_t width{};
  std::uint32_t height{};
  TextureFormat format{TextureFormat::Rgba8Unorm};
  std::shared_ptr<const std::vector<std::uint8_t>> pixels;
};

struct SamplerEntry {
  std::uint64_t revision{};
  SamplerDescriptor descriptor;
};

struct LightEntry {
  std::uint64_t revision{};
  LightDescriptor descriptor;
};

struct InstanceEntry {
  std::uint64_t revision{};
  std::uint64_t transform_revision{};
  std::uint64_t visibility_revision{};
  std::uint64_t material_binding_revision{};
  InstanceDescriptor descriptor;
};

ResourceDelta& DeltaFor(SnapshotDelta& delta, ObjectKind kind) {
  switch (kind) {
    case ObjectKind::Mesh: return delta.geometries;
    case ObjectKind::Material: return delta.materials;
    case ObjectKind::Texture: return delta.textures;
    case ObjectKind::Sampler: return delta.samplers;
    case ObjectKind::Instance: return delta.instances;
    case ObjectKind::Light: return delta.lights;
    case ObjectKind::Camera:
    case ObjectKind::RenderSettings:
      break;
  }
  throw std::logic_error("object kind has no resource delta");
}

void SortAndUnique(ResourceDelta& delta) {
  const auto normalize = [](std::vector<std::uint64_t>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
  };
  normalize(delta.upserts);
  normalize(delta.removals);
  delta.upserts.erase(
      std::remove_if(delta.upserts.begin(), delta.upserts.end(),
                     [&](std::uint64_t handle) {
                       return std::binary_search(delta.removals.begin(),
                                                 delta.removals.end(), handle);
                     }),
      delta.upserts.end());
}

void Normalize(SnapshotDelta& delta) {
  SortAndUnique(delta.geometries);
  SortAndUnique(delta.textures);
  SortAndUnique(delta.samplers);
  SortAndUnique(delta.materials);
  SortAndUnique(delta.instances);
  SortAndUnique(delta.lights);
}

void SetUpsertIndices(
    ResourceDelta& delta,
    const std::map<std::uint64_t, std::size_t>& indices) {
  delta.upsert_indices.clear();
  delta.upsert_indices.reserve(delta.upserts.size());
  for (const auto handle : delta.upserts) {
    const auto found = indices.find(handle);
    if (found == indices.end()) {
      throw std::logic_error("snapshot upsert has no dense table index");
    }
    if (found->second > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("snapshot table exceeds 32-bit dense indices");
    }
    delta.upsert_indices.push_back(
        static_cast<std::uint32_t>(found->second));
  }
}

struct DenseTableUpdate {
  std::vector<std::uint64_t> displaced_handles;
};

struct DirtyDraw {
  std::uint64_t instance{};
  std::optional<std::uint64_t> previous_sort_key;
};

template <typename Table, typename Entries, typename HandleOf,
          typename BuildRecord>
DenseTableUpdate UpdateTable(
    Table& table, std::map<std::uint64_t, std::size_t>& indices,
    const Entries& entries, const ResourceDelta& delta, HandleOf handle_of,
    BuildRecord build_record, SnapshotBuildCounters& counters,
    bool initialize) {
  DenseTableUpdate update;
  if (initialize && !entries.empty()) {
    std::vector<typename Table::value_type> records;
    records.reserve(entries.size());
    indices.clear();
    for (const auto& [handle, entry] : entries) {
      ++counters.visited_records;
      ++counters.copied_records;
      indices.emplace_hint(indices.end(), handle, records.size());
      records.push_back(build_record(handle, entry));
    }
    table.assign(std::move(records));
    ++counters.fully_rebuilt_tables;
    return update;
  }

  for (const auto handle : delta.removals) {
    const auto found = indices.find(handle);
    if (found == indices.end()) {
      // A resource created and removed within one commit never existed in the
      // base snapshot, so there is no dense record to erase.
      continue;
    }
    ++counters.visited_records;
    const auto index = found->second;
    const auto last_index = table.size() - 1U;
    const auto displaced = handle_of(table.back());
    table.erase_unordered(index);
    indices.erase(found);
    if (index != last_index) {
      indices.at(displaced) = index;
      update.displaced_handles.push_back(displaced);
    }
  }

  for (const auto handle : delta.upserts) {
    const auto entry = entries.find(handle);
    if (entry == entries.end()) {
      throw std::logic_error("snapshot upsert references an unknown record");
    }
    ++counters.visited_records;
    ++counters.copied_records;
    const auto index = indices.find(handle);
    if (index == indices.end()) {
      indices.emplace(handle, table.size());
      table.push_back(build_record(handle, entry->second));
    } else {
      table.replace(index->second, build_record(handle, entry->second));
    }
  }
  return update;
}

}  // namespace

class SceneExtractor::Impl {
 public:
  void ApplyMesh(const RenderWorld& world, const Change& change) {
    if (change.change_kind == ChangeKind::Removed) {
      meshes.erase(change.handle);
      return;
    }
    auto& entry = meshes[change.handle];
    entry.vertex_base_revision = entry.vertex_revision;
    entry.index_base_revision = entry.index_revision;
    const auto& descriptor = world.Get(MeshHandle::FromValue(change.handle));
    if (descriptor.positions.size() >
            static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
        descriptor.indices.size() >
            std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("mesh exceeds 32-bit draw limits");
    }
    const bool created = change.change_kind == ChangeKind::Created;
    const bool has_vertex_aspect =
        change.HasAspect(ChangeAspect::Points) ||
        change.HasAspect(ChangeAspect::Primvars) ||
        change.HasAspect(ChangeAspect::VertexLayout);
    if (created || change.HasAspect(ChangeAspect::Points)) {
      entry.points_revision = change.resource_revision;
    }
    if (created || change.HasAspect(ChangeAspect::Primvars)) {
      entry.primvar_revision = change.resource_revision;
    }
    if (created ||
        (has_vertex_aspect &&
         (!change.vertex_ranges_known || !change.vertex_ranges.empty()))) {
      auto vertices = std::make_shared<std::vector<DrawVertex>>();
      vertices->reserve(descriptor.positions.size());
      for (std::size_t i = 0; i < descriptor.positions.size(); ++i) {
        DrawVertex vertex;
        vertex.position = descriptor.positions[i];
        if (!descriptor.normals.empty()) {
          vertex.normal = descriptor.normals[i];
        }
        if (!descriptor.colors.empty()) {
          vertex.color = descriptor.colors[i];
        }
        if (!descriptor.texcoords.empty()) {
          vertex.texcoord = descriptor.texcoords[i];
        }
        vertices->push_back(vertex);
      }
      entry.vertices = std::move(vertices);
      entry.vertex_revision = change.resource_revision;
      entry.vertex_ranges = change.vertex_ranges;
      entry.has_normals = !descriptor.normals.empty();
      entry.has_colors = !descriptor.colors.empty();
      entry.has_texcoords = !descriptor.texcoords.empty();
    }
    if (created || change.HasAspect(ChangeAspect::Topology)) {
      entry.topology_revision = change.resource_revision;
    }
    if (created ||
        (change.HasAspect(ChangeAspect::Topology) &&
         (!change.index_ranges_known || !change.index_ranges.empty()))) {
      entry.indices = std::make_shared<const std::vector<std::uint32_t>>(
          descriptor.indices);
      entry.index_revision = change.resource_revision;
      entry.index_ranges = change.index_ranges;
    }
    if (created ||
        change.HasAspect(ChangeAspect::MaterialPartition)) {
      entry.material_partition_revision = change.resource_revision;
    }
  }

  MaterialRecord BuildMaterial(
      std::uint64_t handle, const MaterialEntry& entry,
      std::vector<MaterialFallbackRecord>& fallbacks) const {
    const auto& material = entry.descriptor;
    MaterialRecord record;
    record.material = handle;
    record.revision = entry.revision;
    record.parameter_revision = entry.parameter_revision;
    record.feature_revision = entry.feature_revision;
    record.parameters = material.parameters;
    record.alpha_mode = material.alpha_mode;
    record.double_sided = material.double_sided;
    record.features = material.features;
    if (record.alpha_mode == AlphaMode::Blended) {
      record.alpha_mode = AlphaMode::Opaque;
      fallbacks.push_back(
          {handle, MaterialFallbackCode::UnsupportedAlphaBlend,
           "alpha blend is unsupported; using opaque fallback"});
    }
    if (material.base_color_texture &&
        HasMaterialFeature(record.features,
                           MaterialFeature::BaseColorTexture)) {
      const auto texture =
          texture_indices.find(material.base_color_texture->texture.value());
      const auto sampler =
          sampler_indices.find(material.base_color_texture->sampler.value());
      if (texture == texture_indices.end()) {
        record.features = static_cast<MaterialFeature>(
            static_cast<std::uint32_t>(record.features) &
            ~static_cast<std::uint32_t>(MaterialFeature::BaseColorTexture));
        fallbacks.push_back(
            {handle, MaterialFallbackCode::MissingTexture,
             "base-color texture is unavailable; using constant color"});
      } else if (sampler == sampler_indices.end()) {
        record.features = static_cast<MaterialFeature>(
            static_cast<std::uint32_t>(record.features) &
            ~static_cast<std::uint32_t>(MaterialFeature::BaseColorTexture));
        fallbacks.push_back(
            {handle, MaterialFallbackCode::MissingSampler,
             "base-color sampler is unavailable; using constant color"});
      } else {
        record.base_color_texture = TextureBindingRecord{
            static_cast<std::uint32_t>(texture->second),
            static_cast<std::uint32_t>(sampler->second),
            material.base_color_texture->texcoord_set};
      }
    }
    return record;
  }

  void EraseMaterialFallbacks(FrameSnapshot& next,
                              std::uint64_t handle) const {
    const auto fallback_index = next.material_fallbacks.lower_bound_index(
        handle,
        [](const MaterialFallbackRecord& candidate, std::uint64_t value) {
          return candidate.material < value;
        });
    auto fallback = next.material_fallbacks.begin() +
                    static_cast<std::ptrdiff_t>(fallback_index);
    while (fallback != next.material_fallbacks.end() &&
           fallback->material == handle) {
      fallback = next.material_fallbacks.erase(fallback);
    }
  }

  void InsertMaterialFallbacks(
      FrameSnapshot& next,
      std::vector<MaterialFallbackRecord> fallbacks) const {
    for (auto& replacement : fallbacks) {
      const auto key = std::tie(replacement.material, replacement.code);
      const auto position_index = next.material_fallbacks.lower_bound_index(
          key, [](const MaterialFallbackRecord& candidate, const auto& key) {
            return std::tie(candidate.material, candidate.code) < key;
          });
      const auto position = next.material_fallbacks.begin() +
                            static_cast<std::ptrdiff_t>(position_index);
      next.material_fallbacks.insert(position, std::move(replacement));
      ++next.build_counters.copied_records;
    }
  }

  DenseTableUpdate UpdateMaterials(FrameSnapshot& next,
                                   const SnapshotDelta& delta,
                                   bool initialize) {
    auto& counters = next.build_counters;
    DenseTableUpdate update;
    if (initialize && !materials.empty()) {
      std::vector<MaterialRecord> records;
      std::vector<MaterialFallbackRecord> fallbacks;
      records.reserve(materials.size());
      material_indices.clear();
      for (const auto& [handle, entry] : materials) {
        ++counters.visited_records;
        ++counters.copied_records;
        material_indices.emplace_hint(material_indices.end(), handle,
                                      records.size());
        records.push_back(BuildMaterial(handle, entry, fallbacks));
      }
      std::stable_sort(
          fallbacks.begin(), fallbacks.end(),
          [](const MaterialFallbackRecord& lhs,
             const MaterialFallbackRecord& rhs) {
            return std::tie(lhs.material, lhs.code) <
                   std::tie(rhs.material, rhs.code);
          });
      counters.copied_records += fallbacks.size();
      next.materials.assign(std::move(records));
      next.material_fallbacks.assign(std::move(fallbacks));
      ++counters.fully_rebuilt_tables;
      return update;
    }

    for (const auto handle : delta.materials.removals) {
      const auto found = material_indices.find(handle);
      if (found == material_indices.end()) {
        continue;
      }
      ++counters.visited_records;
      EraseMaterialFallbacks(next, handle);
      const auto index = found->second;
      const auto last_index = next.materials.size() - 1U;
      const auto displaced = next.materials.back().material;
      next.materials.erase_unordered(index);
      material_indices.erase(found);
      if (index != last_index) {
        material_indices.at(displaced) = index;
        update.displaced_handles.push_back(displaced);
      }
    }

    for (const auto handle : delta.materials.upserts) {
      const auto found = materials.find(handle);
      if (found == materials.end()) {
        throw std::logic_error(
            "snapshot upsert references an unknown material");
      }
      std::vector<MaterialFallbackRecord> fallbacks;
      auto record = BuildMaterial(handle, found->second, fallbacks);
      ++counters.visited_records;
      ++counters.copied_records;
      const auto index = material_indices.find(handle);
      if (index == material_indices.end()) {
        material_indices.emplace(handle, next.materials.size());
        next.materials.push_back(std::move(record));
      } else {
        next.materials.replace(index->second, std::move(record));
      }
      EraseMaterialFallbacks(next, handle);
      InsertMaterialFallbacks(next, std::move(fallbacks));
    }
    return update;
  }

  std::optional<DrawRecord> BuildDraw(
      const InstanceRecord& instance,
      std::optional<std::size_t> known_instance_index = std::nullopt) const {
    if (!instance.visible) {
      return std::nullopt;
    }
    const auto geometry = geometry_indices.find(instance.mesh);
    const auto material = material_indices.find(instance.material);
    if (geometry == geometry_indices.end() ||
        material == material_indices.end()) {
      return std::nullopt;
    }
    std::size_t instance_index{};
    if (known_instance_index) {
      instance_index = *known_instance_index;
    } else {
      const auto found = instance_indices.find(instance.instance);
      if (found == instance_indices.end()) {
        throw std::logic_error("draw references an unknown instance");
      }
      instance_index = found->second;
    }
    return DrawRecord{static_cast<std::uint32_t>(geometry->second),
                      static_cast<std::uint32_t>(material->second),
                      static_cast<std::uint32_t>(instance_index),
                      StableSortKey(instance.material, instance.mesh),
                      instance.instance};
  }

  void UpdateDraws(FrameSnapshot& next, bool initialize,
                   std::vector<DirtyDraw> dirty_instances) const {
    auto& counters = next.build_counters;
    std::stable_sort(
        dirty_instances.begin(), dirty_instances.end(),
        [](const DirtyDraw& lhs, const DirtyDraw& rhs) {
          return lhs.instance < rhs.instance;
        });
    dirty_instances.erase(
        std::unique(dirty_instances.begin(), dirty_instances.end(),
                    [](const DirtyDraw& lhs, const DirtyDraw& rhs) {
                      return lhs.instance == rhs.instance;
                    }),
        dirty_instances.end());
    if (initialize) {
      std::vector<DrawRecord> draws;
      draws.reserve(next.instances.size());
      std::size_t instance_index{};
      for (const auto& instance : next.instances) {
        ++counters.visited_records;
        if (auto draw = BuildDraw(instance, instance_index)) {
          draws.push_back(std::move(*draw));
        }
        ++instance_index;
      }
      std::stable_sort(draws.begin(), draws.end(),
                       [](const DrawRecord& lhs, const DrawRecord& rhs) {
                         return std::tie(lhs.sort_key, lhs.instance) <
                                std::tie(rhs.sort_key, rhs.instance);
                       });
      counters.rebuilt_draws = draws.size();
      next.draws.assign(std::move(draws));
      ++counters.fully_rebuilt_tables;
      return;
    }

    for (const auto& dirty : dirty_instances) {
      if (dirty.previous_sort_key) {
        const auto key = std::tuple{*dirty.previous_sort_key, dirty.instance};
        const auto draw_index = next.draws.lower_bound_index(
            key,
            [](const DrawRecord& candidate, const auto& value) {
              return std::tie(candidate.sort_key, candidate.instance) < value;
            });
        if (draw_index != next.draws.size() &&
            next.draws[draw_index].instance == dirty.instance) {
          next.draws.erase(next.draws.begin() +
                           static_cast<std::ptrdiff_t>(draw_index));
        }
      }

      const auto instance = instance_indices.find(dirty.instance);
      if (instance != instance_indices.end()) {
        if (auto replacement = BuildDraw(next.instances[instance->second])) {
          const auto key =
              std::tie(replacement->sort_key, replacement->instance);
          const auto position_index = next.draws.lower_bound_index(
              key,
              [](const DrawRecord& candidate, const auto& value) {
                return std::tie(candidate.sort_key, candidate.instance) < value;
              });
          const auto position = next.draws.begin() +
                                static_cast<std::ptrdiff_t>(position_index);
          next.draws.insert(position, std::move(*replacement));
        }
      }
      ++counters.rebuilt_draws;
    }
  }

  using DependencyIndex =
      std::map<std::uint64_t, std::set<std::uint64_t>>;

  static void AddDependency(DependencyIndex& dependencies,
                            std::uint64_t resource,
                            std::uint64_t dependent) {
    auto& dependents = dependencies[resource];
    dependents.insert(dependents.end(), dependent);
  }

  static void RemoveDependency(DependencyIndex& dependencies,
                               std::uint64_t resource,
                               std::uint64_t dependent) {
    const auto found = dependencies.find(resource);
    if (found == dependencies.end()) {
      return;
    }
    found->second.erase(dependent);
    if (found->second.empty()) {
      dependencies.erase(found);
    }
  }

  void AddMaterialDependencies(std::uint64_t handle,
                               const MaterialDescriptor& descriptor) {
    if (!descriptor.base_color_texture) {
      return;
    }
    AddDependency(texture_materials,
                  descriptor.base_color_texture->texture.value(), handle);
    AddDependency(sampler_materials,
                  descriptor.base_color_texture->sampler.value(), handle);
  }

  void RemoveMaterialDependencies(std::uint64_t handle,
                                  const MaterialDescriptor& descriptor) {
    if (!descriptor.base_color_texture) {
      return;
    }
    RemoveDependency(texture_materials,
                     descriptor.base_color_texture->texture.value(), handle);
    RemoveDependency(sampler_materials,
                     descriptor.base_color_texture->sampler.value(), handle);
  }

  void AddInstanceDependencies(std::uint64_t handle,
                               const InstanceDescriptor& descriptor) {
    AddDependency(mesh_instances, descriptor.mesh.value(), handle);
    AddDependency(material_instances, descriptor.material.value(), handle);
  }

  void RemoveInstanceDependencies(std::uint64_t handle,
                                  const InstanceDescriptor& descriptor) {
    RemoveDependency(mesh_instances, descriptor.mesh.value(), handle);
    RemoveDependency(material_instances, descriptor.material.value(), handle);
  }

  void AppendDependentMaterials(ResourceDelta& material_delta,
                                const DependencyIndex& dependencies,
                                const std::vector<std::uint64_t>& resources)
      const {
    for (const auto resource : resources) {
      const auto found = dependencies.find(resource);
      if (found == dependencies.end()) {
        continue;
      }
      for (const auto material : found->second) {
        if (materials.contains(material)) {
          material_delta.upserts.push_back(material);
        }
      }
    }
  }

  void AppendDirtyDraw(std::vector<DirtyDraw>& dirty_draws,
                       std::uint64_t handle) const {
    const auto found = instances.find(handle);
    if (found == instances.end()) {
      return;
    }
    const auto& descriptor = found->second.descriptor;
    dirty_draws.push_back(
        {handle, StableSortKey(descriptor.material.value(),
                              descriptor.mesh.value())});
  }

  void AppendDependentDraws(std::vector<DirtyDraw>& dirty_draws,
                            const DependencyIndex& dependencies,
                            const std::vector<std::uint64_t>& resources) const {
    for (const auto resource : resources) {
      const auto found = dependencies.find(resource);
      if (found == dependencies.end()) {
        continue;
      }
      for (const auto instance : found->second) {
        AppendDirtyDraw(dirty_draws, instance);
      }
    }
  }

  void RebuildSnapshot(SnapshotDelta delta,
                       std::vector<DirtyDraw> dirty_instances = {}) {
    const auto previous = snapshot;
    auto next = std::make_shared<FrameSnapshot>(*previous);
    next->source_id = source_id;
    next->revision = revision;
    const bool initialize = previous->source_id != source_id;
    Normalize(delta);
    next->delta = std::move(delta);
    next->build_counters = {};
    const auto geometry_update = UpdateTable(
        next->geometries, geometry_indices, meshes,
        next->delta->geometries,
        [](const GeometryRecord& record) { return record.mesh; },
        [](std::uint64_t handle, const MeshEntry& entry) {
          return GeometryRecord{
              handle,
              entry.points_revision,
              entry.primvar_revision,
              entry.topology_revision,
              entry.material_partition_revision,
              entry.vertex_revision,
              entry.index_revision,
              entry.vertex_base_revision,
              entry.index_base_revision,
              entry.vertex_ranges,
              entry.index_ranges,
              entry.vertices,
              entry.indices,
              entry.has_normals,
              entry.has_colors,
              entry.has_texcoords};
        },
        next->build_counters, initialize);
    next->delta->geometries.upserts.insert(
        next->delta->geometries.upserts.end(),
        geometry_update.displaced_handles.begin(),
        geometry_update.displaced_handles.end());
    SortAndUnique(next->delta->geometries);
    const auto texture_update = UpdateTable(
        next->textures, texture_indices, textures, next->delta->textures,
        [](const TextureRecord& record) { return record.texture; },
        [](std::uint64_t handle, const TextureEntry& entry) {
          return TextureRecord{handle, entry.revision, entry.width,
                               entry.height, entry.format, entry.pixels};
        },
        next->build_counters, initialize);
    next->delta->textures.upserts.insert(
        next->delta->textures.upserts.end(),
        texture_update.displaced_handles.begin(),
        texture_update.displaced_handles.end());
    SortAndUnique(next->delta->textures);
    const auto sampler_update = UpdateTable(
        next->samplers, sampler_indices, samplers, next->delta->samplers,
        [](const SamplerRecord& record) { return record.sampler; },
        [](std::uint64_t handle, const SamplerEntry& entry) {
          const auto& sampler = entry.descriptor;
          return SamplerRecord{handle, entry.revision, sampler.min_filter,
                               sampler.mag_filter, sampler.address_u,
                               sampler.address_v};
        },
        next->build_counters, initialize);
    next->delta->samplers.upserts.insert(
        next->delta->samplers.upserts.end(),
        sampler_update.displaced_handles.begin(),
        sampler_update.displaced_handles.end());
    SortAndUnique(next->delta->samplers);

    AppendDependentMaterials(next->delta->materials, texture_materials,
                             next->delta->textures.removals);
    AppendDependentMaterials(next->delta->materials, texture_materials,
                             texture_update.displaced_handles);
    AppendDependentMaterials(next->delta->materials, sampler_materials,
                             next->delta->samplers.removals);
    AppendDependentMaterials(next->delta->materials, sampler_materials,
                             sampler_update.displaced_handles);
    SortAndUnique(next->delta->materials);
    const auto material_update =
        UpdateMaterials(*next, *next->delta, initialize);
    next->delta->materials.upserts.insert(
        next->delta->materials.upserts.end(),
        material_update.displaced_handles.begin(),
        material_update.displaced_handles.end());
    SortAndUnique(next->delta->materials);
    const auto instance_update = UpdateTable(
        next->instances, instance_indices, instances,
        next->delta->instances,
        [](const InstanceRecord& record) { return record.instance; },
        [](std::uint64_t handle, const InstanceEntry& entry) {
          const auto& instance = entry.descriptor;
          return InstanceRecord{handle,
                                entry.revision,
                                entry.transform_revision,
                                entry.visibility_revision,
                                entry.material_binding_revision,
                                instance.mesh.value(),
                                instance.material.value(),
                                instance.transform,
                                instance.visible};
        },
        next->build_counters, initialize);
    next->delta->instances.upserts.insert(
        next->delta->instances.upserts.end(),
        instance_update.displaced_handles.begin(),
        instance_update.displaced_handles.end());
    SortAndUnique(next->delta->instances);

    AppendDependentDraws(dirty_instances, mesh_instances,
                         next->delta->geometries.removals);
    AppendDependentDraws(dirty_instances, mesh_instances,
                         geometry_update.displaced_handles);
    AppendDependentDraws(dirty_instances, material_instances,
                         next->delta->materials.removals);
    AppendDependentDraws(dirty_instances, material_instances,
                         material_update.displaced_handles);
    for (const auto handle : instance_update.displaced_handles) {
      AppendDirtyDraw(dirty_instances, handle);
    }
    UpdateDraws(*next, initialize, std::move(dirty_instances));

    next->view = {};
    next->projection = {};
    if (active_camera.valid()) {
      const auto camera = cameras.find(active_camera.value());
      if (camera != cameras.end()) {
        next->view = camera->second.view;
        next->projection = camera->second.projection;
      }
    }
    const auto light_update = UpdateTable(
        next->lights, light_indices, lights, next->delta->lights,
        [](const LightRecord& record) { return record.light; },
        [](std::uint64_t handle, const LightEntry& entry) {
          const auto& light = entry.descriptor;
          return LightRecord{handle, entry.revision, light.type, light.color,
                             light.intensity, light.transform};
        },
        next->build_counters, initialize);
    next->delta->lights.upserts.insert(
        next->delta->lights.upserts.end(),
        light_update.displaced_handles.begin(),
        light_update.displaced_handles.end());
    SortAndUnique(next->delta->lights);
    SetUpsertIndices(next->delta->geometries, geometry_indices);
    SetUpsertIndices(next->delta->textures, texture_indices);
    SetUpsertIndices(next->delta->samplers, sampler_indices);
    SetUpsertIndices(next->delta->materials, material_indices);
    SetUpsertIndices(next->delta->instances, instance_indices);
    SetUpsertIndices(next->delta->lights, light_indices);
    snapshot = std::move(next);
  }

  std::map<std::uint64_t, MeshEntry> meshes;
  std::map<std::uint64_t, MaterialEntry> materials;
  std::map<std::uint64_t, TextureEntry> textures;
  std::map<std::uint64_t, SamplerEntry> samplers;
  std::map<std::uint64_t, InstanceEntry> instances;
  std::map<std::uint64_t, CameraDescriptor> cameras;
  std::map<std::uint64_t, LightEntry> lights;
  std::map<std::uint64_t, std::size_t> geometry_indices;
  std::map<std::uint64_t, std::size_t> texture_indices;
  std::map<std::uint64_t, std::size_t> sampler_indices;
  std::map<std::uint64_t, std::size_t> material_indices;
  std::map<std::uint64_t, std::size_t> instance_indices;
  std::map<std::uint64_t, std::size_t> light_indices;
  DependencyIndex texture_materials;
  DependencyIndex sampler_materials;
  DependencyIndex mesh_instances;
  DependencyIndex material_instances;
  CameraHandle active_camera;
  std::uint64_t source_id{g_snapshot_source.fetch_add(
      1, std::memory_order_relaxed)};
  std::uint64_t revision{};
  std::shared_ptr<const FrameSnapshot> snapshot{
      std::make_shared<FrameSnapshot>()};
};

SceneExtractor::SceneExtractor() : impl_(std::make_unique<Impl>()) {}
SceneExtractor::~SceneExtractor() = default;
SceneExtractor::SceneExtractor(SceneExtractor&&) noexcept = default;
SceneExtractor& SceneExtractor::operator=(SceneExtractor&&) noexcept = default;

void SceneExtractor::Apply(const RenderWorld& world, const ChangeSet& changes) {
  if (changes.revision < impl_->revision) {
    throw std::invalid_argument("cannot apply an older RenderWorld revision");
  }
  if (changes.empty()) {
    if (changes.revision != impl_->revision) {
      throw std::invalid_argument("empty ChangeSet skips an extraction revision");
    }
    return;
  }
  if (changes.revision != impl_->revision + 1U) {
    throw std::invalid_argument("ChangeSet revisions must be applied in order");
  }

  SnapshotDelta delta;
  delta.base_revision = impl_->revision;
  std::vector<DirtyDraw> dirty_draw_instances;

  for (const auto& change : changes.changes) {
    const auto erase = change.change_kind == ChangeKind::Removed;
    if (change.object_kind == ObjectKind::Camera) {
      delta.camera_changed = true;
    } else if (change.object_kind == ObjectKind::RenderSettings) {
      delta.render_settings_changed = true;
    } else {
      auto& resources = DeltaFor(delta, change.object_kind);
      (erase ? resources.removals : resources.upserts)
          .push_back(change.handle);
    }
    if (change.object_kind == ObjectKind::Instance &&
        (change.change_kind != ChangeKind::Updated ||
         change.HasAspect(ChangeAspect::Visibility) ||
         change.HasAspect(ChangeAspect::MaterialBinding))) {
      std::optional<std::uint64_t> previous_sort_key;
      const auto found = impl_->instances.find(change.handle);
      if (found != impl_->instances.end()) {
        const auto& descriptor = found->second.descriptor;
        previous_sort_key = StableSortKey(descriptor.material.value(),
                                          descriptor.mesh.value());
      }
      dirty_draw_instances.push_back({change.handle, previous_sort_key});
    }
    switch (change.object_kind) {
      case ObjectKind::Mesh:
        impl_->ApplyMesh(world, change);
        break;
      case ObjectKind::Material:
        if (erase) {
          const auto found = impl_->materials.find(change.handle);
          if (found != impl_->materials.end()) {
            impl_->RemoveMaterialDependencies(change.handle,
                                              found->second.descriptor);
          }
          impl_->materials.erase(change.handle);
        } else {
          auto found = impl_->materials.find(change.handle);
          MaterialEntry entry;
          if (found != impl_->materials.end()) {
            entry = found->second;
            impl_->RemoveMaterialDependencies(change.handle,
                                              found->second.descriptor);
          }
          entry.revision = change.resource_revision;
          if (change.change_kind == ChangeKind::Created ||
              change.HasAspect(ChangeAspect::MaterialParameters)) {
            entry.parameter_revision = change.resource_revision;
          }
          if (change.change_kind == ChangeKind::Created ||
              change.HasAspect(ChangeAspect::MaterialFeatures)) {
            entry.feature_revision = change.resource_revision;
          }
          entry.descriptor =
              world.Get(MaterialHandle::FromValue(change.handle));
          impl_->AddMaterialDependencies(change.handle, entry.descriptor);
          impl_->materials.insert_or_assign(impl_->materials.end(),
                                            change.handle, std::move(entry));
        }
        break;
      case ObjectKind::Texture:
        if (erase) {
          impl_->textures.erase(change.handle);
        } else {
          const auto& texture =
              world.Get(TextureHandle::FromValue(change.handle));
          impl_->textures.insert_or_assign(
              impl_->textures.end(), change.handle,
              TextureEntry{change.resource_revision, texture.width,
                           texture.height, texture.format,
                           std::make_shared<const std::vector<std::uint8_t>>(
                               texture.pixels)});
        }
        break;
      case ObjectKind::Sampler:
        if (erase) {
          impl_->samplers.erase(change.handle);
        } else {
          impl_->samplers.insert_or_assign(
              impl_->samplers.end(), change.handle,
              SamplerEntry{change.resource_revision,
                           world.Get(SamplerHandle::FromValue(change.handle))});
        }
        break;
      case ObjectKind::Instance:
        if (erase) {
          const auto found = impl_->instances.find(change.handle);
          if (found != impl_->instances.end()) {
            impl_->RemoveInstanceDependencies(change.handle,
                                              found->second.descriptor);
          }
          impl_->instances.erase(change.handle);
        } else {
          auto found = impl_->instances.find(change.handle);
          InstanceEntry entry;
          if (found != impl_->instances.end()) {
            entry = found->second;
            impl_->RemoveInstanceDependencies(change.handle,
                                              found->second.descriptor);
          }
          entry.revision = change.resource_revision;
          if (change.change_kind == ChangeKind::Created ||
              change.HasAspect(ChangeAspect::Transform)) {
            entry.transform_revision = change.resource_revision;
          }
          if (change.change_kind == ChangeKind::Created ||
              change.HasAspect(ChangeAspect::Visibility)) {
            entry.visibility_revision = change.resource_revision;
          }
          if (change.change_kind == ChangeKind::Created ||
              change.HasAspect(ChangeAspect::MaterialBinding)) {
            entry.material_binding_revision = change.resource_revision;
          }
          entry.descriptor =
              world.Get(InstanceHandle::FromValue(change.handle));
          impl_->AddInstanceDependencies(change.handle, entry.descriptor);
          impl_->instances.insert_or_assign(
              impl_->instances.end(), change.handle, std::move(entry));
        }
        break;
      case ObjectKind::Camera:
        if (erase) {
          impl_->cameras.erase(change.handle);
        } else {
          impl_->cameras.insert_or_assign(
              impl_->cameras.end(), change.handle,
              world.Get(CameraHandle::FromValue(change.handle)));
        }
        break;
      case ObjectKind::Light:
        if (erase) {
          impl_->lights.erase(change.handle);
        } else {
          impl_->lights.insert_or_assign(
              impl_->lights.end(), change.handle,
              LightEntry{change.resource_revision,
                         world.Get(LightHandle::FromValue(change.handle))});
        }
        break;
      case ObjectKind::RenderSettings:
        // Render settings are frame/host state and are consumed outside the
        // extracted draw model until render requests are introduced.
        break;
    }
  }

  impl_->revision = changes.revision;
  impl_->RebuildSnapshot(std::move(delta), std::move(dirty_draw_instances));
}

void SceneExtractor::SetActiveCamera(CameraHandle camera) {
  if (impl_->active_camera == camera) {
    return;
  }
  impl_->active_camera = camera;
  SnapshotDelta delta;
  delta.base_revision = impl_->revision;
  delta.camera_changed = true;
  impl_->RebuildSnapshot(std::move(delta));
}

std::shared_ptr<const FrameSnapshot> SceneExtractor::snapshot() const noexcept {
  return impl_->snapshot;
}

}  // namespace merlin::extraction
