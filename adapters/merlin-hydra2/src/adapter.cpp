#include "adapter.hpp"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/meshTopology.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/imaging/hd/tokens.h>

#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/vulkan/renderer.hpp>

#ifdef _WIN32
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

merlin::Mat4 ToMerlinMatrix(const GfMatrix4d& matrix) {
  merlin::Mat4 result;
  // GfMatrix uses row vectors while Merlin shaders use column vectors. Store
  // the semantic transpose in Merlin's column-major Mat4 representation.
  for (std::size_t row = 0; row < 4; ++row) {
    for (std::size_t column = 0; column < 4; ++column) {
      result.values[column * 4 + row] = static_cast<float>(
          matrix[static_cast<int>(column)][static_cast<int>(row)]);
    }
  }
  return result;
}

bool MatricesEqual(const merlin::Mat4& lhs, const merlin::Mat4& rhs) {
  return lhs.values == rhs.values;
}

std::filesystem::path PluginDirectory() {
#ifdef _WIN32
  static int module_anchor;
  HMODULE module{};
  const auto address = reinterpret_cast<LPCWSTR>(&module_anchor);
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          address, &module)) {
    throw std::runtime_error("could not locate hdMerlin module");
  }
  std::wstring path(32768, L'\0');
  const DWORD length =
      GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) {
    throw std::runtime_error("could not resolve hdMerlin module path");
  }
  path.resize(length);
  return std::filesystem::path(path).parent_path();
#else
  static int module_anchor;
  Dl_info info{};
  if (dladdr(&module_anchor, &info) == 0 || info.dli_fname == nullptr) {
    throw std::runtime_error("could not locate hdMerlin module");
  }
  return std::filesystem::path(info.dli_fname).parent_path();
#endif
}

std::optional<std::filesystem::path> FirstFrameMarkerPath() {
#ifdef _WIN32
  char* value{};
  std::size_t size{};
  if (_dupenv_s(&value, &size, "MERLIN_HYDRA2_FIRST_FRAME_MARKER") != 0 ||
      value == nullptr) {
    return std::nullopt;
  }
  const std::filesystem::path result(value);
  std::free(value);
  return result;
#else
  if (const char* value =
          std::getenv("MERLIN_HYDRA2_FIRST_FRAME_MARKER")) {
    return std::filesystem::path(value);
  }
  return std::nullopt;
#endif
}

struct MeshState {
  merlin::MeshHandle mesh;
  merlin::InstanceHandle instance;
  merlin::MeshDescriptor mesh_descriptor;
  merlin::InstanceDescriptor instance_descriptor;
  bool authored_visible{true};
};

bool IsInCollection(const SdfPath& path,
                    const HdRprimCollection& collection) {
  const auto& roots = collection.GetRootPaths();
  const bool included = std::any_of(
      roots.begin(), roots.end(), [&path](const SdfPath& root) {
        return path.HasPrefix(root);
      });
  if (!included) {
    return false;
  }
  const auto& excludes = collection.GetExcludePaths();
  return std::none_of(excludes.begin(), excludes.end(),
                      [&path](const SdfPath& exclude) {
                        return path.HasPrefix(exclude);
                      });
}

bool HasRequestedRenderTag(const SdfPath& path,
                           const TfTokenVector& render_tags,
                           HdRenderIndex* render_index) {
  if (render_tags.empty()) {
    return true;
  }
  const HdRprim* rprim = render_index->GetRprim(path);
  return rprim != nullptr &&
         std::find(render_tags.begin(), render_tags.end(),
                   rprim->GetRenderTag()) != render_tags.end();
}

class SceneBridge {
 public:
  SceneBridge() {
    merlin::MaterialDescriptor material;
    material.label = "Hydra fallback material";
    material.base_color = {0.18F, 0.78F, 1.0F, 1.0F};
    fallback_material_ = world_.CreateMaterial(std::move(material));
  }

