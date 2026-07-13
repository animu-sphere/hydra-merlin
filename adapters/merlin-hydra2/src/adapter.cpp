#include "adapter.hpp"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/quaternion.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4d.h>
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
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

struct MeshCorner {
  std::uint32_t point{};
  std::uint32_t face{};
  std::uint32_t authored_corner{};
};

struct PrimvarInput {
  VtValue value;
  VtIntArray indices;
  HdInterpolation interpolation{HdInterpolationConstant};
  bool present{};
};

float Cross2(const GfVec2d& a, const GfVec2d& b, const GfVec2d& c) {
  return static_cast<float>((b[0] - a[0]) * (c[1] - a[1]) -
                            (b[1] - a[1]) * (c[0] - a[0]));
}

bool PointInTriangle(const GfVec2d& p, const GfVec2d& a, const GfVec2d& b,
                     const GfVec2d& c, float winding) {
  constexpr float epsilon = 1.0e-7F;
  return Cross2(a, b, p) * winding >= -epsilon &&
         Cross2(b, c, p) * winding >= -epsilon &&
         Cross2(c, a, p) * winding >= -epsilon;
}

std::vector<std::array<std::uint32_t, 3>> TriangulateFace(
    std::span<const std::uint32_t> points, const std::vector<GfVec3d>& positions,
    std::string& diagnostic) {
  if (points.size() < 3) {
    diagnostic = "face has fewer than three vertices";
    return {};
  }
  GfVec3d normal(0.0);
  for (std::size_t i = 0; i < points.size(); ++i) {
    const auto& a = positions[points[i]];
    const auto& b = positions[points[(i + 1U) % points.size()]];
    normal[0] += (a[1] - b[1]) * (a[2] + b[2]);
    normal[1] += (a[2] - b[2]) * (a[0] + b[0]);
    normal[2] += (a[0] - b[0]) * (a[1] + b[1]);
  }
  const GfVec3d magnitude(std::abs(normal[0]), std::abs(normal[1]),
                          std::abs(normal[2]));
  int drop_axis = 2;
  if (magnitude[0] >= magnitude[1] && magnitude[0] >= magnitude[2]) {
    drop_axis = 0;
  } else if (magnitude[1] >= magnitude[2]) {
    drop_axis = 1;
  }
  std::vector<GfVec2d> projected;
  projected.reserve(points.size());
  for (const auto point : points) {
    const auto& value = positions[point];
    if (drop_axis == 0) {
      projected.emplace_back(value[1], value[2]);
    } else if (drop_axis == 1) {
      projected.emplace_back(value[0], value[2]);
    } else {
      projected.emplace_back(value[0], value[1]);
    }
  }
  double area{};
  for (std::size_t i = 0; i < projected.size(); ++i) {
    const auto& a = projected[i];
    const auto& b = projected[(i + 1U) % projected.size()];
    area += a[0] * b[1] - b[0] * a[1];
  }
  if (std::abs(area) <= 1.0e-12) {
    diagnostic = "face is degenerate";
    return {};
  }
  const float winding = area > 0.0 ? 1.0F : -1.0F;
  std::vector<std::uint32_t> remaining(points.size());
  for (std::uint32_t i = 0; i < remaining.size(); ++i) {
    remaining[i] = i;
  }
  std::vector<std::array<std::uint32_t, 3>> triangles;
  triangles.reserve(points.size() - 2U);
  while (remaining.size() > 3U) {
    bool clipped{};
    for (std::size_t i = 0; i < remaining.size(); ++i) {
      const auto previous = remaining[(i + remaining.size() - 1U) %
                                      remaining.size()];
      const auto current = remaining[i];
      const auto next = remaining[(i + 1U) % remaining.size()];
      if (Cross2(projected[previous], projected[current], projected[next]) *
              winding <= 1.0e-7F) {
        continue;
      }
      bool contains_point{};
      for (const auto candidate : remaining) {
        if (candidate == previous || candidate == current ||
            candidate == next) {
          continue;
        }
        if (PointInTriangle(projected[candidate], projected[previous],
                            projected[current], projected[next], winding)) {
          contains_point = true;
          break;
        }
      }
      if (contains_point) {
        continue;
      }
      triangles.push_back({previous, current, next});
      remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(i));
      clipped = true;
      break;
    }
    if (!clipped) {
      diagnostic = "face is self-intersecting or numerically degenerate";
      return {};
    }
  }
  triangles.push_back({remaining[0], remaining[1], remaining[2]});
  return triangles;
}

std::optional<std::size_t> PrimvarElement(const PrimvarInput& input,
                                          const MeshCorner& corner) {
  std::size_t element{};
  switch (input.interpolation) {
    case HdInterpolationConstant:
      element = 0;
      break;
    case HdInterpolationUniform:
      element = corner.face;
      break;
    case HdInterpolationVertex:
    case HdInterpolationVarying:
      element = corner.point;
      break;
    case HdInterpolationFaceVarying:
      element = corner.authored_corner;
      break;
    case HdInterpolationInstance:
    case HdInterpolationCount:
      return std::nullopt;
  }
  if (!input.indices.empty()) {
    if (element >= input.indices.size() || input.indices[element] < 0) {
      return std::nullopt;
    }
    element = static_cast<std::size_t>(input.indices[element]);
  }
  return element;
}

