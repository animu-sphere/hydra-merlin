#include <merlin/extraction/scene_extractor.hpp>

#include <merlin/core/render_world.hpp>

#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <optional>
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

template <typename Table, typename HandleOf>
std::optional<std::size_t> FindRecordIndex(const Table& table,
                                           std::uint64_t handle,
                                           HandleOf handle_of) {
  const auto found = table.lower_bound_index(
      handle, [&](const auto& record, std::uint64_t candidate) {
        return handle_of(record) < candidate;
      });
  if (found == table.size() || handle_of(table[found]) != handle) {
    return std::nullopt;
  }
  return found;
}

template <typename Table, typename Entries, typename HandleOf,
          typename BuildRecord>
bool UpdateTable(Table& table, const Entries& entries,
                 const ResourceDelta& delta, HandleOf handle_of,
                 BuildRecord build_record, SnapshotBuildCounters& counters) {
  bool full_rebuild = !delta.removals.empty() || table.size() != entries.size();
  if (!full_rebuild) {
    for (const auto handle : delta.upserts) {
      if (!FindRecordIndex(table, handle, handle_of).has_value() ||
          entries.find(handle) == entries.end()) {
        full_rebuild = true;
        break;
      }
    }
  }

  if (full_rebuild) {
    std::vector<typename Table::value_type> records;
    records.reserve(entries.size());
    for (const auto& [handle, entry] : entries) {
      ++counters.visited_records;
      ++counters.copied_records;
      records.push_back(build_record(handle, entry));
    }
    table.assign(std::move(records));
    ++counters.fully_rebuilt_tables;
    return true;
  }

  for (const auto handle : delta.upserts) {
    const auto entry = entries.find(handle);
    const auto index = FindRecordIndex(table, handle, handle_of);
    ++counters.visited_records;
    ++counters.copied_records;
    table.replace(*index, build_record(handle, entry->second));
  }
  return false;
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
      const PersistentTable<TextureRecord>& texture_records,
      const PersistentTable<SamplerRecord>& sampler_records,
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
      const auto texture = FindRecordIndex(
          texture_records, material.base_color_texture->texture.value(),
          [](const TextureRecord& candidate) { return candidate.texture; });
      const auto sampler = FindRecordIndex(
          sampler_records, material.base_color_texture->sampler.value(),
          [](const SamplerRecord& candidate) { return candidate.sampler; });
      if (!texture) {
        record.features = static_cast<MaterialFeature>(
            static_cast<std::uint32_t>(record.features) &
            ~static_cast<std::uint32_t>(MaterialFeature::BaseColorTexture));
        fallbacks.push_back(
            {handle, MaterialFallbackCode::MissingTexture,
             "base-color texture is unavailable; using constant color"});
      } else if (!sampler) {
        record.features = static_cast<MaterialFeature>(
            static_cast<std::uint32_t>(record.features) &
            ~static_cast<std::uint32_t>(MaterialFeature::BaseColorTexture));
        fallbacks.push_back(
            {handle, MaterialFallbackCode::MissingSampler,
             "base-color sampler is unavailable; using constant color"});
      } else {
        record.base_color_texture = TextureBindingRecord{
            static_cast<std::uint32_t>(*texture),
            static_cast<std::uint32_t>(*sampler),
            material.base_color_texture->texcoord_set};
      }
    }
    return record;
  }

  bool UpdateMaterials(FrameSnapshot& next, const SnapshotDelta& delta) const {
    auto& counters = next.build_counters;
    bool full_rebuild = !delta.materials.removals.empty() ||
                        next.materials.size() != materials.size();
    if (!full_rebuild) {
      for (const auto handle : delta.materials.upserts) {
        if (!FindRecordIndex(
                 next.materials, handle,
                 [](const MaterialRecord& record) { return record.material; }) ||
            materials.find(handle) == materials.end()) {
          full_rebuild = true;
          break;
        }
      }
    }

    if (full_rebuild) {
      std::vector<MaterialRecord> records;
      std::vector<MaterialFallbackRecord> fallbacks;
      records.reserve(materials.size());
      for (const auto& [handle, entry] : materials) {
        ++counters.visited_records;
        ++counters.copied_records;
        records.push_back(BuildMaterial(handle, entry, next.textures,
                                        next.samplers, fallbacks));
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
      return true;
    }

    for (const auto handle : delta.materials.upserts) {
      const auto found = materials.find(handle);
      const auto index = FindRecordIndex(
          next.materials, handle,
          [](const MaterialRecord& record) { return record.material; });
      std::vector<MaterialFallbackRecord> fallbacks;
      auto record = BuildMaterial(handle, found->second, next.textures,
                                  next.samplers, fallbacks);
      ++counters.visited_records;
      ++counters.copied_records;
      next.materials.replace(*index, std::move(record));

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
      for (auto& replacement : fallbacks) {
        const auto key = std::tie(replacement.material, replacement.code);
        const auto position_index = next.material_fallbacks.lower_bound_index(
            key,
            [](const MaterialFallbackRecord& candidate, const auto& key) {
              return std::tie(candidate.material, candidate.code) < key;
            });
        const auto position = next.material_fallbacks.begin() +
                              static_cast<std::ptrdiff_t>(position_index);
        next.material_fallbacks.insert(position, std::move(replacement));
        ++counters.copied_records;
      }
    }
    return false;
  }

  std::optional<DrawRecord> BuildDraw(const FrameSnapshot& next,
                                      const InstanceRecord& instance) const {
    if (!instance.visible) {
      return std::nullopt;
    }
    const auto geometry = FindRecordIndex(
        next.geometries, instance.mesh,
        [](const GeometryRecord& record) { return record.mesh; });
    const auto material = FindRecordIndex(
        next.materials, instance.material,
        [](const MaterialRecord& record) { return record.material; });
    if (!geometry || !material) {
      return std::nullopt;
    }
    const auto instance_index = FindRecordIndex(
        next.instances, instance.instance,
        [](const InstanceRecord& record) { return record.instance; });
    return DrawRecord{static_cast<std::uint32_t>(*geometry),
                      static_cast<std::uint32_t>(*material),
                      static_cast<std::uint32_t>(*instance_index),
                      StableSortKey(instance.material, instance.mesh),
                      instance.instance};
  }

  void UpdateDraws(FrameSnapshot& next, const FrameSnapshot& previous,
                   bool resource_indices_changed,
                   std::vector<std::uint64_t> dirty_instances) const {
    auto& counters = next.build_counters;
    std::sort(dirty_instances.begin(), dirty_instances.end());
    dirty_instances.erase(
        std::unique(dirty_instances.begin(), dirty_instances.end()),
        dirty_instances.end());
    if (resource_indices_changed || previous.source_id != source_id) {
      std::vector<DrawRecord> draws;
      draws.reserve(next.instances.size());
      for (const auto& instance : next.instances) {
        ++counters.visited_records;
        if (auto draw = BuildDraw(next, instance)) {
          draws.push_back(std::move(*draw));
        }
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

    for (const auto handle : dirty_instances) {
      const auto old_instance = FindRecordIndex(
          previous.instances, handle,
          [](const InstanceRecord& record) { return record.instance; });
      if (old_instance) {
        const auto& record = previous.instances[*old_instance];
        const auto key =
            std::tuple{StableSortKey(record.material, record.mesh), handle};
        const auto draw_index = next.draws.lower_bound_index(
            key,
            [](const DrawRecord& candidate, const auto& value) {
              return std::tie(candidate.sort_key, candidate.instance) < value;
            });
        if (draw_index != next.draws.size() &&
            next.draws[draw_index].instance == handle) {
          next.draws.erase(next.draws.begin() +
                           static_cast<std::ptrdiff_t>(draw_index));
        }
      }

      const auto instance = FindRecordIndex(
          next.instances, handle,
          [](const InstanceRecord& record) { return record.instance; });
      if (instance) {
        if (auto replacement = BuildDraw(next, next.instances[*instance])) {
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

  void RebuildSnapshot(SnapshotDelta delta,
                       std::vector<std::uint64_t> dirty_instances = {}) {
    const auto previous = snapshot;
    auto next = std::make_shared<FrameSnapshot>(*previous);
    next->source_id = source_id;
    next->revision = revision;
    Normalize(delta);
    next->delta = std::move(delta);
    next->build_counters = {};
    const auto geometry_indices_changed = UpdateTable(
        next->geometries, meshes, next->delta->geometries,
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
        next->build_counters);
    UpdateTable(
        next->textures, textures, next->delta->textures,
        [](const TextureRecord& record) { return record.texture; },
        [](std::uint64_t handle, const TextureEntry& entry) {
          return TextureRecord{handle, entry.revision, entry.width,
                               entry.height, entry.format, entry.pixels};
        },
        next->build_counters);
    UpdateTable(
        next->samplers, samplers, next->delta->samplers,
        [](const SamplerRecord& record) { return record.sampler; },
        [](std::uint64_t handle, const SamplerEntry& entry) {
          const auto& sampler = entry.descriptor;
          return SamplerRecord{handle, entry.revision, sampler.min_filter,
                               sampler.mag_filter, sampler.address_u,
                               sampler.address_v};
        },
        next->build_counters);
    const auto material_indices_changed = UpdateMaterials(*next, *next->delta);
    const auto instance_indices_changed = UpdateTable(
        next->instances, instances, next->delta->instances,
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
        next->build_counters);
    UpdateDraws(*next, *previous,
                geometry_indices_changed || material_indices_changed ||
                    instance_indices_changed,
                std::move(dirty_instances));

    next->view = {};
    next->projection = {};
    if (active_camera.valid()) {
      const auto camera = cameras.find(active_camera.value());
      if (camera != cameras.end()) {
        next->view = camera->second.view;
        next->projection = camera->second.projection;
      }
    }
    UpdateTable(
        next->lights, lights, next->delta->lights,
        [](const LightRecord& record) { return record.light; },
        [](std::uint64_t handle, const LightEntry& entry) {
          const auto& light = entry.descriptor;
          return LightRecord{handle, entry.revision, light.type, light.color,
                             light.intensity, light.transform};
        },
        next->build_counters);
    snapshot = std::move(next);
  }

  std::map<std::uint64_t, MeshEntry> meshes;
  std::map<std::uint64_t, MaterialEntry> materials;
  std::map<std::uint64_t, TextureEntry> textures;
  std::map<std::uint64_t, SamplerEntry> samplers;
  std::map<std::uint64_t, InstanceEntry> instances;
  std::map<std::uint64_t, CameraDescriptor> cameras;
  std::map<std::uint64_t, LightEntry> lights;
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
  bool material_indices_changed{};
  std::vector<std::uint64_t> dirty_draw_instances;

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
    if ((change.object_kind == ObjectKind::Texture ||
         change.object_kind == ObjectKind::Sampler) &&
        change.change_kind != ChangeKind::Updated) {
      // Material bindings currently store dense texture/sampler indices. A
      // structural table edit can shift those indices even when the authored
      // material itself did not change. Bindless stable slots remove this
      // conservative dependency in the next v0.7.0 slice.
      material_indices_changed = true;
    }
    if (change.object_kind == ObjectKind::Instance &&
        (change.change_kind != ChangeKind::Updated ||
         change.HasAspect(ChangeAspect::Visibility) ||
         change.HasAspect(ChangeAspect::MaterialBinding))) {
      dirty_draw_instances.push_back(change.handle);
    }
    switch (change.object_kind) {
      case ObjectKind::Mesh:
        impl_->ApplyMesh(world, change);
        break;
      case ObjectKind::Material:
        if (erase) {
          impl_->materials.erase(change.handle);
        } else {
          auto found = impl_->materials.find(change.handle);
          MaterialEntry entry;
          if (found != impl_->materials.end()) {
            entry = found->second;
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
          impl_->materials.insert_or_assign(change.handle, std::move(entry));
        }
        break;
      case ObjectKind::Texture:
        if (erase) {
          impl_->textures.erase(change.handle);
        } else {
          const auto& texture =
              world.Get(TextureHandle::FromValue(change.handle));
          impl_->textures.insert_or_assign(
              change.handle,
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
              change.handle,
              SamplerEntry{change.resource_revision,
                           world.Get(SamplerHandle::FromValue(change.handle))});
        }
        break;
      case ObjectKind::Instance:
        if (erase) {
          impl_->instances.erase(change.handle);
        } else {
          auto found = impl_->instances.find(change.handle);
          InstanceEntry entry;
          if (found != impl_->instances.end()) {
            entry = found->second;
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
          impl_->instances.insert_or_assign(
              change.handle, std::move(entry));
        }
        break;
      case ObjectKind::Camera:
        if (erase) {
          impl_->cameras.erase(change.handle);
        } else {
          impl_->cameras.insert_or_assign(
              change.handle, world.Get(CameraHandle::FromValue(change.handle)));
        }
        break;
      case ObjectKind::Light:
        if (erase) {
          impl_->lights.erase(change.handle);
        } else {
          impl_->lights.insert_or_assign(
              change.handle,
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

  if (material_indices_changed) {
    for (const auto& [handle, entry] : impl_->materials) {
      (void)entry;
      delta.materials.upserts.push_back(handle);
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