  void SyncMesh(const SdfPath& path, merlin::MeshDescriptor mesh,
                const merlin::Mat4& transform, bool visible) {
    std::scoped_lock lock(mutex_);
    const auto key = path.GetString();
    auto found = meshes_.find(key);
    if (mesh.positions.empty() || mesh.indices.empty()) {
      if (found != meshes_.end()) {
        world_.Remove(found->second.instance);
        world_.Remove(found->second.mesh);
        meshes_.erase(found);
      }
      return;
    }
    if (found == meshes_.end()) {
      MeshState state;
      state.mesh_descriptor = std::move(mesh);
      state.mesh = world_.CreateMesh(state.mesh_descriptor);
      state.instance_descriptor.label = key;
      state.instance_descriptor.mesh = state.mesh;
      state.instance_descriptor.material = fallback_material_;
      state.instance_descriptor.transform = transform;
      state.instance_descriptor.visible = visible;
      state.authored_visible = visible;
      state.instance = world_.CreateInstance(state.instance_descriptor);
      meshes_.emplace(key, std::move(state));
      return;
    }

    auto& state = found->second;
    state.mesh_descriptor = std::move(mesh);
    world_.UpdateMesh(state.mesh, state.mesh_descriptor);
    state.instance_descriptor.transform = transform;
    state.instance_descriptor.visible = visible;
    state.authored_visible = visible;
    world_.UpdateInstance(state.instance, state.instance_descriptor);
  }

  void RemoveMesh(const SdfPath& path) {
    std::scoped_lock lock(mutex_);
    const auto found = meshes_.find(path.GetString());
    if (found == meshes_.end()) {
      return;
    }
    world_.Remove(found->second.instance);
    world_.Remove(found->second.mesh);
    meshes_.erase(found);
  }

  void SyncCamera(const SdfPath& path, const merlin::Mat4& view,
                  const merlin::Mat4& projection) {
    std::scoped_lock lock(mutex_);
    SyncCameraLocked(path.GetString(), view, projection);
  }

  void RemoveCamera(const SdfPath& path) {
    std::scoped_lock lock(mutex_);
    const auto found = cameras_.find(path.GetString());
    if (found == cameras_.end()) {
      return;
    }
    world_.Remove(found->second);
    cameras_.erase(found);
  }

  void Render(const HdRenderPassState& state,
              const HdRenderPassAovBindingVector& bindings,
              const HdRprimCollection& collection,
              const TfTokenVector& render_tags,
              HdRenderIndex* render_index) {
    std::scoped_lock lock(mutex_);

    for (auto& [path_string, mesh] : meshes_) {
      const SdfPath path(path_string);
      const bool selected =
          IsInCollection(path, collection) &&
          HasRequestedRenderTag(path, render_tags, render_index);
      const bool visible = mesh.authored_visible && selected;
      if (mesh.instance_descriptor.visible != visible) {
        mesh.instance_descriptor.visible = visible;
        world_.UpdateInstance(mesh.instance, mesh.instance_descriptor);
      }
    }

    merlin::CameraHandle active_camera;
    if (const HdCamera* camera = state.GetCamera()) {
      const auto key = camera->GetId().GetString();
      const auto view = ToMerlinMatrix(state.GetWorldToViewMatrix());
      const auto projection = ToMerlinMatrix(state.GetProjectionMatrix());
      active_camera = SyncCameraLocked(key, view, projection);
    }
    extractor_.SetActiveCamera(active_camera);

    const auto changes = world_.Commit();
    if (!changes.empty()) {
      extractor_.Apply(world_, changes);
    }

    std::uint32_t width{};
    std::uint32_t height{};
    for (const auto& binding : bindings) {
      HdRenderBuffer* buffer = binding.renderBuffer;
      if (buffer == nullptr && !binding.renderBufferId.IsEmpty()) {
        buffer = dynamic_cast<HdRenderBuffer*>(render_index->GetBprim(
            HdPrimTypeTokens->renderBuffer, binding.renderBufferId));
      }
      if (buffer != nullptr && buffer->GetWidth() != 0 &&
          buffer->GetHeight() != 0) {
        width = buffer->GetWidth();
        height = buffer->GetHeight();
        break;
      }
    }
    if (width == 0 || height == 0) {
      return;
    }

    if (!renderer_) {
      renderer_ = std::make_unique<merlin::vulkan::Renderer>();
    }
    const auto shader_dir = PluginDirectory() / "shaders";
    const merlin::vulkan::ShaderPaths shaders{
        shader_dir / "triangle.vert.spv", shader_dir / "triangle.frag.spv"};
    const auto result =
        renderer_->Render(extractor_.scene(), width, height, shaders);

    std::size_t buffers_written{};
    for (const auto& binding : bindings) {
      HdRenderBuffer* base = binding.renderBuffer;
      if (base == nullptr && !binding.renderBufferId.IsEmpty()) {
        base = dynamic_cast<HdRenderBuffer*>(render_index->GetBprim(
            HdPrimTypeTokens->renderBuffer, binding.renderBufferId));
      }
      auto* buffer = dynamic_cast<HdMerlinRenderBuffer*>(base);
      if (buffer == nullptr) {
        continue;
      }
      buffer->SetConverged(false);
      bool written{};
      if (binding.aovName == HdAovTokens->color) {
        written = buffer->WriteColor(result.color.pixels, result.color.width,
                                     result.color.height);
      } else if (HdAovHasDepthSemantic(binding.aovName)) {
        written = buffer->WriteDepth(result.depth.pixels, result.depth.width,
                                     result.depth.height);
      }
      buffer->SetConverged(written);
      buffers_written += written ? 1U : 0U;
    }

    if (buffers_written != 0) {
      if (const auto marker_path = FirstFrameMarkerPath()) {
        const auto covered_pixels = static_cast<std::size_t>(std::count_if(
            result.depth.pixels.begin(), result.depth.pixels.end(),
            [](float depth) { return depth < 1.0F; }));
        if (marker_path->has_parent_path()) {
          std::filesystem::create_directories(marker_path->parent_path());
        }
        std::ofstream stream(*marker_path, std::ios::trunc);
        stream << "scene_revision=" << result.scene_revision << '\n'
               << "completion_value=" << result.completion_value << '\n'
               << "draw_count=" << extractor_.scene().draws.size() << '\n'
               << "buffers_written=" << buffers_written << '\n'
               << "covered_pixels=" << covered_pixels << '\n';
      }
    }
  }