PrimvarInput FindPrimvar(const HdRprim& rprim, HdSceneDelegate* delegate,
                         const TfToken& name,
                         bool texture_coordinate_fallback = false) {
  constexpr std::array<HdInterpolation, 5> interpolations{
      HdInterpolationConstant, HdInterpolationUniform, HdInterpolationVertex,
      HdInterpolationVarying, HdInterpolationFaceVarying};
  for (const auto interpolation : interpolations) {
    const auto descriptors =
        rprim.GetPrimvarDescriptors(delegate, interpolation);
    for (const auto& descriptor : descriptors) {
      if (descriptor.name != name &&
          !(texture_coordinate_fallback &&
            descriptor.role == HdPrimvarRoleTokens->textureCoordinate)) {
        continue;
      }
      PrimvarInput result;
      result.interpolation = interpolation;
      result.present = true;
      result.value = descriptor.indexed
                         ? rprim.GetIndexedPrimvar(delegate, descriptor.name,
                                                  &result.indices)
                         : rprim.GetPrimvar(delegate, descriptor.name);
      return result;
    }
  }
  return {};
}

template <typename ArrayType, typename ResultType, typename Convert>
bool ReadArrayElement(const VtValue& value, std::size_t index,
                      ResultType& result, Convert convert) {
  if (!value.IsHolding<ArrayType>()) {
    return false;
  }
  const auto& array = value.UncheckedGet<ArrayType>();
  if (index >= array.size()) {
    return false;
  }
  result = convert(array[index]);
  return true;
}

bool ReadVec3(const PrimvarInput& input, const MeshCorner& corner,
              merlin::Vec3& result) {
  const auto element = PrimvarElement(input, corner);
  if (!element) {
    return false;
  }
  const auto convert = [](const auto& value) {
    return merlin::Vec3{static_cast<float>(value[0]),
                        static_cast<float>(value[1]),
                        static_cast<float>(value[2])};
  };
  return ReadArrayElement<VtVec3fArray>(input.value, *element, result,
                                        convert) ||
         ReadArrayElement<VtVec3dArray>(input.value, *element, result,
                                        convert);
}

bool ReadVec2(const PrimvarInput& input, const MeshCorner& corner,
              merlin::Vec2& result) {
  const auto element = PrimvarElement(input, corner);
  if (!element) {
    return false;
  }
  const auto convert = [](const auto& value) {
    return merlin::Vec2{static_cast<float>(value[0]),
                        static_cast<float>(value[1])};
  };
  return ReadArrayElement<VtVec2fArray>(input.value, *element, result,
                                        convert) ||
         ReadArrayElement<VtVec2dArray>(input.value, *element, result,
                                        convert);
}

bool ReadFloat(const PrimvarInput& input, const MeshCorner& corner,
               float& result) {
  const auto element = PrimvarElement(input, corner);
  if (!element) {
    return false;
  }
  return ReadArrayElement<VtFloatArray>(
             input.value, *element, result,
             [](float value) { return value; }) ||
         ReadArrayElement<VtDoubleArray>(
             input.value, *element, result,
             [](double value) { return static_cast<float>(value); });
}

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

std::optional<std::filesystem::path> RegressionLogPath() {
#ifdef _WIN32
  char* value{};
  std::size_t size{};
  if (_dupenv_s(&value, &size, "MERLIN_HYDRA2_REGRESSION_LOG") != 0 ||
      value == nullptr) {
    return std::nullopt;
  }
  const std::filesystem::path result(value);
  std::free(value);
  return result;
#else
  if (const char* value =
          std::getenv("MERLIN_HYDRA2_REGRESSION_LOG")) {
    return std::filesystem::path(value);
  }
  return std::nullopt;
#endif
}

std::string RegressionPhase() {
#ifdef _WIN32
  char* value{};
  std::size_t size{};
  if (_dupenv_s(&value, &size, "MERLIN_HYDRA2_REGRESSION_PHASE") != 0 ||
      value == nullptr) {
    return "unlabeled";
  }
  std::string result(value);
  std::free(value);
  return result;
#else
  if (const char* value =
          std::getenv("MERLIN_HYDRA2_REGRESSION_PHASE")) {
    return value;
  }
  return "unlabeled";
#endif
}

bool ValidationRequested() {
#ifdef _WIN32
  char* value{};
  std::size_t size{};
  if (_dupenv_s(&value, &size, "MERLIN_HYDRA2_ENABLE_VALIDATION") != 0 ||
      value == nullptr) {
    return false;
  }
  const bool result = std::string_view(value) == "1";
  std::free(value);
  return result;
#else
  const char* value = std::getenv("MERLIN_HYDRA2_ENABLE_VALIDATION");
  return value != nullptr && std::string_view(value) == "1";
#endif
}

struct MeshState {
  merlin::MeshHandle mesh;
  std::vector<merlin::InstanceHandle> instances;
  merlin::MeshDescriptor mesh_descriptor;
  std::vector<merlin::InstanceDescriptor> instance_descriptors;
  // Key into SceneBridge::materials_ for the currently bound material, or empty
  // when the mesh uses the shared fallback material. Holds one reference.
  std::string material_key;
  bool authored_visible{true};
};

