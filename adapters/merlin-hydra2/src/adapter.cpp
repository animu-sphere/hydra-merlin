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
#include <pxr/base/trace/trace.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/meshTopology.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hio/image.h>
#include <pxr/usd/sdf/assetPath.h>

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
#include <atomic>
#include <charconv>
#include <cctype>
#include <chrono>
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

using CpuClock = std::chrono::steady_clock;

std::uint64_t ElapsedNanoseconds(CpuClock::time_point start) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(CpuClock::now() -
                                                           start)
          .count());
}

struct HydraTelemetrySnapshot {
  std::uint64_t hydra_sync_ns{};
  std::uint64_t hydra_sync_count{};
  std::uint64_t mesh_sync_count{};
  std::uint64_t material_sync_count{};
  std::uint64_t light_sync_count{};
  std::uint64_t instancer_sync_count{};
  std::uint64_t camera_sync_count{};
  std::uint64_t render_world_update_ns{};
  std::uint64_t snapshot_extraction_ns{};
  std::uint64_t points_fetch_count{};
  std::uint64_t topology_fetch_count{};
  std::uint64_t primvar_descriptor_fetch_count{};
  std::uint64_t primvar_fetch_count{};
  std::uint64_t material_fetch_count{};
};

struct HydraTelemetry {
  std::atomic<std::uint64_t> hydra_sync_ns{};
  std::atomic<std::uint64_t> hydra_sync_count{};
  std::atomic<std::uint64_t> mesh_sync_count{};
  std::atomic<std::uint64_t> material_sync_count{};
  std::atomic<std::uint64_t> light_sync_count{};
  std::atomic<std::uint64_t> instancer_sync_count{};
  std::atomic<std::uint64_t> camera_sync_count{};
  std::atomic<std::uint64_t> render_world_update_ns{};
  std::atomic<std::uint64_t> snapshot_extraction_ns{};
  std::atomic<std::uint64_t> points_fetch_count{};
  std::atomic<std::uint64_t> topology_fetch_count{};
  std::atomic<std::uint64_t> primvar_descriptor_fetch_count{};
  std::atomic<std::uint64_t> primvar_fetch_count{};
  std::atomic<std::uint64_t> material_fetch_count{};

  HydraTelemetrySnapshot Consume() {
    const auto take = [](std::atomic<std::uint64_t>& value) {
      return value.exchange(0, std::memory_order_relaxed);
    };
    return {take(hydra_sync_ns),
            take(hydra_sync_count),
            take(mesh_sync_count),
            take(material_sync_count),
            take(light_sync_count),
            take(instancer_sync_count),
            take(camera_sync_count),
            take(render_world_update_ns),
            take(snapshot_extraction_ns),
            take(points_fetch_count),
            take(topology_fetch_count),
            take(primvar_descriptor_fetch_count),
            take(primvar_fetch_count),
            take(material_fetch_count)};
  }
};

HydraTelemetry g_hydra_telemetry;

class ScopedHydraSync {
 public:
  ScopedHydraSync() : start_(CpuClock::now()) {}
  ~ScopedHydraSync() {
    g_hydra_telemetry.hydra_sync_ns.fetch_add(
        ElapsedNanoseconds(start_), std::memory_order_relaxed);
    g_hydra_telemetry.hydra_sync_count.fetch_add(1,
                                                  std::memory_order_relaxed);
  }

 private:
  CpuClock::time_point start_;
};

class ScopedAtomicTimer {
 public:
  explicit ScopedAtomicTimer(std::atomic<std::uint64_t>& destination)
      : destination_(destination), start_(CpuClock::now()) {}
  ~ScopedAtomicTimer() {
    destination_.fetch_add(ElapsedNanoseconds(start_),
                           std::memory_order_relaxed);
  }

 private:
  std::atomic<std::uint64_t>& destination_;
  CpuClock::time_point start_;
};

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
    g_hydra_telemetry.primvar_descriptor_fetch_count.fetch_add(
        1, std::memory_order_relaxed);
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
      g_hydra_telemetry.primvar_fetch_count.fetch_add(
          1, std::memory_order_relaxed);
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