 private:
  merlin::CameraHandle SyncCameraLocked(const std::string& key,
                                        const merlin::Mat4& view,
                                        const merlin::Mat4& projection) {
    const auto found = cameras_.find(key);
    if (found == cameras_.end()) {
      merlin::CameraDescriptor descriptor;
      descriptor.label = key;
      descriptor.view = view;
      descriptor.projection = projection;
      const auto handle = world_.CreateCamera(std::move(descriptor));
      cameras_.emplace(key, handle);
      return handle;
    }
    const auto handle = found->second;
    const auto& old = world_.Get(handle);
    if (!MatricesEqual(old.view, view) ||
        !MatricesEqual(old.projection, projection)) {
      auto descriptor = old;
      descriptor.view = view;
      descriptor.projection = projection;
      world_.UpdateCamera(handle, std::move(descriptor));
    }
    return handle;
  }

  std::mutex mutex_;
  merlin::RenderWorld world_;
  merlin::extraction::SceneExtractor extractor_;
  std::unique_ptr<merlin::vulkan::Renderer> renderer_;
  merlin::MaterialHandle fallback_material_;
  std::unordered_map<std::string, MeshState> meshes_;
  std::unordered_map<std::string, merlin::CameraHandle> cameras_;
};

class HdMerlinMesh final : public HdMesh {
 public:
  HdMerlinMesh(const SdfPath& id, std::shared_ptr<SceneBridge> bridge)
      : HdMesh(id), bridge_(std::move(bridge)) {
    descriptor_.label = id.GetString();
  }

  ~HdMerlinMesh() override { bridge_->RemoveMesh(GetId()); }

  HdDirtyBits GetInitialDirtyBitsMask() const override {
    return HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyTopology |
           HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyVisibility |
           HdChangeTracker::DirtyRenderTag;
  }

  void Sync(HdSceneDelegate* delegate, HdRenderParam* render_param,
            HdDirtyBits* dirty_bits, const TfToken& repr_token) override {
    (void)render_param;
    (void)repr_token;
    const bool points_dirty =
        (*dirty_bits & HdChangeTracker::DirtyPoints) != 0;
    const bool topology_dirty =
        (*dirty_bits & HdChangeTracker::DirtyTopology) != 0;
    if (points_dirty) {
      const VtValue points = GetPoints(delegate);
      descriptor_.positions.clear();
      if (points.IsHolding<VtVec3fArray>()) {
        for (const auto& point : points.UncheckedGet<VtVec3fArray>()) {
          descriptor_.positions.push_back({point[0], point[1], point[2]});
        }
      } else if (points.IsHolding<VtVec3dArray>()) {
        for (const auto& point : points.UncheckedGet<VtVec3dArray>()) {
          descriptor_.positions.push_back(
              {static_cast<float>(point[0]), static_cast<float>(point[1]),
               static_cast<float>(point[2])});
        }
      } else {
        TF_WARN("Merlin mesh %s has unsupported points type",
                GetId().GetText());
      }
    }
    if (topology_dirty) {
      topology_ = GetMeshTopology(delegate);
    }
    if (points_dirty || topology_dirty) {
      RebuildIndices();
    }
    if ((*dirty_bits & HdChangeTracker::DirtyTransform) != 0) {
      transform_ = ToMerlinMatrix(delegate->GetTransform(GetId()));
    }
    if ((*dirty_bits & HdChangeTracker::DirtyVisibility) != 0) {
      visible_ = delegate->GetVisible(GetId());
    }
    bridge_->SyncMesh(GetId(), descriptor_, transform_, visible_);
    *dirty_bits = HdChangeTracker::Clean;
  }

