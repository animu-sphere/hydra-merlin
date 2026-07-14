#include <merlin/extraction/scene_extractor.hpp>

#include <merlin/core/render_world.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace merlin::extraction {
namespace {

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

  void RebuildSnapshot() {
    auto next = std::make_shared<FrameSnapshot>();
    next->revision = revision;

    std::map<std::uint64_t, std::uint32_t> geometry_indices;
    next->geometries.reserve(meshes.size());
    for (const auto& [handle, entry] : meshes) {
      geometry_indices.emplace(
          handle, static_cast<std::uint32_t>(next->geometries.size()));
      next->geometries.push_back(
          {handle,
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
           entry.has_texcoords});
    }

    std::map<std::uint64_t, std::uint32_t> texture_indices;
    next->textures.reserve(textures.size());
    for (const auto& [handle, entry] : textures) {
      texture_indices.emplace(
          handle, static_cast<std::uint32_t>(next->textures.size()));
      next->textures.push_back({handle, entry.revision, entry.width,
                                entry.height, entry.format, entry.pixels});
    }

    std::map<std::uint64_t, std::uint32_t> sampler_indices;
    next->samplers.reserve(samplers.size());
    for (const auto& [handle, entry] : samplers) {
      sampler_indices.emplace(
          handle, static_cast<std::uint32_t>(next->samplers.size()));
      const auto& sampler = entry.descriptor;
      next->samplers.push_back({handle, entry.revision, sampler.min_filter,
                                sampler.mag_filter, sampler.address_u,
                                sampler.address_v});
    }

    std::map<std::uint64_t, std::uint32_t> material_indices;
    next->materials.reserve(materials.size());
    for (const auto& [handle, entry] : materials) {
      material_indices.emplace(
          handle, static_cast<std::uint32_t>(next->materials.size()));
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
        next->material_fallbacks.push_back(
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
          next->material_fallbacks.push_back(
              {handle, MaterialFallbackCode::MissingTexture,
               "base-color texture is unavailable; using constant color"});
        } else if (sampler == sampler_indices.end()) {
          record.features = static_cast<MaterialFeature>(
              static_cast<std::uint32_t>(record.features) &
              ~static_cast<std::uint32_t>(MaterialFeature::BaseColorTexture));
          next->material_fallbacks.push_back(
              {handle, MaterialFallbackCode::MissingSampler,
               "base-color sampler is unavailable; using constant color"});
        } else {
          record.base_color_texture = TextureBindingRecord{
              texture->second, sampler->second,
              material.base_color_texture->texcoord_set};
        }
      }
      next->materials.push_back(std::move(record));
    }

    next->instances.reserve(instances.size());
    for (const auto& [handle, entry] : instances) {
      const auto instance_index =
          static_cast<std::uint32_t>(next->instances.size());
      const auto& instance = entry.descriptor;
      next->instances.push_back(
          {handle,
           entry.revision,
           entry.transform_revision,
           entry.visibility_revision,
           entry.material_binding_revision,
           instance.mesh.value(),
           instance.material.value(),
           instance.transform,
           instance.visible});
      if (!instance.visible) {
        continue;
      }
      const auto geometry = geometry_indices.find(instance.mesh.value());
      const auto material = material_indices.find(instance.material.value());
      if (geometry == geometry_indices.end() ||
          material == material_indices.end()) {
        continue;
      }
      next->draws.push_back(
          {geometry->second, material->second, instance_index,
           StableSortKey(instance.material.value(), instance.mesh.value())});
    }

    std::stable_sort(next->draws.begin(), next->draws.end(),
                     [&](const DrawRecord& lhs, const DrawRecord& rhs) {
                       if (lhs.sort_key != rhs.sort_key) {
                         return lhs.sort_key < rhs.sort_key;
                       }
                       return next->instances[lhs.instance_index].instance <
                              next->instances[rhs.instance_index].instance;
                     });

    if (active_camera.valid()) {
      const auto camera = cameras.find(active_camera.value());
      if (camera != cameras.end()) {
        next->view = camera->second.view;
        next->projection = camera->second.projection;
      }
    }
    next->lights.reserve(lights.size());
    for (const auto& [handle, entry] : lights) {
      const auto& light = entry.descriptor;
      next->lights.push_back({handle, entry.revision, light.type, light.color,
                              light.intensity, light.transform});
    }
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

  for (auto& [handle, entry] : impl_->meshes) {
    (void)handle;
    entry.vertex_base_revision = entry.vertex_revision;
    entry.index_base_revision = entry.index_revision;
    entry.vertex_ranges.clear();
    entry.index_ranges.clear();
  }

  for (const auto& change : changes.changes) {
    const auto erase = change.change_kind == ChangeKind::Removed;
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

  impl_->revision = changes.revision;
  impl_->RebuildSnapshot();
}

void SceneExtractor::SetActiveCamera(CameraHandle camera) {
  if (impl_->active_camera == camera) {
    return;
  }
  impl_->active_camera = camera;
  impl_->RebuildSnapshot();
}

std::shared_ptr<const FrameSnapshot> SceneExtractor::snapshot() const noexcept {
  return impl_->snapshot;
}

}  // namespace merlin::extraction