const VtValue* FindParameter(const HdMaterialNode2& node, const char* name) {
  const auto found = node.parameters.find(TfToken(name));
  return found == node.parameters.end() ? nullptr : &found->second;
}

bool ReadScalar(const VtValue& value, float& result) {
  if (value.IsHolding<float>()) {
    result = value.UncheckedGet<float>();
    return true;
  }
  if (value.IsHolding<double>()) {
    result = static_cast<float>(value.UncheckedGet<double>());
    return true;
  }
  return false;
}

bool ReadColor(const VtValue& value, merlin::Vec4& result) {
  if (value.IsHolding<GfVec3f>()) {
    const auto& color = value.UncheckedGet<GfVec3f>();
    result = {color[0], color[1], color[2], result.w};
    return true;
  }
  if (value.IsHolding<GfVec3d>()) {
    const auto& color = value.UncheckedGet<GfVec3d>();
    result = {static_cast<float>(color[0]), static_cast<float>(color[1]),
              static_cast<float>(color[2]), result.w};
    return true;
  }
  if (value.IsHolding<GfVec4f>()) {
    const auto& color = value.UncheckedGet<GfVec4f>();
    result = {color[0], color[1], color[2], color[3]};
    return true;
  }
  if (value.IsHolding<GfVec4d>()) {
    const auto& color = value.UncheckedGet<GfVec4d>();
    result = {static_cast<float>(color[0]), static_cast<float>(color[1]),
              static_cast<float>(color[2]), static_cast<float>(color[3])};
    return true;
  }
  return false;
}

merlin::AddressMode ReadAddressMode(const HdMaterialNode2& node,
                                    const char* name) {
  const auto* value = FindParameter(node, name);
  if (value == nullptr) {
    return merlin::AddressMode::Repeat;
  }
  std::string token;
  if (value->IsHolding<TfToken>()) {
    token = value->UncheckedGet<TfToken>().GetString();
  } else if (value->IsHolding<std::string>()) {
    token = value->UncheckedGet<std::string>();
  }
  if (token == "clamp") {
    return merlin::AddressMode::ClampToEdge;
  }
  if (token == "mirror") {
    return merlin::AddressMode::MirroredRepeat;
  }
  return merlin::AddressMode::Repeat;
}

bool ReadPpmUnsigned(std::istream& stream, std::uint32_t& value) {
  std::string token;
  while (stream >> token) {
    if (!token.empty() && token.front() == '#') {
      stream.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
      continue;
    }
    const auto* begin = token.data();
    const auto* end = begin + token.size();
    const auto [parsed_end, error] = std::from_chars(begin, end, value);
    return error == std::errc{} && parsed_end == end;
  }
  return false;
}

std::optional<merlin::TextureDescriptor> LoadPpmTexture(
    const std::string& path, std::string& diagnostic) {
  std::ifstream stream(path, std::ios::binary);
  std::string magic;
  if (!(stream >> magic) || magic != "P3") {
    diagnostic = "unsupported PPM encoding in " + path;
    return std::nullopt;
  }

  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t maximum{};
  if (!ReadPpmUnsigned(stream, width) ||
      !ReadPpmUnsigned(stream, height) ||
      !ReadPpmUnsigned(stream, maximum) || width == 0 || height == 0 ||
      maximum == 0 || maximum > 65535U ||
      static_cast<std::uint64_t>(width) * height >
          std::numeric_limits<std::size_t>::max() / 4U) {
    diagnostic = "invalid PPM header in " + path;
    return std::nullopt;
  }

  merlin::TextureDescriptor texture;
  texture.label = path;
  texture.width = width;
  texture.height = height;
  texture.pixels.resize(static_cast<std::size_t>(width) * height * 4U);
  for (std::size_t pixel = 0;
       pixel < static_cast<std::size_t>(width) * height; ++pixel) {
    for (std::size_t component = 0; component < 3U; ++component) {
      std::uint32_t sample{};
      if (!ReadPpmUnsigned(stream, sample) || sample > maximum) {
        diagnostic = "invalid PPM pixel data in " + path;
        return std::nullopt;
      }
      texture.pixels[pixel * 4U + component] = static_cast<std::uint8_t>(
          (static_cast<std::uint64_t>(sample) * 255U + maximum / 2U) /
          maximum);
    }
    texture.pixels[pixel * 4U + 3U] = 255U;
  }
  return texture;
}