 protected:
  HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override {
    return bits;
  }

  void _InitRepr(const TfToken& repr_token, HdDirtyBits* dirty_bits) override {
    (void)repr_token;
    *dirty_bits |= GetInitialDirtyBitsMask();
  }

 private:
  void RebuildIndices() {
    const auto& counts = topology_.GetFaceVertexCounts();
    const auto& authored_indices = topology_.GetFaceVertexIndices();
    const auto& authored_holes = topology_.GetHoleIndices();
    const std::unordered_set<int> holes(authored_holes.begin(),
                                        authored_holes.end());
    descriptor_.indices.clear();
    std::size_t offset{};
    int face_index{};
    bool valid = true;
    for (const int count : counts) {
      if (count < 0 || static_cast<std::size_t>(count) >
                           authored_indices.size() - offset) {
        valid = false;
        break;
      }
      if (!holes.contains(face_index)) {
        for (int corner = 1; corner + 1 < count; ++corner) {
          const int triangle[3] = {authored_indices[offset],
                                   authored_indices[offset + corner],
                                   authored_indices[offset + corner + 1]};
          for (const int index : triangle) {
            if (index < 0 || static_cast<std::size_t>(index) >=
                                 descriptor_.positions.size()) {
              valid = false;
              break;
            }
            descriptor_.indices.push_back(static_cast<std::uint32_t>(index));
          }
          if (!valid) {
            break;
          }
        }
      }
      if (!valid) {
        break;
      }
      offset += static_cast<std::size_t>(count);
      ++face_index;
    }
    if (!valid || offset != authored_indices.size()) {
      descriptor_.indices.clear();
      TF_WARN("Merlin mesh %s has invalid topology", GetId().GetText());
    }
  }

  std::shared_ptr<SceneBridge> bridge_;
  merlin::MeshDescriptor descriptor_;
  HdMeshTopology topology_;
  merlin::Mat4 transform_;
  bool visible_{true};
};

class HdMerlinCamera final : public HdCamera {
 public:
  HdMerlinCamera(const SdfPath& id, std::shared_ptr<SceneBridge> bridge)
      : HdCamera(id), bridge_(std::move(bridge)) {}

  ~HdMerlinCamera() override { bridge_->RemoveCamera(GetId()); }

  void Sync(HdSceneDelegate* delegate, HdRenderParam* render_param,
            HdDirtyBits* dirty_bits) override {
    HdCamera::Sync(delegate, render_param, dirty_bits);
    bridge_->SyncCamera(GetId(), ToMerlinMatrix(GetTransform().GetInverse()),
                        ToMerlinMatrix(ComputeProjectionMatrix()));
  }

 private:
  std::shared_ptr<SceneBridge> bridge_;
};

class HdMerlinRenderPass final : public HdRenderPass {
 public:
  HdMerlinRenderPass(HdRenderIndex* index,
                     const HdRprimCollection& collection,
                     std::shared_ptr<SceneBridge> bridge)
      : HdRenderPass(index, collection), bridge_(std::move(bridge)) {}

 private:
  void _Execute(const HdRenderPassStateSharedPtr& render_pass_state,
                const TfTokenVector& render_tags) override {
    bridge_->Render(*render_pass_state, render_pass_state->GetAovBindings(),
                    GetRprimCollection(), render_tags, GetRenderIndex());
  }

  std::shared_ptr<SceneBridge> bridge_;
};

}  // namespace

HdMerlinRenderBuffer::HdMerlinRenderBuffer(const SdfPath& id)
    : HdRenderBuffer(id) {}

