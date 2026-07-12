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
  std::uint64_t topology_revision{};
  std::shared_ptr<const std::vector<DrawVertex>> vertices;
  std::shared_ptr<const std::vector<std::uint32_t>> indices;
};

struct MaterialEntry {
  std::uint64_t revision{};
  MaterialDescriptor descriptor;
};

struct InstanceEntry {
  std::uint64_t revision{};
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
    const auto& descriptor = world.Get(MeshHandle::FromValue(change.handle));
    if (descriptor.positions.size() >
            static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
        descriptor.indices.size() >
            std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("mesh exceeds 32-bit draw limits");
    }
    if (change.change_kind == ChangeKind::Created ||
        change.HasAspect(ChangeAspect::Points)) {
      auto vertices = std::make_shared<std::vector<DrawVertex>>();
      vertices->reserve(descriptor.positions.size());
      for (const auto& position : descriptor.positions) {
        vertices->push_back({position});
      }
      entry.vertices = std::move(vertices);
      entry.points_revision = change.resource_revision;
    }
    if (change.change_kind == ChangeKind::Created ||
        change.HasAspect(ChangeAspect::Topology)) {
      entry.indices = std::make_shared<const std::vector<std::uint32_t>>(
          descriptor.indices);
      entry.topology_revision = change.resource_revision;
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
      next->geometries.push_back({handle, entry.points_revision,
                                  entry.topology_revision, entry.vertices,
                                  entry.indices});
    }

    std::map<std::uint64_t, std::uint32_t> material_indices;
    next->materials.reserve(materials.size());
    for (const auto& [handle, entry] : materials) {
      material_indices.emplace(
          handle, static_cast<std::uint32_t>(next->materials.size()));
      const auto& material = entry.descriptor;
      next->materials.push_back({handle, entry.revision, material.base_color,
                                 material.metallic, material.roughness,
                                 material.alpha_mode, material.double_sided});
    }

    next->instances.reserve(instances.size());
    for (const auto& [handle, entry] : instances) {
      const auto instance_index =
          static_cast<std::uint32_t>(next->instances.size());
      const auto& instance = entry.descriptor;
      next->instances.push_back({handle, entry.revision,
                                 instance.mesh.value(),
                                 instance.material.value(),
                                 instance.transform, instance.visible});
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
    snapshot = std::move(next);
  }

  std::map<std::uint64_t, MeshEntry> meshes;
  std::map<std::uint64_t, MaterialEntry> materials;
  std::map<std::uint64_t, InstanceEntry> instances;
  std::map<std::uint64_t, CameraDescriptor> cameras;
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
          impl_->materials.insert_or_assign(
              change.handle,
              MaterialEntry{change.resource_revision,
                            world.Get(MaterialHandle::FromValue(change.handle))});
        }
        break;
      case ObjectKind::Instance:
        if (erase) {
          impl_->instances.erase(change.handle);
        } else {
          impl_->instances.insert_or_assign(
              change.handle,
              InstanceEntry{change.resource_revision,
                            world.Get(InstanceHandle::FromValue(change.handle))});
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
        // Lights are intentionally ignored until the basic PBR path lands.
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