std::optional<merlin::TextureDescriptor> LoadTexture(
    const HdMaterialNode2& node, std::string& diagnostic) {
  const auto* file = FindParameter(node, "file");
  if (file == nullptr || !file->IsHolding<SdfAssetPath>()) {
    diagnostic = "UsdUVTexture has no resolved file asset";
    return std::nullopt;
  }
  const auto& asset = file->UncheckedGet<SdfAssetPath>();
  const auto path = asset.GetResolvedPath().empty() ? asset.GetAssetPath()
                                                     : asset.GetResolvedPath();
  auto extension = std::filesystem::path(path).extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char value) {
                   return static_cast<char>(std::tolower(value));
                 });
  if (extension == ".ppm") {
    return LoadPpmTexture(path, diagnostic);
  }
  const auto image = HioImage::OpenForReading(path, 0, 0,
                                              HioImage::SourceColorSpace::Auto,
                                              true);
  if (!image || image->GetWidth() <= 0 || image->GetHeight() <= 0) {
    diagnostic = "could not open texture image " + path;
    return std::nullopt;
  }
  merlin::TextureDescriptor texture;
  texture.label = path;
  texture.width = static_cast<std::uint32_t>(image->GetWidth());
  texture.height = static_cast<std::uint32_t>(image->GetHeight());
  texture.pixels.resize(static_cast<std::size_t>(texture.width) *
                        texture.height * 4U);
  HioImage::StorageSpec storage;
  storage.width = image->GetWidth();
  storage.height = image->GetHeight();
  storage.depth = 1;
  storage.format = HioFormatUNorm8Vec4;
  storage.flipped = false;
  storage.data = texture.pixels.data();
  if (!image->Read(storage)) {
    diagnostic = "could not decode texture image " + path;
    return std::nullopt;
  }
  return texture;
}

struct ParsedMaterial {
  merlin::MaterialDescriptor material;
  std::optional<merlin::TextureDescriptor> texture;
  merlin::SamplerDescriptor sampler;
  std::string diagnostic;
};