bool HdMerlinRenderBuffer::Allocate(const GfVec3i& dimensions, HdFormat format,
                                    bool multi_sampled) {
  std::scoped_lock lock(mutex_);
  if (map_count_ != 0 || dimensions[0] < 0 || dimensions[1] < 0 ||
      dimensions[2] < 0 || multi_sampled ||
      (format != HdFormatUNorm8Vec4 && format != HdFormatFloat32)) {
    return false;
  }
  const auto pixel_size = HdDataSizeOfFormat(format);
  const auto width = static_cast<std::size_t>(dimensions[0]);
  const auto height = static_cast<std::size_t>(dimensions[1]);
  const auto depth = static_cast<std::size_t>(dimensions[2]);
  if (width != 0 && height > std::numeric_limits<std::size_t>::max() / width) {
    return false;
  }
  const auto plane_pixels = width * height;
  if (depth != 0 &&
      plane_pixels > std::numeric_limits<std::size_t>::max() / depth) {
    return false;
  }
  const auto pixels = plane_pixels * depth;
  if (pixel_size != 0 &&
      pixels > std::numeric_limits<std::size_t>::max() / pixel_size) {
    return false;
  }
  dimensions_ = dimensions;
  format_ = format;
  multi_sampled_ = multi_sampled;
  converged_ = false;
  data_.assign(pixels * pixel_size, 0);
  return true;
}

unsigned int HdMerlinRenderBuffer::GetWidth() const {
  std::scoped_lock lock(mutex_);
  return static_cast<unsigned int>(dimensions_[0]);
}

unsigned int HdMerlinRenderBuffer::GetHeight() const {
  std::scoped_lock lock(mutex_);
  return static_cast<unsigned int>(dimensions_[1]);
}

unsigned int HdMerlinRenderBuffer::GetDepth() const {
  std::scoped_lock lock(mutex_);
  return static_cast<unsigned int>(dimensions_[2]);
}

HdFormat HdMerlinRenderBuffer::GetFormat() const {
  std::scoped_lock lock(mutex_);
  return format_;
}

bool HdMerlinRenderBuffer::IsMultiSampled() const {
  std::scoped_lock lock(mutex_);
  return multi_sampled_;
}

void* HdMerlinRenderBuffer::Map() {
  std::scoped_lock lock(mutex_);
  if (data_.empty()) {
    return nullptr;
  }
  ++map_count_;
  return data_.data();
}

void HdMerlinRenderBuffer::Unmap() {
  std::scoped_lock lock(mutex_);
  if (map_count_ != 0) {
    --map_count_;
  }
}

bool HdMerlinRenderBuffer::IsMapped() const {
  std::scoped_lock lock(mutex_);
  return map_count_ != 0;
}

void HdMerlinRenderBuffer::Resolve() {}

bool HdMerlinRenderBuffer::IsConverged() const {
  std::scoped_lock lock(mutex_);
  return converged_;
}

bool HdMerlinRenderBuffer::WriteColor(
    const std::vector<std::uint8_t>& rgba8, std::uint32_t width,
    std::uint32_t height) {
  std::scoped_lock lock(mutex_);
  if (map_count_ != 0 || format_ != HdFormatUNorm8Vec4 ||
      dimensions_ != GfVec3i(static_cast<int>(width), static_cast<int>(height),
                             1) ||
      rgba8.size() != data_.size()) {
    return false;
  }
  data_ = rgba8;
  return true;
}

bool HdMerlinRenderBuffer::WriteDepth(const std::vector<float>& depth,
                                      std::uint32_t width,
                                      std::uint32_t height) {
  std::scoped_lock lock(mutex_);
  const auto byte_size = depth.size() * sizeof(float);
  if (map_count_ != 0 || format_ != HdFormatFloat32 ||
      dimensions_ != GfVec3i(static_cast<int>(width), static_cast<int>(height),
                             1) ||
      byte_size != data_.size()) {
    return false;
  }
  std::memcpy(data_.data(), depth.data(), byte_size);
  return true;
}

void HdMerlinRenderBuffer::SetConverged(bool converged) {
  std::scoped_lock lock(mutex_);
  converged_ = converged;
}

void HdMerlinRenderBuffer::_Deallocate() {
  std::scoped_lock lock(mutex_);
  if (map_count_ != 0) {
    return;
  }
  dimensions_ = GfVec3i(0);
  format_ = HdFormatInvalid;
  multi_sampled_ = false;
  converged_ = false;
  data_.clear();
}

