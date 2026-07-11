#include <merlin/extraction/scene_extractor.hpp>

#include <merlin/core/render_world.hpp>

#include <algorithm>
#include <map>
#include <stdexcept>
#include <utility>

namespace merlin::extraction {
namespace {

template <typename Descriptor>
using RecordMap = std::map<std::uint64_t, Descriptor>;

std::uint64_t StableSortKey(const InstanceDescriptor& instance) {
  // Opaque-only MVP: material first, then mesh. The instance handle is used as
  // the final tie breaker when the draw packet is built.
  return (instance.material.value() * 0x9e3779b185ebca87ULL) ^
         instance.mesh.value();
}

}  // namespace

class SceneExtractor::Impl {
 public:
  void Rebuild() {
    output.vertices.clear();
    output.indices.clear();
    output.draws.clear();

    for (const auto& [instance_handle, instance] : instances) {
      if (!instance.visible) {
        continue;
      }
      const auto mesh_it = meshes.find(instance.mesh.value());
      const auto material_it = materials.find(instance.material.value());
      if (mesh_it == meshes.end() || material_it == materials.end()) {
        continue;
      }

      const auto& mesh = mesh_it->second;
      if (mesh.positions.empty() || mesh.indices.empty()) {
        continue;
      }
      if (output.vertices.size() > static_cast<std::size_t>(INT32_MAX) ||
          output.indices.size() > static_cast<std::size_t>(UINT32_MAX) ||
          mesh.indices.size() > static_cast<std::size_t>(UINT32_MAX)) {
        throw std::length_error("extracted scene exceeds 32-bit draw limits");
      }

      DrawCommand draw;
      draw.first_index = static_cast<std::uint32_t>(output.indices.size());
      draw.index_count = static_cast<std::uint32_t>(mesh.indices.size());
      draw.vertex_offset = static_cast<std::int32_t>(output.vertices.size());
      draw.transform = instance.transform;
      draw.base_color = material_it->second.base_color;
      draw.instance_handle = instance_handle;
      draw.sort_key = StableSortKey(instance);

      for (const auto& position : mesh.positions) {
        output.vertices.push_back({position});
      }
      output.indices.insert(output.indices.end(), mesh.indices.begin(),
                            mesh.indices.end());
      output.draws.push_back(draw);
    }

    std::stable_sort(output.draws.begin(), output.draws.end(),
                     [](const DrawCommand& lhs, const DrawCommand& rhs) {
                       if (lhs.sort_key != rhs.sort_key) {
                         return lhs.sort_key < rhs.sort_key;
                       }
                       return lhs.instance_handle < rhs.instance_handle;
                     });

    auto camera_it = cameras.end();
    if (active_camera.valid()) {
      camera_it = cameras.find(active_camera.value());
    }
    if (camera_it != cameras.end()) {
      const auto& camera = camera_it->second;
      output.view = camera.view;
      output.projection = camera.projection;
    } else {
      output.view = {};
      output.projection = {};
    }
  }

  RecordMap<MeshDescriptor> meshes;
  RecordMap<MaterialDescriptor> materials;
  RecordMap<InstanceDescriptor> instances;
  RecordMap<CameraDescriptor> cameras;
  CameraHandle active_camera;
  ExtractedScene output;
};

SceneExtractor::SceneExtractor() : impl_(std::make_unique<Impl>()) {}
SceneExtractor::~SceneExtractor() = default;
SceneExtractor::SceneExtractor(SceneExtractor&&) noexcept = default;
SceneExtractor& SceneExtractor::operator=(SceneExtractor&&) noexcept = default;

void SceneExtractor::Apply(const RenderWorld& world, const ChangeSet& changes) {
  if (changes.revision < impl_->output.revision) {
    throw std::invalid_argument("cannot apply an older RenderWorld revision");
  }
  if (changes.empty()) {
    if (changes.revision != impl_->output.revision) {
      throw std::invalid_argument("empty ChangeSet skips an extraction revision");
    }
    return;
  }
  if (changes.revision != impl_->output.revision + 1U) {
    throw std::invalid_argument("ChangeSet revisions must be applied in order");
  }

  for (const auto& change : changes.changes) {
    const auto erase = change.change_kind == ChangeKind::Removed;
    switch (change.object_kind) {
      case ObjectKind::Mesh:
        if (erase) {
          impl_->meshes.erase(change.handle);
        } else {
          impl_->meshes.insert_or_assign(
              change.handle, world.Get(MeshHandle::FromValue(change.handle)));
        }
        break;
      case ObjectKind::Material:
        if (erase) {
          impl_->materials.erase(change.handle);
        } else {
          impl_->materials.insert_or_assign(
              change.handle, world.Get(MaterialHandle::FromValue(change.handle)));
        }
        break;
      case ObjectKind::Instance:
        if (erase) {
          impl_->instances.erase(change.handle);
        } else {
          impl_->instances.insert_or_assign(
              change.handle, world.Get(InstanceHandle::FromValue(change.handle)));
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
        // flattened draw extraction until render requests are introduced.
        break;
    }
  }

  impl_->output.revision = changes.revision;
  impl_->Rebuild();
}

void SceneExtractor::SetActiveCamera(CameraHandle camera) {
  if (impl_->active_camera == camera) {
    return;
  }
  impl_->active_camera = camera;
  impl_->Rebuild();
}

const ExtractedScene& SceneExtractor::scene() const noexcept {
  return impl_->output;
}

}  // namespace merlin::extraction