ParsedMaterial ParseMaterialResource(const SdfPath& path,
                                     const VtValue& resource) {
  ParsedMaterial result;
  result.material.label = path.GetString();
  result.sampler.label = path.GetString() + ":baseColorSampler";
  HdMaterialNetwork2 network;
  if (resource.IsHolding<HdMaterialNetwork2>()) {
    network = resource.UncheckedGet<HdMaterialNetwork2>();
  } else if (resource.IsHolding<HdMaterialNetworkMap>()) {
    network = HdConvertToHdMaterialNetwork2(
        resource.UncheckedGet<HdMaterialNetworkMap>());
  } else {
    result.diagnostic = "material resource is not a Hydra material network";
    return result;
  }

  const HdMaterialNode2* surface{};
  const auto terminal = network.terminals.find(HdMaterialTerminalTokens->surface);
  if (terminal != network.terminals.end()) {
    const auto node = network.nodes.find(terminal->second.upstreamNode);
    if (node != network.nodes.end()) {
      surface = &node->second;
    }
  }
  if (surface == nullptr) {
    const auto node = std::find_if(
        network.nodes.begin(), network.nodes.end(), [](const auto& entry) {
          return entry.second.nodeTypeId == TfToken("UsdPreviewSurface");
        });
    if (node != network.nodes.end()) {
      surface = &node->second;
    }
  }
  if (surface == nullptr ||
      surface->nodeTypeId != TfToken("UsdPreviewSurface")) {
    result.diagnostic =
        "unsupported surface network; using constant fallback material";
    return result;
  }

  if (const auto* value = FindParameter(*surface, "diffuseColor")) {
    (void)ReadColor(*value, result.material.parameters.base_color);
  }
  if (const auto* value = FindParameter(*surface, "metallic")) {
    (void)ReadScalar(*value, result.material.parameters.metallic);
  }
  if (const auto* value = FindParameter(*surface, "roughness")) {
    (void)ReadScalar(*value, result.material.parameters.roughness);
  }
  float opacity = result.material.parameters.base_color.w;
  if (const auto* value = FindParameter(*surface, "opacity")) {
    (void)ReadScalar(*value, opacity);
  }
  result.material.parameters.base_color.w = opacity;
  float opacity_threshold{};
  if (const auto* value = FindParameter(*surface, "opacityThreshold")) {
    (void)ReadScalar(*value, opacity_threshold);
  }
  if (opacity_threshold > 0.0F) {
    result.material.alpha_mode = merlin::AlphaMode::Masked;
    result.material.parameters.alpha_cutoff = opacity_threshold;
  } else if (opacity < 1.0F) {
    result.material.alpha_mode = merlin::AlphaMode::Blended;
  }

  const auto color_connection =
      surface->inputConnections.find(TfToken("diffuseColor"));
  if (color_connection == surface->inputConnections.end() ||
      color_connection->second.empty()) {
    return result;
  }
  const auto texture_node =
      network.nodes.find(color_connection->second.front().upstreamNode);
  if (texture_node == network.nodes.end() ||
      texture_node->second.nodeTypeId != TfToken("UsdUVTexture")) {
    result.diagnostic =
        "diffuseColor connection is unsupported; using constant color";
    return result;
  }
  result.texture = LoadTexture(texture_node->second, result.diagnostic);
  if (!result.texture) {
    return result;
  }
  result.sampler.address_u =
      ReadAddressMode(texture_node->second, "wrapS");
  result.sampler.address_v =
      ReadAddressMode(texture_node->second, "wrapT");
  result.material.features |= merlin::MaterialFeature::BaseColorTexture;
  return result;
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
  bool authored{};
  merlin::MaterialDescriptor descriptor;
  std::optional<merlin::TextureHandle> texture;
  std::optional<merlin::SamplerHandle> sampler;
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
    material.parameters.base_color = {0.18F, 0.78F, 1.0F, 1.0F};
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
    ScopedAtomicTimer update_timer(g_hydra_telemetry.render_world_update_ns);
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

  void SyncMaterial(const SdfPath& path, ParsedMaterial parsed) {
    ScopedAtomicTimer update_timer(g_hydra_telemetry.render_world_update_ns);
    std::scoped_lock lock(mutex_);
    const auto key = path.GetString();
    auto found = materials_.find(key);
    if (found == materials_.end()) {
      MaterialEntry entry;
      entry.authored = true;
      entry.descriptor = std::move(parsed.material);
      if (parsed.texture) {
        entry.texture = world_.CreateTexture(std::move(*parsed.texture));
        entry.sampler = world_.CreateSampler(std::move(parsed.sampler));
        entry.descriptor.base_color_texture = merlin::TextureBinding{
            *entry.texture, *entry.sampler, 0};
      }
      entry.handle = world_.CreateMaterial(entry.descriptor);
      materials_.emplace(key, std::move(entry));
      return;
    }

    auto& entry = found->second;
    entry.authored = true;
    auto descriptor = std::move(parsed.material);
    if (parsed.texture) {
      if (entry.texture) {
        world_.UpdateTexture(*entry.texture, std::move(*parsed.texture));
      } else {
        entry.texture = world_.CreateTexture(std::move(*parsed.texture));
      }
      if (entry.sampler) {
        world_.UpdateSampler(*entry.sampler, std::move(parsed.sampler));
      } else {
        entry.sampler = world_.CreateSampler(std::move(parsed.sampler));
      }
      descriptor.base_color_texture =
          merlin::TextureBinding{*entry.texture, *entry.sampler, 0};
    }
    world_.UpdateMaterial(entry.handle, descriptor);
    entry.descriptor = std::move(descriptor);
    if (!parsed.texture) {
      if (entry.texture) {
        world_.Remove(*entry.texture);
        entry.texture.reset();
      }
      if (entry.sampler) {
        world_.Remove(*entry.sampler);
        entry.sampler.reset();
      }
    }
  }

  void RemoveMaterial(const SdfPath& path) {
    std::scoped_lock lock(mutex_);
    const auto found = materials_.find(path.GetString());
    if (found == materials_.end()) {
      return;
    }
    found->second.authored = false;
    if (found->second.references == 0) {
      RetireMaterialLocked(found);
    }
  }

  void SyncLight(const SdfPath& path, merlin::LightDescriptor descriptor,
                 merlin::ChangeAspect aspects) {
    ScopedAtomicTimer update_timer(g_hydra_telemetry.render_world_update_ns);
    std::scoped_lock lock(mutex_);
    const auto key = path.GetString();
    const auto found = lights_.find(key);
    if (found == lights_.end()) {
      lights_.emplace(key, world_.CreateLight(std::move(descriptor)));
    } else if (aspects != merlin::ChangeAspect::None) {
      world_.UpdateLight(found->second, std::move(descriptor), aspects);
    }
  }

  void RemoveLight(const SdfPath& path) {
    std::scoped_lock lock(mutex_);
    const auto found = lights_.find(path.GetString());
    if (found == lights_.end()) {
      return;
    }
    world_.Remove(found->second);
    lights_.erase(found);
  }

  void Render(const HdRenderPassState& state,
              const HdRenderPassAovBindingVector& bindings,
              const HdRprimCollection& collection,
              const TfTokenVector& render_tags,
              HdRenderIndex* render_index) {
    const auto render_execute_start = CpuClock::now();
    std::scoped_lock lock(mutex_);
    std::uint64_t render_world_update_ns{};

    const auto visibility_update_start = CpuClock::now();
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
    render_world_update_ns += ElapsedNanoseconds(visibility_update_start);

    merlin::CameraHandle active_camera;
    if (const HdCamera* camera = state.GetCamera()) {
      const auto key = camera->GetId().GetString();
      const auto view = ToMerlinMatrix(state.GetWorldToViewMatrix());
      const auto projection = ToMerlinMatrix(state.GetProjectionMatrix());
      const auto camera_update_start = CpuClock::now();
      active_camera = SyncCameraLocked(key, view, projection);
      render_world_update_ns += ElapsedNanoseconds(camera_update_start);
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
      g_hydra_telemetry.render_world_update_ns.fetch_add(
          render_world_update_ns, std::memory_order_relaxed);
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
      const auto render_settings_update_start = CpuClock::now();
      world_.UpdateRenderSettings(render_settings_,
                                  render_settings_descriptor_);
      render_world_update_ns +=
          ElapsedNanoseconds(render_settings_update_start);
    }

    const auto commit_start = CpuClock::now();
    const auto changes = world_.Commit();
    render_world_update_ns += ElapsedNanoseconds(commit_start);
    g_hydra_telemetry.render_world_update_ns.fetch_add(
        render_world_update_ns, std::memory_order_relaxed);
    if (!changes.empty()) {
      const auto extraction_start = CpuClock::now();
      extractor_.Apply(world_, changes);
      g_hydra_telemetry.snapshot_extraction_ns.fetch_add(
          ElapsedNanoseconds(extraction_start), std::memory_order_relaxed);
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
        case merlin::ObjectKind::Texture:
        case merlin::ObjectKind::Sampler:
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

    const auto frame_telemetry = g_hydra_telemetry.Consume();
    const auto render_execute_ns = ElapsedNanoseconds(render_execute_start);

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
        const auto textured_materials = static_cast<std::size_t>(
            std::count_if(snapshot->materials.begin(),
                          snapshot->materials.end(), [](const auto& material) {
                            return material.base_color_texture.has_value();
                          }));
        const auto texcoord_geometries = static_cast<std::size_t>(
            std::count_if(snapshot->geometries.begin(),
                          snapshot->geometries.end(), [](const auto& geometry) {
                            return geometry.has_texcoords;
                          }));
        const auto missing_texcoord_geometries =
            snapshot->geometries.size() - texcoord_geometries;
        stream << "schema_version=3"
               << " phase=" << RegressionPhase()
               << " scene_revision=" << result.scene_revision
               << " completion_value=" << result.completion_value
               << " draw_count=" << snapshot->draws.size()
               << " buffers_written=" << buffers_written
               << " width=" << result.depth.product.width
               << " height=" << result.depth.product.height
               << " covered_pixels=" << covered_pixels
               << " covered_x_sum=" << covered_x_sum
               << " covered_y_sum=" << covered_y_sum
               << " textured_materials=" << textured_materials
               << " texcoord_geometries=" << texcoord_geometries
               << " missing_texcoord_geometries="
               << missing_texcoord_geometries
               << " material_fallbacks=" << snapshot->material_fallbacks.size()
               << " texture_cache_hits="
               << result.counters.texture_cache_hits
               << " texture_cache_misses="
               << result.counters.texture_cache_misses
               << " validation_enabled="
               << renderer_->capabilities().validation_enabled
               << " validation_messages="
               << renderer_statistics.validation_messages
               << " hydra_sync_ns=" << frame_telemetry.hydra_sync_ns
               << " hydra_sync_count=" << frame_telemetry.hydra_sync_count
               << " mesh_sync_count=" << frame_telemetry.mesh_sync_count
               << " material_sync_count="
               << frame_telemetry.material_sync_count
               << " light_sync_count=" << frame_telemetry.light_sync_count
               << " instancer_sync_count="
               << frame_telemetry.instancer_sync_count
               << " camera_sync_count=" << frame_telemetry.camera_sync_count
               << " scene_index_processing_ns=0"
               << " scene_index_processing_available=0"
               << " render_world_update_ns="
               << frame_telemetry.render_world_update_ns
               << " snapshot_extraction_ns="
               << frame_telemetry.snapshot_extraction_ns
               << " gpu_scene_update_ns=" << result.cpu_timings.upload_ns
               << " command_recording_ns="
               << result.cpu_timings.command_recording_ns
               << " queue_submission_ns="
               << result.cpu_timings.queue_submission_ns
               << " gpu_execution_ns="
               << result.cpu_timings.gpu_execution_ns
               << " completion_wait_ns="
               << result.cpu_timings.completion_wait_ns
               << " readback_ns=" << result.cpu_timings.readback_ns
               << " backend_total_ns="
               << result.cpu_timings.backend_total_ns
               << " render_pass_execute_ns=" << render_execute_ns
               << " points_fetch_count="
               << frame_telemetry.points_fetch_count
               << " topology_fetch_count="
               << frame_telemetry.topology_fetch_count
               << " primvar_descriptor_fetch_count="
               << frame_telemetry.primvar_descriptor_fetch_count
               << " primvar_fetch_count="
               << frame_telemetry.primvar_fetch_count
               << " material_fetch_count="
               << frame_telemetry.material_fetch_count
               // Resolve, Map, and CPU-to-Hgi upload happen after this render
               // pass returns. Their current-frame scopes are therefore
               // sourced exclusively from the enclosing OpenUSD host trace.
               << " render_buffer_resolve_ns=0"
               << " render_buffer_resolve_ns_available=0"
               << " render_buffer_map_ns=0"
               << " render_buffer_map_ns_available=0"
               << " host_upload_bytes=0 host_upload_bytes_available=0"
               << " host_upload_ns=0 host_upload_ns_available=0"
               << " host_composite_ns=0 host_composite_ns_available=0"
               << " presentation_ns=0 presentation_ns_available=0"
               << " requested_aov_count="
               << result.counters.requested_aov_count
               << " rendered_aov_count="
               << result.counters.rendered_aov_count
               << " cpu_readback_aov_count="
               << result.counters.cpu_readback_aov_count
               << " requested_aov_mask="
               << result.counters.requested_aov_mask
               << " rendered_aov_mask="
               << result.counters.rendered_aov_mask
               << " cpu_readback_aov_mask="
               << result.counters.cpu_readback_aov_mask
               << " upload_bytes=" << result.counters.upload_bytes
               << " readback_bytes=" << result.counters.readback_bytes
               << " allocation_count=" << result.counters.allocation_count
               << " visible_primitive_count="
               << result.counters.visible_primitive_count
               << " wait_count=" << result.counters.wait_count
               << " map_count=" << result.counters.map_count
               << " resolve_count=" << result.counters.resolve_count
               << " descriptor_pool_creation_count="
               << result.counters.descriptor_pool_creation_count
               << " descriptor_allocation_count="
               << result.counters.descriptor_allocation_count
               << " descriptor_update_count="
               << result.counters.descriptor_update_count
               << " pipeline_creation_count="
               << result.counters.pipeline_creation_count
               << " shader_module_cache_misses="
               << result.counters.shader_module_cache_misses
               << " geometry_cache_misses="
               << result.counters.geometry_cache_misses
               << " buffer_allocation_bytes="
               << result.counters.buffer_allocation_bytes
               << " image_allocation_bytes="
               << result.counters.image_allocation_bytes
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
      MaterialEntry entry;
      entry.descriptor.label = key;
      entry.handle = world_.CreateMaterial(entry.descriptor);
      found = materials_.emplace(key, std::move(entry)).first;
    }
    ++found->second.references;
    key_out = std::move(key);
    return found->second.handle;
  }

  void RetireMaterialLocked(
      std::unordered_map<std::string, MaterialEntry>::iterator found) {
    world_.Remove(found->second.handle);
    if (found->second.texture) {
      world_.Remove(*found->second.texture);
    }
    if (found->second.sampler) {
      world_.Remove(*found->second.sampler);
    }
    materials_.erase(found);
  }

  // Drops one mesh reference. Authored material Sprims keep their normalized
  // resources alive even while temporarily unbound.
  void ReleaseMaterialLocked(const std::string& key) {
    if (key.empty()) {
      return;
    }
    const auto found = materials_.find(key);
    if (found == materials_.end()) {
      return;
    }
    if (found->second.references != 0) {
      --found->second.references;
    }
    if (found->second.references == 0 && !found->second.authored) {
      RetireMaterialLocked(found);
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
  std::unordered_map<std::string, merlin::LightHandle> lights_;
};

class HdMerlinInstancer final : public HdInstancer {
 public:
  HdMerlinInstancer(HdSceneDelegate* delegate, const SdfPath& id)
      : HdInstancer(delegate, id) {}

  void Sync(HdSceneDelegate* delegate, HdRenderParam* render_param,
            HdDirtyBits* dirty_bits) override {
    ScopedHydraSync sync_timer;
    g_hydra_telemetry.instancer_sync_count.fetch_add(
        1, std::memory_order_relaxed);
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

class HdMerlinMaterial final : public HdMaterial {
 public:
  HdMerlinMaterial(const SdfPath& id, std::shared_ptr<SceneBridge> bridge)
      : HdMaterial(id), bridge_(std::move(bridge)) {}

  ~HdMerlinMaterial() override { bridge_->RemoveMaterial(GetId()); }

  void Sync(HdSceneDelegate* delegate, HdRenderParam* render_param,
            HdDirtyBits* dirty_bits) override {
    ScopedHydraSync sync_timer;
    g_hydra_telemetry.material_sync_count.fetch_add(
        1, std::memory_order_relaxed);
    (void)render_param;
    if (*dirty_bits != HdMaterial::Clean) {
      g_hydra_telemetry.material_fetch_count.fetch_add(
          1, std::memory_order_relaxed);
      auto parsed =
          ParseMaterialResource(GetId(), delegate->GetMaterialResource(GetId()));
      if (!parsed.diagnostic.empty()) {
        TF_WARN("Merlin material %s: %s", GetId().GetText(),
                parsed.diagnostic.c_str());
      }
      bridge_->SyncMaterial(GetId(), std::move(parsed));
    }
    *dirty_bits = HdMaterial::Clean;
  }

  HdDirtyBits GetInitialDirtyBitsMask() const override {
    return HdMaterial::AllDirty;
  }

 private:
  std::shared_ptr<SceneBridge> bridge_;
};

class HdMerlinDistantLight final : public HdLight {
 public:
  HdMerlinDistantLight(const SdfPath& id, std::shared_ptr<SceneBridge> bridge)
      : HdLight(id), bridge_(std::move(bridge)) {
    descriptor_.label = id.GetString();
    descriptor_.type = merlin::LightType::Directional;
  }

  ~HdMerlinDistantLight() override { bridge_->RemoveLight(GetId()); }

  void Sync(HdSceneDelegate* delegate, HdRenderParam* render_param,
            HdDirtyBits* dirty_bits) override {
    ScopedHydraSync sync_timer;
    g_hydra_telemetry.light_sync_count.fetch_add(1,
                                                  std::memory_order_relaxed);
    (void)render_param;
    merlin::ChangeAspect aspects = merlin::ChangeAspect::None;
    if ((*dirty_bits & HdLight::DirtyTransform) != 0) {
      descriptor_.transform = ToMerlinMatrix(delegate->GetTransform(GetId()));
      aspects |= merlin::ChangeAspect::Transform;
    }
    if ((*dirty_bits & HdLight::DirtyParams) != 0) {
      merlin::Vec4 color{descriptor_.color.x, descriptor_.color.y,
                         descriptor_.color.z, 1.0F};
      const auto color_value =
          delegate->GetLightParamValue(GetId(), HdLightTokens->color);
      (void)ReadColor(color_value, color);
      descriptor_.color = {color.x, color.y, color.z};
      float intensity = 1.0F;
      const auto intensity_value =
          delegate->GetLightParamValue(GetId(), HdLightTokens->intensity);
      (void)ReadScalar(intensity_value, intensity);
      float exposure{};
      const auto exposure_value =
          delegate->GetLightParamValue(GetId(), HdLightTokens->exposure);
      (void)ReadScalar(exposure_value, exposure);
      descriptor_.intensity = intensity * std::pow(2.0F, exposure);
      aspects |= merlin::ChangeAspect::LightParameters;
    }
    bridge_->SyncLight(GetId(), descriptor_, aspects);
    *dirty_bits = HdLight::Clean;
  }

  HdDirtyBits GetInitialDirtyBitsMask() const override {
    return HdLight::AllDirty;
  }

 private:
  std::shared_ptr<SceneBridge> bridge_;
  merlin::LightDescriptor descriptor_;
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
    ScopedHydraSync sync_timer;
    g_hydra_telemetry.mesh_sync_count.fetch_add(1,
                                                 std::memory_order_relaxed);
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
      g_hydra_telemetry.topology_fetch_count.fetch_add(
          1, std::memory_order_relaxed);
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
    g_hydra_telemetry.points_fetch_count.fetch_add(
        1, std::memory_order_relaxed);
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
    const bool has_display_color = colors.present || opacities.present;

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
            if (has_display_color) {
              descriptor_.colors.push_back(color);
            }
            merlin::Vec2 texcoord;
            if (texcoords.present && !ReadVec2(texcoords, corner, texcoord)) {
              TF_WARN("Merlin mesh %s has malformed texture coordinate primvar",
                      GetId().GetText());
              reset();
              return;
            }
            if (texcoords.present) {
              descriptor_.texcoords.push_back(texcoord);
            }
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
    ScopedHydraSync sync_timer;
    g_hydra_telemetry.camera_sync_count.fetch_add(1,
                                                   std::memory_order_relaxed);
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
  TRACE_SCOPE("HdMerlinRenderBuffer::Map");
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

void HdMerlinRenderBuffer::Resolve() {
  TRACE_SCOPE("HdMerlinRenderBuffer::Resolve");
}

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
  static const TfTokenVector types{HdPrimTypeTokens->camera,
                                   HdPrimTypeTokens->material,
                                   HdPrimTypeTokens->distantLight};
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
  if (type_id == HdPrimTypeTokens->material) {
    return new HdMerlinMaterial(sprim_id, impl_->bridge);
  }
  if (type_id == HdPrimTypeTokens->distantLight) {
    return new HdMerlinDistantLight(sprim_id, impl_->bridge);
  }
  return nullptr;
}

HdSprim* HdMerlinRenderDelegate::CreateFallbackSprim(const TfToken& type_id) {
  if (type_id == HdPrimTypeTokens->camera) {
    return new HdMerlinCamera(SdfPath("/__merlinFallbackCamera"),
                              impl_->bridge);
  }
  if (type_id == HdPrimTypeTokens->material) {
    return new HdMerlinMaterial(SdfPath("/__merlinFallbackMaterial"),
                                impl_->bridge);
  }
  if (type_id == HdPrimTypeTokens->distantLight) {
    return new HdMerlinDistantLight(SdfPath("/__merlinFallbackLight"),
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