class HdMerlinRenderDelegate::Impl {
 public:
  std::shared_ptr<SceneBridge> bridge = std::make_shared<SceneBridge>();
};

HdMerlinRenderDelegate::HdMerlinRenderDelegate(
    const HdRenderSettingsMap& settings)
    : HdRenderDelegate(settings),
      impl_(std::make_unique<Impl>()),
      resources_(std::make_shared<HdResourceRegistry>()) {}

HdMerlinRenderDelegate::~HdMerlinRenderDelegate() = default;

const TfTokenVector& HdMerlinRenderDelegate::GetSupportedRprimTypes() const {
  static const TfTokenVector types{HdPrimTypeTokens->mesh};
  return types;
}

const TfTokenVector& HdMerlinRenderDelegate::GetSupportedSprimTypes() const {
  static const TfTokenVector types{HdPrimTypeTokens->camera};
  return types;
}

const TfTokenVector& HdMerlinRenderDelegate::GetSupportedBprimTypes() const {
  static const TfTokenVector types{HdPrimTypeTokens->renderBuffer};
  return types;
}

HdResourceRegistrySharedPtr HdMerlinRenderDelegate::GetResourceRegistry() const {
  return resources_;
}

HdRenderPassSharedPtr HdMerlinRenderDelegate::CreateRenderPass(
    HdRenderIndex* index, const HdRprimCollection& collection) {
  return std::make_shared<HdMerlinRenderPass>(index, collection,
                                              impl_->bridge);
}

HdInstancer* HdMerlinRenderDelegate::CreateInstancer(HdSceneDelegate* delegate,
                                                     const SdfPath& id) {
  (void)delegate;
  (void)id;
  return nullptr;
}

void HdMerlinRenderDelegate::DestroyInstancer(HdInstancer* instancer) {
  delete instancer;
}

HdRprim* HdMerlinRenderDelegate::CreateRprim(const TfToken& type_id,
                                             const SdfPath& rprim_id) {
  if (type_id == HdPrimTypeTokens->mesh) {
    return new HdMerlinMesh(rprim_id, impl_->bridge);
  }
  return nullptr;
}

void HdMerlinRenderDelegate::DestroyRprim(HdRprim* rprim) { delete rprim; }

HdSprim* HdMerlinRenderDelegate::CreateSprim(const TfToken& type_id,
                                             const SdfPath& sprim_id) {
  if (type_id == HdPrimTypeTokens->camera) {
    return new HdMerlinCamera(sprim_id, impl_->bridge);
  }
  return nullptr;
}

HdSprim* HdMerlinRenderDelegate::CreateFallbackSprim(const TfToken& type_id) {
  if (type_id == HdPrimTypeTokens->camera) {
    return new HdMerlinCamera(SdfPath("/__merlinFallbackCamera"),
                              impl_->bridge);
  }
  return nullptr;
}

void HdMerlinRenderDelegate::DestroySprim(HdSprim* sprim) { delete sprim; }

HdBprim* HdMerlinRenderDelegate::CreateBprim(const TfToken& type_id,
                                             const SdfPath& bprim_id) {
  if (type_id == HdPrimTypeTokens->renderBuffer) {
    return new HdMerlinRenderBuffer(bprim_id);
  }
  return nullptr;
}

HdBprim* HdMerlinRenderDelegate::CreateFallbackBprim(const TfToken& type_id) {
  if (type_id == HdPrimTypeTokens->renderBuffer) {
    return new HdMerlinRenderBuffer(SdfPath("/__merlinFallbackRenderBuffer"));
  }
  return nullptr;
}

void HdMerlinRenderDelegate::DestroyBprim(HdBprim* bprim) { delete bprim; }

void HdMerlinRenderDelegate::CommitResources(HdChangeTracker* tracker) {
  (void)tracker;
}

HdAovDescriptor HdMerlinRenderDelegate::GetDefaultAovDescriptor(
    const TfToken& name) const {
  if (name == HdAovTokens->color) {
    return {HdFormatUNorm8Vec4, false, VtValue(GfVec4f(0.0F))};
  }
  if (name == HdAovTokens->depth) {
    return {HdFormatFloat32, false, VtValue(1.0F)};
  }
  return {};
}

PXR_NAMESPACE_CLOSE_SCOPE