// Shared material tracked by authored path so the render-world material and its
// map entry are released once no mesh references it.
struct MaterialEntry {
  merlin::MaterialHandle handle;
  std::size_t references{};
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
    render_settings_descriptor_.label = "Hydra render pass";
    render_settings_descriptor_.color_aov = false;
    render_settings_descriptor_.depth_aov = false;
    render_settings_ =
        world_.CreateRenderSettings(render_settings_descriptor_);
  }

  void SyncMesh(const SdfPath& path, merlin::MeshDescriptor mesh,
                const SdfPath& material_path,
                const std::vector<merlin::Mat4>& transforms, bool visible,
                merlin::ChangeAspect mesh_aspects,
                merlin::ChangeAspect instance_aspects) {
    std::scoped_lock lock(mutex_);
    const auto key = path.GetString();
    auto found = meshes_.find(key);
    if (mesh.positions.empty() || mesh.indices.empty()) {
      if (found != meshes_.end()) {
        for (const auto instance : found->second.instances) {
          world_.Remove(instance);
        }
        world_.Remove(found->second.mesh);
        ReleaseMaterialLocked(found->second.material_key);
        meshes_.erase(found);
      }
      return;
    }
    if (found == meshes_.end()) {
      MeshState state;
      state.mesh_descriptor = std::move(mesh);
      state.mesh = world_.CreateMesh(state.mesh_descriptor);
      state.authored_visible = visible;
      const auto material =
          AcquireMaterialLocked(material_path, state.material_key);
      for (std::size_t i = 0; i < transforms.size(); ++i) {
        merlin::InstanceDescriptor descriptor;
        descriptor.label = key + "[" + std::to_string(i) + "]";
        descriptor.mesh = state.mesh;
        descriptor.material = material;
        descriptor.transform = transforms[i];
        descriptor.visible = visible;
        state.instances.push_back(world_.CreateInstance(descriptor));
        state.instance_descriptors.push_back(std::move(descriptor));
      }
      meshes_.emplace(key, std::move(state));
      return;
    }

    auto& state = found->second;
    state.mesh_descriptor = std::move(mesh);
    if (mesh_aspects != merlin::ChangeAspect::None) {
      world_.UpdateMesh(state.mesh, state.mesh_descriptor, mesh_aspects);
    }
    state.authored_visible = visible;
    // Re-resolve the binding. Acquire the new material before repointing any
    // instances and defer releasing the previous one until after the update
    // loop, so no committed instance references a retired material handle, and
    // an unchanged binding neither creates nor retires materials.
    merlin::MaterialHandle material;
    std::string released_key;
    const std::string desired_key =
        material_path.IsEmpty() ? std::string() : material_path.GetString();
    if (desired_key != state.material_key) {
      released_key = state.material_key;
      material = AcquireMaterialLocked(material_path, state.material_key);
    } else if (state.material_key.empty()) {
      material = fallback_material_;
    } else {
      material = materials_.at(state.material_key).handle;
    }
    while (state.instances.size() > transforms.size()) {
      world_.Remove(state.instances.back());
      state.instances.pop_back();
      state.instance_descriptors.pop_back();
    }
    while (state.instances.size() < transforms.size()) {
      const auto i = state.instances.size();
      merlin::InstanceDescriptor descriptor;
      descriptor.label = key + "[" + std::to_string(i) + "]";
      descriptor.mesh = state.mesh;
      descriptor.material = material;
      descriptor.transform = transforms[i];
      descriptor.visible = visible;
      state.instances.push_back(world_.CreateInstance(descriptor));
      state.instance_descriptors.push_back(std::move(descriptor));
    }
    for (std::size_t i = 0; i < state.instances.size(); ++i) {
      auto& descriptor = state.instance_descriptors[i];
      descriptor.transform = transforms[i];
      descriptor.visible = visible;
      descriptor.material = material;
      if (instance_aspects != merlin::ChangeAspect::None) {
        world_.UpdateInstance(state.instances[i], descriptor,
                              instance_aspects);
      }
    }
    // No-op unless the binding changed above; retires the previous material
    // once its last referencing mesh has moved off it.
    ReleaseMaterialLocked(released_key);
  }

  void RemoveMesh(const SdfPath& path) {
    std::scoped_lock lock(mutex_);
    const auto found = meshes_.find(path.GetString());
    if (found == meshes_.end()) {
      return;
    }
    for (const auto instance : found->second.instances) {
      world_.Remove(instance);
    }
    world_.Remove(found->second.mesh);
    ReleaseMaterialLocked(found->second.material_key);
    meshes_.erase(found);
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
      for (std::size_t i = 0; i < mesh.instances.size(); ++i) {
        auto& descriptor = mesh.instance_descriptors[i];
        if (descriptor.visible != visible) {
          descriptor.visible = visible;
          world_.UpdateInstance(mesh.instances[i], descriptor,
                                merlin::ChangeAspect::Visibility);
        }
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

    std::uint32_t width{};
    std::uint32_t height{};
    bool color_aov{};
    bool depth_aov{};
    for (const auto& binding : bindings) {
      color_aov = color_aov || binding.aovName == HdAovTokens->color;
      depth_aov = depth_aov || HdAovHasDepthSemantic(binding.aovName);
      HdRenderBuffer* buffer = binding.renderBuffer;
      if (buffer == nullptr && !binding.renderBufferId.IsEmpty()) {
        buffer = dynamic_cast<HdRenderBuffer*>(render_index->GetBprim(
            HdPrimTypeTokens->renderBuffer, binding.renderBufferId));
      }
      if (width == 0 && buffer != nullptr && buffer->GetWidth() != 0 &&
          buffer->GetHeight() != 0) {
        width = buffer->GetWidth();
        height = buffer->GetHeight();
      }
    }
    if (width == 0 || height == 0) {
      return;
    }
    if (render_settings_descriptor_.width != width ||
        render_settings_descriptor_.height != height ||
        render_settings_descriptor_.color_aov != color_aov ||
        render_settings_descriptor_.depth_aov != depth_aov) {
      render_settings_descriptor_.width = width;
      render_settings_descriptor_.height = height;
      render_settings_descriptor_.color_aov = color_aov;
      render_settings_descriptor_.depth_aov = depth_aov;
      world_.UpdateRenderSettings(render_settings_,
                                  render_settings_descriptor_);
    }

    const auto changes = world_.Commit();
    if (!changes.empty()) {
      extractor_.Apply(world_, changes);
    }

    std::uint32_t mesh_aspects{};
    std::uint32_t material_aspects{};
    std::uint32_t instance_aspects{};
    std::uint32_t camera_aspects{};
    std::uint32_t render_settings_aspects{};
    std::uint64_t mesh_resource_revision{};
    std::uint64_t material_resource_revision{};
    std::uint64_t instance_resource_revision{};
    std::uint64_t camera_resource_revision{};
    std::uint64_t render_settings_resource_revision{};
    for (const auto& change : changes.changes) {
      const auto aspects = static_cast<std::uint32_t>(change.aspects);
      switch (change.object_kind) {
        case merlin::ObjectKind::Mesh:
          mesh_aspects |= aspects;
          mesh_resource_revision =
              std::max(mesh_resource_revision, change.resource_revision);
          break;
        case merlin::ObjectKind::Material:
          material_aspects |= aspects;
          material_resource_revision =
              std::max(material_resource_revision, change.resource_revision);
          break;
        case merlin::ObjectKind::Instance:
          instance_aspects |= aspects;
          instance_resource_revision =
              std::max(instance_resource_revision, change.resource_revision);
          break;
        case merlin::ObjectKind::Camera:
          camera_aspects |= aspects;
          camera_resource_revision =
              std::max(camera_resource_revision, change.resource_revision);
          break;
        case merlin::ObjectKind::Light:
          break;
        case merlin::ObjectKind::RenderSettings:
          render_settings_aspects |= aspects;
          render_settings_resource_revision = std::max(
              render_settings_resource_revision, change.resource_revision);
          break;
      }
    }

    if (!renderer_) {
      const bool validation_requested = ValidationRequested();
      renderer_ = std::make_unique<merlin::vulkan::Renderer>(
          merlin::vulkan::RendererOptions{validation_requested});
      if (validation_requested &&
          !renderer_->capabilities().validation_enabled) {
        throw std::runtime_error(
            "Hydra regression requested unavailable Vulkan validation");
      }
    }
    const auto shader_dir = PluginDirectory() / "shaders";
    const merlin::vulkan::ShaderPaths shaders{
        shader_dir / "triangle.vert.spv", shader_dir / "triangle.frag.spv"};
    const auto snapshot = extractor_.snapshot();
    merlin::vulkan::RenderRequest request;
    request.snapshot = snapshot;
    request.width = width;
    request.height = height;
    request.shaders = shaders;
    request.products.clear();
    const auto request_product = [&](merlin::Aov aov) {
      const auto found = std::find_if(
          request.products.begin(), request.products.end(),
          [aov](const merlin::vulkan::RenderProductRequest& product) {
            return product.aov == aov;
          });
      if (found == request.products.end()) {
        request.products.push_back({aov, true});
      }
    };
    for (const auto& binding : bindings) {
      if (binding.aovName == HdAovTokens->color) {
        request_product(merlin::Aov::Color);
      } else if (HdAovHasDepthSemantic(binding.aovName)) {
        request_product(merlin::Aov::Depth);
      } else if (binding.aovName == HdAovTokens->primId) {
        request_product(merlin::Aov::PrimId);
      } else if (binding.aovName == HdAovTokens->instanceId) {
        request_product(merlin::Aov::InstanceId);
      }
    }
    // Regression evidence uses depth coverage even when the host binds only a
    // display color product.
    request_product(merlin::Aov::Depth);
    const auto result = renderer_->Resolve(renderer_->Submit(request));
    const auto renderer_statistics = renderer_->statistics();
    if (ValidationRequested() &&
        renderer_statistics.validation_messages != 0) {
      throw std::runtime_error("Vulkan validation reported Hydra warnings");
    }

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
        written = buffer->WriteColor(result.color.pixels,
                                     result.color.product.width,
                                     result.color.product.height);
      } else if (HdAovHasDepthSemantic(binding.aovName)) {
        written = buffer->WriteDepth(result.depth.pixels,
                                     result.depth.product.width,
                                     result.depth.product.height);
      } else if (binding.aovName == HdAovTokens->primId) {
        written = buffer->WriteId(result.prim_id.pixels,
                                  result.prim_id.product.width,
                                  result.prim_id.product.height);
      } else if (binding.aovName == HdAovTokens->instanceId) {
        written = buffer->WriteId(result.instance_id.pixels,
                                  result.instance_id.product.width,
                                  result.instance_id.product.height);
      }
      buffer->SetConverged(written);
      buffers_written += written ? 1U : 0U;
    }

    if (buffers_written != 0) {
      if (const auto marker_path = RegressionLogPath()) {
        std::size_t covered_pixels{};
        std::uint64_t covered_x_sum{};
        std::uint64_t covered_y_sum{};
        for (std::uint32_t y = 0; y < result.depth.product.height; ++y) {
          for (std::uint32_t x = 0; x < result.depth.product.width; ++x) {
            const auto index =
                static_cast<std::size_t>(y) * result.depth.product.width + x;
            if (result.depth.pixels[index] < 1.0F) {
              ++covered_pixels;
              covered_x_sum += x;
              covered_y_sum += y;
            }
          }
        }
        if (marker_path->has_parent_path()) {
          std::filesystem::create_directories(marker_path->parent_path());
        }
        std::ofstream stream(*marker_path, std::ios::app);
        stream << "phase=" << RegressionPhase()
               << " scene_revision=" << result.scene_revision
               << " completion_value=" << result.completion_value
               << " draw_count=" << snapshot->draws.size()
               << " buffers_written=" << buffers_written
               << " width=" << result.depth.product.width
               << " height=" << result.depth.product.height
               << " covered_pixels=" << covered_pixels
               << " covered_x_sum=" << covered_x_sum
               << " covered_y_sum=" << covered_y_sum
               << " validation_enabled="
               << renderer_->capabilities().validation_enabled
               << " validation_messages="
               << renderer_statistics.validation_messages
               << " mesh_aspects=" << mesh_aspects
               << " material_aspects=" << material_aspects
               << " instance_aspects=" << instance_aspects
               << " camera_aspects=" << camera_aspects
               << " render_settings_aspects=" << render_settings_aspects
               << " mesh_resource_revision=" << mesh_resource_revision
               << " material_resource_revision="
               << material_resource_revision
               << " instance_resource_revision="
               << instance_resource_revision
               << " camera_resource_revision=" << camera_resource_revision
               << " render_settings_resource_revision="
               << render_settings_resource_revision
               << '\n';
      }
    }
  }

 private:
  // Resolves the material bound at `path`, creating it on first use, and takes
  // one reference. `key_out` receives the tracking key to pass back to
  // ReleaseMaterialLocked (empty for the untracked fallback material).
  merlin::MaterialHandle AcquireMaterialLocked(const SdfPath& path,
                                               std::string& key_out) {
    if (path.IsEmpty()) {
      key_out.clear();
      return fallback_material_;
    }
    auto key = path.GetString();
    auto found = materials_.find(key);
    if (found == materials_.end()) {
      merlin::MaterialDescriptor descriptor;
      descriptor.label = key;
      found = materials_
                  .emplace(key, MaterialEntry{
                                    world_.CreateMaterial(std::move(descriptor)),
                                    0})
                  .first;
    }
    ++found->second.references;
    key_out = std::move(key);
    return found->second.handle;
  }

  // Drops one reference to a tracked material, retiring the render-world
  // resource and map entry once the last mesh releases it.
  void ReleaseMaterialLocked(const std::string& key) {
    if (key.empty()) {
      return;
    }
    const auto found = materials_.find(key);
    if (found == materials_.end()) {
      return;
    }
    if (--found->second.references == 0) {
      world_.Remove(found->second.handle);
      materials_.erase(found);
    }
  }

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
      world_.UpdateCamera(handle, std::move(descriptor),
                          merlin::ChangeAspect::Camera);
    }
    return handle;
  }

  std::mutex mutex_;
  merlin::RenderWorld world_;
  merlin::extraction::SceneExtractor extractor_;
  std::unique_ptr<merlin::vulkan::Renderer> renderer_;
  merlin::MaterialHandle fallback_material_;
  merlin::RenderSettingsHandle render_settings_;
  merlin::RenderSettingsDescriptor render_settings_descriptor_;
  std::unordered_map<std::string, MeshState> meshes_;
  std::unordered_map<std::string, MaterialEntry> materials_;
  std::unordered_map<std::string, merlin::CameraHandle> cameras_;
};

class HdMerlinInstancer final : public HdInstancer {
 public:
  HdMerlinInstancer(HdSceneDelegate* delegate, const SdfPath& id)
      : HdInstancer(delegate, id) {}

  void Sync(HdSceneDelegate* delegate, HdRenderParam* render_param,
            HdDirtyBits* dirty_bits) override {
    (void)render_param;
    if ((*dirty_bits & HdChangeTracker::DirtyVisibility) != 0) {
      visible_ = delegate->GetVisible(GetId());
    }
    _UpdateInstancer(delegate, dirty_bits);
    if (HdChangeTracker::IsAnyPrimvarDirty(*dirty_bits, GetId())) {
      for (const auto& descriptor : delegate->GetPrimvarDescriptors(
               GetId(), HdInterpolationInstance)) {
        if (HdChangeTracker::IsPrimvarDirty(*dirty_bits, GetId(),
                                            descriptor.name)) {
          primvars_.insert_or_assign(descriptor.name,
                                     delegate->Get(GetId(), descriptor.name));
        }
      }
    }
    *dirty_bits = HdChangeTracker::Clean;
  }

  VtMatrix4dArray ComputeInstanceTransforms(const SdfPath& prototype_id) {
    if (!visible_) {
      return {};
    }
    const auto indices =
        GetDelegate()->GetInstanceIndices(GetId(), prototype_id);
    VtMatrix4dArray transforms(indices.size(),
        GetDelegate()->GetInstancerTransform(GetId()));
    ApplyVec3(transforms, indices, HdInstancerTokens->instanceTranslations,
              [&](GfMatrix4d& matrix, const GfVec3d& value) {
                GfMatrix4d operation(1.0);
                operation.SetTranslate(value);
                matrix = operation * matrix;
              });
    ApplyVec4(transforms, indices, HdInstancerTokens->instanceRotations,
              [&](GfMatrix4d& matrix, const GfVec4d& value) {
                GfMatrix4d operation(1.0);
                operation.SetRotate(
                    GfQuatd(value[0], value[1], value[2], value[3]));
                matrix = operation * matrix;
              });
    ApplyVec3(transforms, indices, HdInstancerTokens->instanceScales,
              [&](GfMatrix4d& matrix, const GfVec3d& value) {
                GfMatrix4d operation(1.0);
                operation.SetScale(value);
                matrix = operation * matrix;
              });
    const auto transform_found =
        primvars_.find(HdInstancerTokens->instanceTransforms);
    if (transform_found != primvars_.end() &&
        transform_found->second.IsHolding<VtMatrix4dArray>()) {
      const auto& values =
          transform_found->second.UncheckedGet<VtMatrix4dArray>();
      for (std::size_t i = 0; i < indices.size(); ++i) {
        const int index = indices[i];
        if (index >= 0 && static_cast<std::size_t>(index) < values.size()) {
          transforms[i] = values[index] * transforms[i];
        }
      }
    }
    if (GetParentId().IsEmpty()) {
      return transforms;
    }
    auto* parent = dynamic_cast<HdMerlinInstancer*>(
        GetDelegate()->GetRenderIndex().GetInstancer(GetParentId()));
    if (parent == nullptr) {
      TF_WARN("Merlin instancer %s has unavailable parent %s",
              GetId().GetText(), GetParentId().GetText());
      return transforms;
    }
    const auto parent_transforms = parent->ComputeInstanceTransforms(GetId());
    VtMatrix4dArray flattened(parent_transforms.size() * transforms.size());
    for (std::size_t parent_index = 0;
         parent_index < parent_transforms.size(); ++parent_index) {
      for (std::size_t child_index = 0; child_index < transforms.size();
           ++child_index) {
        flattened[parent_index * transforms.size() + child_index] =
            transforms[child_index] * parent_transforms[parent_index];
      }
    }
    return flattened;
  }

 private:
  template <typename Callback>
  void ApplyVec3(VtMatrix4dArray& transforms, const VtIntArray& indices,
                 const TfToken& token, Callback callback) {
    const auto found = primvars_.find(token);
    if (found == primvars_.end()) {
      return;
    }
    const auto apply = [&](const auto& values) {
      for (std::size_t i = 0; i < indices.size(); ++i) {
        const int index = indices[i];
        if (index >= 0 && static_cast<std::size_t>(index) < values.size()) {
          const auto& value = values[index];
          callback(transforms[i],
                   GfVec3d(value[0], value[1], value[2]));
        }
      }
    };
    if (found->second.IsHolding<VtVec3fArray>()) {
      apply(found->second.UncheckedGet<VtVec3fArray>());
    } else if (found->second.IsHolding<VtVec3dArray>()) {
      apply(found->second.UncheckedGet<VtVec3dArray>());
    }
  }

  template <typename Callback>
  void ApplyVec4(VtMatrix4dArray& transforms, const VtIntArray& indices,
                 const TfToken& token, Callback callback) {
    const auto found = primvars_.find(token);
    if (found == primvars_.end()) {
      return;
    }
    const auto apply = [&](const auto& values) {
      for (std::size_t i = 0; i < indices.size(); ++i) {
        const int index = indices[i];
        if (index >= 0 && static_cast<std::size_t>(index) < values.size()) {
          const auto& value = values[index];
          callback(transforms[i],
                   GfVec4d(value[0], value[1], value[2], value[3]));
        }
      }
    };
    if (found->second.IsHolding<VtVec4fArray>()) {
      apply(found->second.UncheckedGet<VtVec4fArray>());
    } else if (found->second.IsHolding<VtVec4dArray>()) {
      apply(found->second.UncheckedGet<VtVec4dArray>());
    }
  }

  bool visible_{true};
  std::unordered_map<TfToken, VtValue, TfToken::HashFunctor> primvars_;
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
           HdChangeTracker::DirtyPrimvar |
           HdChangeTracker::DirtyMaterialId |
           HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyVisibility |
           HdChangeTracker::DirtyRenderTag |
           HdChangeTracker::DirtyInstancer;
  }

  void Sync(HdSceneDelegate* delegate, HdRenderParam* render_param,
            HdDirtyBits* dirty_bits, const TfToken& repr_token) override {
    (void)render_param;
    (void)repr_token;
    _UpdateInstancer(delegate, dirty_bits);
    const bool points_dirty =
        (*dirty_bits & HdChangeTracker::DirtyPoints) != 0;
    const bool topology_dirty =
        (*dirty_bits & HdChangeTracker::DirtyTopology) != 0;
    merlin::ChangeAspect mesh_aspects = merlin::ChangeAspect::None;
    merlin::ChangeAspect instance_aspects = merlin::ChangeAspect::None;
    if (points_dirty) {
      mesh_aspects |= merlin::ChangeAspect::Points;
    }
    if (topology_dirty) {
      mesh_aspects |= merlin::ChangeAspect::Topology;
      topology_ = GetMeshTopology(delegate);
    }
    const bool primvars_dirty =
        (*dirty_bits & HdChangeTracker::DirtyPrimvar) != 0;
    if (primvars_dirty) {
      mesh_aspects |= merlin::ChangeAspect::Primvars;
    }
    if (points_dirty || topology_dirty || primvars_dirty) {
      RebuildMesh(delegate);
    }
    const bool transform_dirty =
        (*dirty_bits & HdChangeTracker::DirtyTransform) != 0;
    const bool instancer_dirty =
        HdChangeTracker::IsInstancerDirty(*dirty_bits, GetId());
    if (transform_dirty) {
      instance_aspects |= merlin::ChangeAspect::Transform;
      hydra_transform_ = delegate->GetTransform(GetId());
    }
    if (instancer_dirty) {
      instance_aspects |= merlin::ChangeAspect::Transform;
    }
    if (transform_dirty || instancer_dirty || transforms_.empty()) {
      transforms_.clear();
      if (GetInstancerId().IsEmpty()) {
        transforms_.push_back(ToMerlinMatrix(hydra_transform_));
      } else {
        HdInstancer::_SyncInstancerAndParents(delegate->GetRenderIndex(),
                                              GetInstancerId());
        auto* instancer = dynamic_cast<HdMerlinInstancer*>(
            delegate->GetRenderIndex().GetInstancer(GetInstancerId()));
        if (instancer == nullptr) {
          TF_WARN("Merlin mesh %s has unavailable instancer %s",
                  GetId().GetText(), GetInstancerId().GetText());
        } else {
          for (const auto& instance_transform :
               instancer->ComputeInstanceTransforms(GetId())) {
            transforms_.push_back(ToMerlinMatrix(hydra_transform_ *
                                                 instance_transform));
          }
        }
      }
    }
    if ((*dirty_bits & HdChangeTracker::DirtyVisibility) != 0) {
      instance_aspects |= merlin::ChangeAspect::Visibility;
      visible_ = delegate->GetVisible(GetId());
    }
    if ((*dirty_bits & HdChangeTracker::DirtyMaterialId) != 0) {
      instance_aspects |= merlin::ChangeAspect::MaterialBinding;
      material_id_ = delegate->GetMaterialId(GetId());
    }
    if ((*dirty_bits & HdChangeTracker::DirtyRenderTag) != 0) {
      instance_aspects |= merlin::ChangeAspect::Visibility;
    }
    bridge_->SyncMesh(GetId(), descriptor_, material_id_, transforms_, visible_,
                      mesh_aspects, instance_aspects);
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
  void RebuildMesh(HdSceneDelegate* delegate) {
    // Clears every packed vertex array so a partially built mesh never leaves
    // stale normals/colors/texcoords behind on an error return.
    const auto reset = [&] {
      descriptor_.positions.clear();
      descriptor_.normals.clear();
      descriptor_.colors.clear();
      descriptor_.texcoords.clear();
      descriptor_.indices.clear();
    };
    reset();

    std::vector<GfVec3d> points;
    const auto authored_points = GetPoints(delegate);
    if (authored_points.IsHolding<VtVec3fArray>()) {
      const auto& values = authored_points.UncheckedGet<VtVec3fArray>();
      points.reserve(values.size());
      for (const auto& value : values) {
        points.emplace_back(value);
      }
    } else if (authored_points.IsHolding<VtVec3dArray>()) {
      const auto& values = authored_points.UncheckedGet<VtVec3dArray>();
      points.assign(values.begin(), values.end());
    } else {
      TF_WARN("Merlin mesh %s has unsupported points type",
              GetId().GetText());
      return;
    }

    const auto& counts = topology_.GetFaceVertexCounts();
    const auto& authored_indices = topology_.GetFaceVertexIndices();
    const auto& authored_holes = topology_.GetHoleIndices();
    std::unordered_set<int> holes;
    for (const auto hole : authored_holes) {
      if (hole < 0 || static_cast<std::size_t>(hole) >= counts.size() ||
          !holes.insert(hole).second) {
        TF_WARN("Merlin mesh %s has invalid hole index %d",
                GetId().GetText(), hole);
        return;
      }
    }
    const auto normals = FindPrimvar(*this, delegate, HdTokens->normals);
    const auto colors = FindPrimvar(*this, delegate, HdTokens->displayColor);
    const auto opacities =
        FindPrimvar(*this, delegate, HdTokens->displayOpacity);
    const auto texcoords =
        FindPrimvar(*this, delegate, TfToken("st"), true);

    std::size_t offset{};
    for (std::uint32_t face_index = 0; face_index < counts.size();
         ++face_index) {
      const int count = counts[face_index];
      if (count < 3 || offset > authored_indices.size() ||
          static_cast<std::size_t>(count) >
              authored_indices.size() - offset) {
        TF_WARN("Merlin mesh %s face %u has malformed vertex count %d",
                GetId().GetText(), face_index, count);
        reset();
        return;
      }
      if (!holes.contains(face_index)) {
        std::vector<std::uint32_t> face_points;
        face_points.reserve(static_cast<std::size_t>(count));
        for (int corner = 0; corner < count; ++corner) {
          const int point = authored_indices[offset +
                                             static_cast<std::size_t>(corner)];
          if (point < 0 || static_cast<std::size_t>(point) >= points.size()) {
            TF_WARN("Merlin mesh %s face %u references invalid point %d",
                    GetId().GetText(), face_index, point);
            reset();
            return;
          }
          face_points.push_back(static_cast<std::uint32_t>(point));
        }
        std::string diagnostic;
        const auto triangles = TriangulateFace(face_points, points, diagnostic);
        if (triangles.empty()) {
          TF_WARN("Merlin mesh %s face %u: %s", GetId().GetText(),
                  face_index, diagnostic.c_str());
          reset();
          return;
        }
        for (const auto& triangle : triangles) {
          const auto& a = points[face_points[triangle[0]]];
          const auto& b = points[face_points[triangle[1]]];
          const auto& c = points[face_points[triangle[2]]];
          GfVec3d generated_normal = GfCross(b - a, c - a);
          generated_normal.Normalize();
          for (const auto local_corner : triangle) {
            const MeshCorner corner{face_points[local_corner], face_index,
                                    static_cast<std::uint32_t>(offset) +
                                        local_corner};
            const auto& point = points[corner.point];
            descriptor_.positions.push_back(
                {static_cast<float>(point[0]), static_cast<float>(point[1]),
                 static_cast<float>(point[2])});
            merlin::Vec3 normal{static_cast<float>(generated_normal[0]),
                                static_cast<float>(generated_normal[1]),
                                static_cast<float>(generated_normal[2])};
            if (normals.present && !ReadVec3(normals, corner, normal)) {
              TF_WARN("Merlin mesh %s has malformed normals primvar",
                      GetId().GetText());
              reset();
              return;
            }
            const float length = std::sqrt(normal.x * normal.x +
                                           normal.y * normal.y +
                                           normal.z * normal.z);
            if (length > 1.0e-20F) {
              normal.x /= length;
              normal.y /= length;
              normal.z /= length;
            }
            descriptor_.normals.push_back(normal);
            merlin::Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};
            merlin::Vec3 rgb;
            if (colors.present && !ReadVec3(colors, corner, rgb)) {
              TF_WARN("Merlin mesh %s has malformed displayColor primvar",
                      GetId().GetText());
              reset();
              return;
            }
            if (colors.present) {
              color.x = rgb.x;
              color.y = rgb.y;
              color.z = rgb.z;
            }
            if (opacities.present &&
                !ReadFloat(opacities, corner, color.w)) {
              TF_WARN("Merlin mesh %s has malformed displayOpacity primvar",
                      GetId().GetText());
              reset();
              return;
            }
            color.w = std::clamp(color.w, 0.0F, 1.0F);
            descriptor_.colors.push_back(color);
            merlin::Vec2 texcoord;
            if (texcoords.present && !ReadVec2(texcoords, corner, texcoord)) {
              TF_WARN("Merlin mesh %s has malformed texture coordinate primvar",
                      GetId().GetText());
              reset();
              return;
            }
            descriptor_.texcoords.push_back(texcoord);
            descriptor_.indices.push_back(
                static_cast<std::uint32_t>(descriptor_.indices.size()));
          }
        }
      }
      offset += static_cast<std::size_t>(count);
    }
    if (offset != authored_indices.size()) {
      reset();
      TF_WARN("Merlin mesh %s has trailing face-vertex indices",
              GetId().GetText());
    }
  }

  std::shared_ptr<SceneBridge> bridge_;
  merlin::MeshDescriptor descriptor_;
  HdMeshTopology topology_;
  SdfPath material_id_;
  GfMatrix4d hydra_transform_{1.0};
  std::vector<merlin::Mat4> transforms_{merlin::Mat4{}};
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
      (format != HdFormatUNorm8Vec4 && format != HdFormatFloat32 &&
       format != HdFormatInt32)) {
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

bool HdMerlinRenderBuffer::WriteId(const std::vector<std::uint32_t>& ids,
                                   std::uint32_t width,
                                   std::uint32_t height) {
  std::scoped_lock lock(mutex_);
  const auto byte_size = ids.size() * sizeof(std::uint32_t);
  if (format_ != HdFormatInt32 || map_count_ != 0 ||
      dimensions_ != GfVec3i(static_cast<int>(width), static_cast<int>(height),
                             1) ||
      byte_size != data_.size()) {
    return false;
  }
  std::memcpy(data_.data(), ids.data(), byte_size);
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
  return new HdMerlinInstancer(delegate, id);
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
  if (name == HdAovTokens->primId || name == HdAovTokens->instanceId) {
    return {HdFormatInt32, false, VtValue(-1)};
  }
  return {};
}

PXR_NAMESPACE_CLOSE_SCOPE
