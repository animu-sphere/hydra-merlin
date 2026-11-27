#include "adapter.hpp"
#include "distant_light.hpp"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/gf/quath.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/quaternion.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3h.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/trace/trace.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/glf/simpleLight.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/dataSourceLocator.h>
#include <pxr/imaging/hd/driver.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/legacyDisplayStyleSchema.h>
#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/meshTopology.h>
#include <pxr/imaging/hd/meshSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/sceneIndexObserver.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hgi/enums.h>
#include <pxr/imaging/hgi/hgi.h>
#include <pxr/imaging/hgi/texture.h>
#include <pxr/imaging/hgi/tokens.h>
#include <pxr/imaging/hgi/types.h>
#include <pxr/imaging/hio/image.h>
#include <pxr/imaging/pxOsd/tokens.h>
#include <pxr/usd/sdf/assetPath.h>

#include <merlin/core/render_world.hpp>
#include <merlin/core/diagnostic.hpp>
#include <merlin/core/gaussian.hpp>
#include <merlin/core/mesh_attributes.hpp>
#include <merlin/core/mesh_subdivision.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/render/backend.hpp>
#include <merlin/render/diffuse_environment.hpp>
#include <merlin/render/specular_environment.hpp>
#include <merlin/render/surface_geometry.hpp>
#ifdef MERLIN_HYDRA2_ENABLE_VULKAN
#include <merlin/vulkan/backend.hpp>
#include <merlin/vulkan/shader_abi.hpp>
#elif defined(MERLIN_HYDRA2_ENABLE_METAL)
#include <merlin/metal/backend.hpp>
#endif

#ifdef _WIN32
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

#ifdef MERLIN_HYDRA2_HAVE_OPENGL
#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
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
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifndef MERLIN_OPENUSD_VALIDATED_PXR_VERSION
#error "hdMerlin requires the configured OpenUSD compatibility contract"
#elif PXR_VERSION != MERLIN_OPENUSD_VALIDATED_PXR_VERSION
#error "hdMerlin was compiled with an OpenUSD header different from the configured SDK"
#endif

#ifdef MERLIN_OPENUSD_RELEASE_ONLY
#error "hdMerlin Debug requires Debug OpenUSD libraries; use Release or a matching Debug OpenUSD SDK"
#endif

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PUBLIC_TOKENS(HdMerlinRenderSettingsTokens,
    HD_MERLIN_RENDER_SETTINGS_TOKENS);

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
  std::uint64_t triangulation_rebuild_count{};
  std::uint64_t packed_mesh_rebuild_count{};
  std::uint64_t changed_vertex_count{};
  std::uint64_t diagnostic_count{};
  std::uint64_t coarse_primvar_invalidation_count{};
  std::uint64_t subdivision_plan_count{};
  std::uint64_t subdivision_eval_count{};
  std::uint64_t subdivision_eval_ns{};
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
  std::atomic<std::uint64_t> triangulation_rebuild_count{};
  std::atomic<std::uint64_t> packed_mesh_rebuild_count{};
  std::atomic<std::uint64_t> changed_vertex_count{};
  std::atomic<std::uint64_t> diagnostic_count{};
  std::atomic<std::uint64_t> coarse_primvar_invalidation_count{};
  std::atomic<std::uint64_t> subdivision_plan_count{};
  std::atomic<std::uint64_t> subdivision_eval_count{};
  std::atomic<std::uint64_t> subdivision_eval_ns{};

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
        take(material_fetch_count),
        take(triangulation_rebuild_count),
        take(packed_mesh_rebuild_count),
        take(changed_vertex_count),
        take(diagnostic_count),
        take(coarse_primvar_invalidation_count),
        take(subdivision_plan_count),
        take(subdivision_eval_count),
        take(subdivision_eval_ns)};
  }
};

HydraTelemetry g_hydra_telemetry;

const char* DiagnosticDispositionName(
    merlin::DiagnosticDisposition disposition) {
  switch (disposition) {
  case merlin::DiagnosticDisposition::Fallback:
    return "fallback";
  case merlin::DiagnosticDisposition::Rejected:
    return "rejected";
  case merlin::DiagnosticDisposition::Ignored:
    return "ignored";
  }
  return "rejected";
}

class HydraDiagnosticSink final : public merlin::DiagnosticSink {
public:
  void Report(const merlin::Diagnostic& diagnostic) override {
    g_hydra_telemetry.diagnostic_count.fetch_add(
        1, std::memory_order_relaxed);
    TF_WARN("schema=%s code=%s disposition=%s source=%s message=%s "
            "recovery=%s",
        merlin::kDiagnosticSchema.data(), diagnostic.code.c_str(),
        DiagnosticDispositionName(diagnostic.disposition),
        diagnostic.source.c_str(), diagnostic.message.c_str(),
        diagnostic.recovery.c_str());
    std::scoped_lock lock(mutex_);
    records_.push_back({next_sequence_++, diagnostic});
    if (records_.size() > kCapacity) {
      records_.pop_front();
    }
  }

  std::vector<merlin::Diagnostic> ReadSince(std::uint64_t& cursor) const {
    std::scoped_lock lock(mutex_);
    std::vector<merlin::Diagnostic> result;
    for (const auto& record : records_) {
      if (record.sequence > cursor) {
        result.push_back(record.diagnostic);
      }
    }
    if (!records_.empty()) {
      cursor = records_.back().sequence;
    }
    return result;
  }

private:
  struct Record {
    std::uint64_t sequence{};
    merlin::Diagnostic diagnostic;
  };
  static constexpr std::size_t kCapacity = 256;
  mutable std::mutex mutex_;
  std::deque<Record> records_;
  std::uint64_t next_sequence_{1};
};

HydraDiagnosticSink g_diagnostic_sink;

void ReportHydraDiagnostic(
    std::string code, const SdfPath& source, std::string message,
    merlin::DiagnosticDisposition disposition,
    std::string recovery) {
  g_diagnostic_sink.Report(
      {merlin::kDiagnosticSchemaVersion, std::move(code),
          merlin::DiagnosticSeverity::Warning, disposition, source.GetString(),
          std::move(message), std::move(recovery)});
}

std::string GaussianSettingsName(
    const merlin::render::GpuDrivenGaussianSettings& settings) {
  return std::string(
             merlin::render::GpuDrivenGaussianModeName(settings.mode)) +
         "/" +
         std::string(merlin::render::GaussianRasterPathName(settings.raster)) +
         (settings.adaptive_tile_bounds ? "/adaptive-bounds" : "");
}

// The source is the render setting key rather than a scene path.
void ReportRenderSettingRejection(const TfToken& key,
    const merlin::render::RendererSettingsValidationError& error,
    const merlin::render::GpuDrivenGaussianSettings& kept) {
  g_diagnostic_sink.Report({merlin::kDiagnosticSchemaVersion, error.code,
      merlin::DiagnosticSeverity::Warning,
      merlin::DiagnosticDisposition::Rejected, key.GetString(), error.message,
      "keep " + GaussianSettingsName(kept)});
}

std::optional<std::string> ReadRenderSettingString(const VtValue& value) {
  if (value.IsHolding<std::string>()) {
    return value.UncheckedGet<std::string>();
  }
  if (value.IsHolding<TfToken>()) {
    return value.UncheckedGet<TfToken>().GetString();
  }
  return std::nullopt;
}

// Applies one Gaussian render setting to settings. Returns the rejection
// when the value is not a name in the renderer settings vocabulary, or not a
// bool for a flag.
std::optional<merlin::render::RendererSettingsValidationError>
ReadGaussianRenderSetting(const TfToken& key, const VtValue& value,
    merlin::render::GpuDrivenGaussianSettings& settings) {
  if (key == HdMerlinRenderSettingsTokens->gpuDrivenGaussianEnabled) {
    if (!value.IsHolding<bool>()) {
      return merlin::render::RendererSettingsValidationError{
          "renderer-settings.invalid-gpu-driven-gaussian-mode",
          "GPU Gaussian execution must be on or off."};
    }
    // Turning the flag on keeps an explicit require.
    if (!value.UncheckedGet<bool>()) {
      settings.mode = merlin::render::GpuDrivenGaussianMode::Disabled;
    } else if (settings.mode ==
               merlin::render::GpuDrivenGaussianMode::Disabled) {
      settings.mode = merlin::render::GpuDrivenGaussianMode::Prefer;
    }
    return std::nullopt;
  }
  if (key == HdMerlinRenderSettingsTokens->gpuDrivenGaussianTiled) {
    if (!value.IsHolding<bool>()) {
      return merlin::render::RendererSettingsValidationError{
          "renderer-settings.invalid-gaussian-raster-path",
          "Gaussian tile raster must be on or off."};
    }
    settings.raster = value.UncheckedGet<bool>()
                          ? merlin::render::GaussianRasterPath::Tiled
                          : merlin::render::GaussianRasterPath::SortedStream;
    return std::nullopt;
  }
  if (key == HdMerlinRenderSettingsTokens->gpuDrivenGaussianAdaptiveBounds) {
    if (!value.IsHolding<bool>()) {
      return merlin::render::RendererSettingsValidationError{
          "renderer-settings.invalid-gaussian-adaptive-bounds",
          "Gaussian adaptive tile bounds must be on or off."};
    }
    settings.adaptive_tile_bounds = value.UncheckedGet<bool>();
    return std::nullopt;
  }
  const auto name = ReadRenderSettingString(value);
  if (key == HdMerlinRenderSettingsTokens->gpuDrivenGaussianMode) {
    for (const auto mode : {merlin::render::GpuDrivenGaussianMode::Disabled,
             merlin::render::GpuDrivenGaussianMode::Prefer,
             merlin::render::GpuDrivenGaussianMode::Require}) {
      if (name && *name == merlin::render::GpuDrivenGaussianModeName(mode)) {
        settings.mode = mode;
        return std::nullopt;
      }
    }
    return merlin::render::RendererSettingsValidationError{
        "renderer-settings.invalid-gpu-driven-gaussian-mode",
        "GPU-driven Gaussian execution mode must be disabled, prefer, or "
        "require."};
  }
  for (const auto path : {merlin::render::GaussianRasterPath::SortedStream,
           merlin::render::GaussianRasterPath::Tiled}) {
    if (name && *name == merlin::render::GaussianRasterPathName(path)) {
      settings.raster = path;
      return std::nullopt;
    }
  }
  return merlin::render::RendererSettingsValidationError{
      "renderer-settings.invalid-gaussian-raster-path",
      "Gaussian raster path must be sorted-stream or tiled."};
}

// The policy names come before the flags, so a host that passes both at
// creation has the flags refine the names.
const std::array<TfToken, 5>& GaussianRenderSettingKeys() {
  static const std::array<TfToken, 5> keys{
      HdMerlinRenderSettingsTokens->gpuDrivenGaussianMode,
      HdMerlinRenderSettingsTokens->gpuDrivenGaussianRaster,
      HdMerlinRenderSettingsTokens->gpuDrivenGaussianEnabled,
      HdMerlinRenderSettingsTokens->gpuDrivenGaussianTiled,
      HdMerlinRenderSettingsTokens->gpuDrivenGaussianAdaptiveBounds};
  return keys;
}

bool IsGaussianRenderSetting(const TfToken& key) {
  const auto& keys = GaussianRenderSettingKeys();
  return std::find(keys.begin(), keys.end(), key) != keys.end();
}

VtValue GaussianRenderSettingValue(const TfToken& key,
    const merlin::render::GpuDrivenGaussianSettings& settings) {
  if (key == HdMerlinRenderSettingsTokens->gpuDrivenGaussianEnabled) {
    return VtValue(
        settings.mode != merlin::render::GpuDrivenGaussianMode::Disabled);
  }
  if (key == HdMerlinRenderSettingsTokens->gpuDrivenGaussianTiled) {
    return VtValue(
        settings.raster == merlin::render::GaussianRasterPath::Tiled);
  }
  if (key == HdMerlinRenderSettingsTokens->gpuDrivenGaussianAdaptiveBounds) {
    return VtValue(settings.adaptive_tile_bounds);
  }
  return VtValue(std::string(
      key == HdMerlinRenderSettingsTokens->gpuDrivenGaussianMode
          ? merlin::render::GpuDrivenGaussianModeName(settings.mode)
          : merlin::render::GaussianRasterPathName(settings.raster)));
}

struct SceneIndexPrimvarSources {
  HdSampledDataSourceHandle value;
  HdIntArrayDataSourceHandle indices;

  [[nodiscard]] bool available() const noexcept {
    return bool(value);
  }

  friend bool operator==(const SceneIndexPrimvarSources&,
      const SceneIndexPrimvarSources&) = default;
};

class HydraDirtyTracker final : public HdSceneIndexObserver {
public:
  void SetSceneIndex(const HdSceneIndexBaseRefPtr& scene_index) {
    std::scoped_lock lock(mutex_);
    scene_index_ = scene_index;
  }

  SceneIndexPrimvarSources GetPrimvarSources(const SdfPath& path,
      const TfToken& name) const {
    HdSceneIndexBaseRefPtr scene_index;
    {
      std::scoped_lock lock(mutex_);
      scene_index = scene_index_;
    }
    if (!scene_index) {
      return {};
    }
    const auto prim = scene_index->GetPrim(path);
    if (!prim) {
      return {};
    }
    const auto primvar =
        HdPrimvarsSchema::GetFromParent(prim.dataSource).GetPrimvar(name);
    if (!primvar) {
      return {};
    }
    return {primvar.IsIndexed() ? primvar.GetIndexedPrimvarValue()
                                : primvar.GetPrimvarValue(),
        primvar.GetIndices()};
  }

  std::optional<HdDataSourceLocatorSet> Consume(const SdfPath& path) {
    std::scoped_lock lock(mutex_);
    const auto found = dirty_.find(path);
    if (found == dirty_.end()) {
      return std::nullopt;
    }
    auto result = std::move(found->second);
    dirty_.erase(found);
    return result;
  }

  void PrimsAdded(const HdSceneIndexBase& sender,
      const AddedPrimEntries& entries) override {
    (void)sender;
    std::scoped_lock lock(mutex_);
    for (const auto& entry : entries) {
      dirty_.insert_or_assign(entry.primPath,
          HdDataSourceLocatorSet::UniversalSet());
    }
  }

  void PrimsRemoved(const HdSceneIndexBase& sender,
      const RemovedPrimEntries& entries) override {
    (void)sender;
    std::scoped_lock lock(mutex_);
    for (const auto& entry : entries) {
      std::erase_if(dirty_, [&](const auto& item) {
        return item.first.HasPrefix(entry.primPath);
      });
    }
  }

  void PrimsDirtied(const HdSceneIndexBase& sender,
      const DirtiedPrimEntries& entries) override {
    (void)sender;
    std::scoped_lock lock(mutex_);
    for (const auto& entry : entries) {
      dirty_[entry.primPath].insert(entry.dirtyLocators);
    }
  }

  void PrimsRenamed(const HdSceneIndexBase& sender,
      const RenamedPrimEntries& entries) override {
    (void)sender;
    std::scoped_lock lock(mutex_);
    for (const auto& entry : entries) {
      std::erase_if(dirty_, [&](const auto& item) {
        return item.first.HasPrefix(entry.oldPrimPath);
      });
      dirty_.insert_or_assign(entry.newPrimPath,
          HdDataSourceLocatorSet::UniversalSet());
    }
  }

private:
  mutable std::mutex mutex_;
  HdSceneIndexBaseRefPtr scene_index_;
  std::unordered_map<SdfPath, HdDataSourceLocatorSet, SdfPath::Hash> dirty_;
};

class ScopedHydraSync {
public:
  ScopedHydraSync() : start_(CpuClock::now()) {
  }
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
      : destination_(destination), start_(CpuClock::now()) {
  }
  ~ScopedAtomicTimer() {
    destination_.fetch_add(ElapsedNanoseconds(start_),
        std::memory_order_relaxed);
  }

private:
  std::atomic<std::uint64_t>& destination_;
  CpuClock::time_point start_;
};

using merlin::MeshCorner;

struct PrimvarInput {
  VtValue value;
  VtIntArray indices;
  TfToken source_name;
  HdInterpolation interpolation{HdInterpolationConstant};
  bool indexed{};
  bool present{};
  SceneIndexPrimvarSources sources;
};

std::vector<std::array<std::uint32_t, 3>> TriangulateFace(
    std::span<const std::uint32_t> points, std::string& diagnostic) {
  if (points.size() < 3) {
    diagnostic = "face has fewer than three vertices";
    return {};
  }
  // Match HdMeshUtil's authored-order fan triangulation. Hydra render
  // delegates must accept non-planar polygons consistently: projecting such a
  // face to an inferred plane can collapse otherwise usable corners and reject
  // the complete rprim, as happened with Kitchen_set's twisted stove quad.
  std::vector<std::array<std::uint32_t, 3>> triangles;
  triangles.reserve(points.size() - 2U);
  for (std::uint32_t corner = 1U; corner + 1U < points.size(); ++corner) {
    triangles.push_back({0U, corner, corner + 1U});
  }
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

std::optional<merlin::PrimvarInterpolation> ToPrimvarInterpolation(
    HdInterpolation interpolation) {
  switch (interpolation) {
  case HdInterpolationConstant:
    return merlin::PrimvarInterpolation::Constant;
  case HdInterpolationUniform:
    return merlin::PrimvarInterpolation::Uniform;
  case HdInterpolationVertex:
    return merlin::PrimvarInterpolation::Vertex;
  case HdInterpolationVarying:
    return merlin::PrimvarInterpolation::Varying;
  case HdInterpolationFaceVarying:
    return merlin::PrimvarInterpolation::FaceVarying;
  case HdInterpolationInstance:
  case HdInterpolationCount:
    break;
  }
  return std::nullopt;
}

template <typename Rule, std::size_t N>
Rule SubdivisionRule(const TfToken& token,
    const std::array<std::string_view, N>& names, Rule default_rule) {
  if (token.IsEmpty()) {
    return default_rule;
  }
  const auto found = std::ranges::find(names, token.GetString());
  return found == names.end() ? Rule::Unsupported
                              : static_cast<Rule>(found - names.begin());
}

merlin::SubdivisionTopology ToSubdivisionTopology(
    const HdMeshTopology& topology, const PxOsdSubdivTags& tags) {
  using namespace merlin;
  using namespace std::literals;
  SubdivisionTopology result;
  result.scheme = SubdivisionRule(topology.GetScheme(),
      std::array{"none"sv, "catmullClark"sv}, SubdivisionScheme::None);
  result.boundary = SubdivisionRule(tags.GetVertexInterpolationRule(),
      std::array{"none"sv, "edgeOnly"sv, "edgeAndCorner"sv}, result.boundary);
  result.face_varying = SubdivisionRule(tags.GetFaceVaryingInterpolationRule(),
      std::array{"none"sv, "cornersOnly"sv, "cornersPlus1"sv,
          "cornersPlus2"sv, "boundaries"sv, "all"sv},
      result.face_varying);
  result.triangle_rule = SubdivisionRule(tags.GetTriangleSubdivision(),
      std::array{"catmullClark"sv, "smooth"sv}, result.triangle_rule);
  result.crease_method = SubdivisionRule(tags.GetCreaseMethod(),
      std::array{"uniform"sv, "chaikin"sv}, result.crease_method);
  result.left_handed = topology.GetOrientation() == HdTokens->leftHanded;
  result.face_counts.assign(topology.GetFaceVertexCounts().begin(),
      topology.GetFaceVertexCounts().end());
  result.point_indices.assign(topology.GetFaceVertexIndices().begin(),
      topology.GetFaceVertexIndices().end());
  result.holes.assign(topology.GetHoleIndices().begin(),
      topology.GetHoleIndices().end());
  result.crease_lengths.assign(tags.GetCreaseLengths().begin(),
      tags.GetCreaseLengths().end());
  result.crease_indices.assign(tags.GetCreaseIndices().begin(),
      tags.GetCreaseIndices().end());
  result.crease_sharpness.assign(tags.GetCreaseWeights().begin(),
      tags.GetCreaseWeights().end());
  result.corner_indices.assign(tags.GetCornerIndices().begin(),
      tags.GetCornerIndices().end());
  result.corner_sharpness.assign(tags.GetCornerWeights().begin(),
      tags.GetCornerWeights().end());
  return result;
}

template <typename FloatArray, typename DoubleArray, typename Convert>
std::optional<merlin::SubdivisionPrimvar> ToSubdivisionPrimvar(
    const PrimvarInput& input, Convert convert) {
  if (!input.present) {
    return std::nullopt;
  }
  merlin::SubdivisionPrimvar result;
  const auto interpolation = ToPrimvarInterpolation(input.interpolation);
  if (interpolation) {
    result.interpolation = *interpolation;
    const auto copy = [&](const auto& values) {
      result.values.reserve(values.size());
      std::ranges::transform(values, std::back_inserter(result.values), convert);
    };
    if (input.value.IsHolding<FloatArray>()) {
      copy(input.value.UncheckedGet<FloatArray>());
    } else if (input.value.IsHolding<DoubleArray>()) {
      copy(input.value.UncheckedGet<DoubleArray>());
    }
    result.indices.assign(input.indices.begin(), input.indices.end());
  }
  return result;
}

// Resolves Hydra storage into the host-neutral primvar consumed by shared
// normal preparation; the element mapping itself is not duplicated here.
std::optional<merlin::NormalPrimvar> ToNormalPrimvar(
    const PrimvarInput& input) {
  merlin::NormalPrimvar result;
  const auto interpolation = ToPrimvarInterpolation(input.interpolation);
  if (!interpolation) {
    return std::nullopt;
  }
  result.interpolation = *interpolation;
  const auto append = [&](const auto& values) {
    result.values.reserve(values.size());
    for (const auto& value : values) {
      result.values.push_back({static_cast<float>(value[0]),
          static_cast<float>(value[1]), static_cast<float>(value[2])});
    }
  };
  if (input.value.IsHolding<VtVec3fArray>()) {
    append(input.value.UncheckedGet<VtVec3fArray>());
  } else if (input.value.IsHolding<VtVec3dArray>()) {
    append(input.value.UncheckedGet<VtVec3dArray>());
  } else {
    return std::nullopt;
  }
  result.indices.assign(input.indices.begin(), input.indices.end());
  return result;
}

// A float4 `tangents` primvar carries handedness in w; a float3 one is
// right-handed (w = +1).
std::optional<merlin::TangentPrimvar> ToTangentPrimvar(
    const PrimvarInput& input) {
  merlin::TangentPrimvar result;
  const auto interpolation = ToPrimvarInterpolation(input.interpolation);
  if (!interpolation) {
    return std::nullopt;
  }
  result.interpolation = *interpolation;
  const auto append = [&](const auto& values, bool has_handedness) {
    result.values.reserve(values.size());
    for (const auto& value : values) {
      result.values.push_back({static_cast<float>(value[0]),
          static_cast<float>(value[1]), static_cast<float>(value[2]),
          has_handedness ? static_cast<float>(value[3]) : 1.0F});
    }
  };
  if (input.value.IsHolding<VtVec4fArray>()) {
    append(input.value.UncheckedGet<VtVec4fArray>(), true);
  } else if (input.value.IsHolding<VtVec4dArray>()) {
    append(input.value.UncheckedGet<VtVec4dArray>(), true);
  } else if (input.value.IsHolding<VtVec3fArray>()) {
    append(input.value.UncheckedGet<VtVec3fArray>(), false);
  } else if (input.value.IsHolding<VtVec3dArray>()) {
    append(input.value.UncheckedGet<VtVec3dArray>(), false);
  } else {
    return std::nullopt;
  }
  result.indices.assign(input.indices.begin(), input.indices.end());
  return result;
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

bool RegressionCoverageRequested() {
  if (!RegressionLogPath()) {
    return false;
  }
#ifdef _WIN32
  char* value{};
  std::size_t size{};
  if (_dupenv_s(&value, &size, "MERLIN_HYDRA2_REGRESSION_COVERAGE") != 0 ||
      value == nullptr) {
    return true;
  }
  const bool result = std::string_view(value) != "0";
  std::free(value);
  return result;
#else
  const char* value = std::getenv("MERLIN_HYDRA2_REGRESSION_COVERAGE");
  return value == nullptr || std::string_view(value) != "0";
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

#ifdef MERLIN_HYDRA2_ENABLE_METAL
std::optional<std::uint64_t> MetalCapacityBytesFromEnvironment(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  std::uint64_t capacity_mib{};
  const auto* begin = value;
  const auto* end = value + std::strlen(value);
  const auto [parsed_end, error] =
      std::from_chars(begin, end, capacity_mib);
  constexpr std::uint64_t bytes_per_mib = 1024ULL * 1024ULL;
  if (error != std::errc{} || parsed_end != end || capacity_mib == 0 ||
      capacity_mib > std::numeric_limits<std::uint64_t>::max() /
                         bytes_per_mib) {
    throw std::invalid_argument(
        std::string(name) + " must be a positive in-range integer");
  }
  return capacity_mib * bytes_per_mib;
}
#endif

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

std::string ReadTokenParameter(const HdMaterialNode2& node, const char* name) {
  const auto* value = FindParameter(node, name);
  if (value == nullptr) {
    return {};
  }
  if (value->IsHolding<TfToken>()) {
    return value->UncheckedGet<TfToken>().GetString();
  }
  if (value->IsHolding<std::string>()) {
    return value->UncheckedGet<std::string>();
  }
  return {};
}

merlin::AddressMode ReadAddressMode(const HdMaterialNode2& node,
    const char* name) {
  const auto token = ReadTokenParameter(node, name);
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
    const std::string& path, bool flip_vertically, std::string& diagnostic) {
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
    const auto target = flip_vertically
        ? (height - 1U - pixel / width) * width + pixel % width
        : pixel;
    for (std::size_t component = 0; component < 3U; ++component) {
      std::uint32_t sample{};
      if (!ReadPpmUnsigned(stream, sample) || sample > maximum) {
        diagnostic = "invalid PPM pixel data in " + path;
        return std::nullopt;
      }
      texture.pixels[target * 4U + component] = static_cast<std::uint8_t>(
          (static_cast<std::uint64_t>(sample) * 255U + maximum / 2U) /
          maximum);
    }
    texture.pixels[target * 4U + 3U] = 255U;
  }
  return texture;
}

std::optional<merlin::TextureDescriptor> LoadTexture(
    const HdMaterialNode2& node, HioImage::SourceColorSpace color_space,
    bool flip_vertically, std::string& diagnostic) {
  const auto* file = FindParameter(node, "file");
  if (file == nullptr || !file->IsHolding<SdfAssetPath>()) {
    diagnostic = "texture node has no file asset";
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
    auto texture = LoadPpmTexture(path, flip_vertically, diagnostic);
    if (texture && color_space == HioImage::SRGB) {
      texture->format = merlin::TextureFormat::Rgba8Srgb;
    }
    return texture;
  }
  const auto image = HioImage::OpenForReading(path, 0, 0, color_space, true);
  if (!image || image->GetWidth() <= 0 || image->GetHeight() <= 0) {
    diagnostic = "could not open texture image " + path;
    return std::nullopt;
  }
  const auto format = image->GetFormat();
  const auto type = HioGetHioType(format);
  const auto components = HioGetComponentCount(format);
  if ((type != HioTypeUnsignedByte && type != HioTypeUnsignedByteSRGB) ||
      components < 1 || components > 4) {
    diagnostic = "texture image is not 8-bit grayscale, RGB or RGBA: " + path;
    return std::nullopt;
  }
  const auto pixel_count =
      static_cast<std::uint64_t>(image->GetWidth()) * image->GetHeight();
  if (pixel_count > std::numeric_limits<std::size_t>::max() / 4U) {
    diagnostic = "texture image dimensions overflow the RGBA payload: " + path;
    return std::nullopt;
  }
  merlin::TextureDescriptor texture;
  texture.label = path;
  texture.width = static_cast<std::uint32_t>(image->GetWidth());
  texture.height = static_cast<std::uint32_t>(image->GetHeight());
  if (color_space == HioImage::SRGB ||
      (color_space == HioImage::Auto && image->IsColorSpaceSRGB())) {
    texture.format = merlin::TextureFormat::Rgba8Srgb;
  }
  texture.pixels.resize(static_cast<std::size_t>(pixel_count) * 4U);
  std::vector<std::uint8_t> source;
  if (components != 4) {
    source.resize(static_cast<std::size_t>(pixel_count) * components);
  }
  HioImage::StorageSpec storage;
  storage.width = image->GetWidth();
  storage.height = image->GetHeight();
  storage.depth = 1;
  storage.format = format;
  storage.flipped = flip_vertically;
  storage.data = components == 4 ? texture.pixels.data() : source.data();
  if (!image->Read(storage)) {
    diagnostic = "could not decode texture image " + path;
    return std::nullopt;
  }
  if (components != 4) {
    for (std::size_t pixel = 0; pixel < pixel_count; ++pixel) {
      const auto* input = source.data() + pixel * components;
      auto* output = texture.pixels.data() + pixel * 4U;
      output[0] = input[0];
      output[1] = components == 3 ? input[1] : input[0];
      output[2] = components == 3 ? input[2] : input[0];
      output[3] = components == 2 ? input[1] : 255U;
    }
  }
  return texture;
}

struct ParsedMaterial {
  merlin::MaterialDescriptor material;
  std::optional<merlin::TextureDescriptor> texture;
  merlin::SamplerDescriptor sampler;
  std::string diagnostic;
  // UsdPreviewSurface normal <- UsdUVTexture. A rejected or reinterpreted
  // normal input reports normal_diagnostic; the rest of the material stays.
  std::optional<merlin::TextureDescriptor> normal_texture;
  merlin::SamplerDescriptor normal_sampler;
  std::string normal_diagnostic;
  struct ImageInput {
    std::optional<merlin::TextureDescriptor> texture;
    merlin::SamplerDescriptor sampler;
  };
  ImageInput metallic;
  ImageInput roughness;
  ImageInput emissive;
  ImageInput opacity;
  bool flip_texture_rows{};
  std::vector<std::string> materialx_unsupported_inputs;
};

bool MaterialParametersEqual(const merlin::MaterialParameterBlock& lhs,
    const merlin::MaterialParameterBlock& rhs) {
  return lhs.base_color.x == rhs.base_color.x &&
         lhs.base_color.y == rhs.base_color.y &&
         lhs.base_color.z == rhs.base_color.z &&
         lhs.base_color.w == rhs.base_color.w &&
         lhs.metallic == rhs.metallic && lhs.roughness == rhs.roughness &&
         lhs.alpha_cutoff == rhs.alpha_cutoff &&
         lhs.normal_scale == rhs.normal_scale &&
         lhs.normal_flip_green == rhs.normal_flip_green &&
         lhs.emissive_radiance.x == rhs.emissive_radiance.x &&
         lhs.emissive_radiance.y == rhs.emissive_radiance.y &&
         lhs.emissive_radiance.z == rhs.emissive_radiance.z &&
         lhs.specular_color.x == rhs.specular_color.x &&
         lhs.specular_color.y == rhs.specular_color.y &&
         lhs.specular_color.z == rhs.specular_color.z &&
         lhs.specular_weight == rhs.specular_weight &&
         lhs.specular_ior == rhs.specular_ior;
}

bool TextureBindingsEqual(const std::optional<merlin::TextureBinding>& lhs,
    const std::optional<merlin::TextureBinding>& rhs) {
  if (lhs.has_value() != rhs.has_value()) {
    return false;
  }
  return !lhs || (lhs->texture == rhs->texture &&
                     lhs->sampler == rhs->sampler &&
                     lhs->texcoord_set == rhs->texcoord_set);
}

bool MaterialFeaturesEqual(const merlin::MaterialDescriptor& lhs,
    const merlin::MaterialDescriptor& rhs) {
  if (!TextureBindingsEqual(lhs.base_color_texture, rhs.base_color_texture) ||
      !TextureBindingsEqual(lhs.normal_texture, rhs.normal_texture) ||
      !TextureBindingsEqual(lhs.metallic_texture, rhs.metallic_texture) ||
      !TextureBindingsEqual(lhs.roughness_texture, rhs.roughness_texture) ||
      !TextureBindingsEqual(lhs.emissive_texture, rhs.emissive_texture) ||
      !TextureBindingsEqual(lhs.opacity_texture, rhs.opacity_texture)) {
    return false;
  }
  return lhs.alpha_mode == rhs.alpha_mode &&
         lhs.double_sided == rhs.double_sided && lhs.features == rhs.features;
}

bool TextureDescriptorsEqual(const merlin::TextureDescriptor& lhs,
    const merlin::TextureDescriptor& rhs) {
  return lhs.label == rhs.label && lhs.width == rhs.width &&
         lhs.height == rhs.height && lhs.format == rhs.format &&
         lhs.pixels == rhs.pixels;
}

using MaterialTextureKey = std::tuple<std::string, merlin::TextureFormat, bool>;
using MaterialSamplerKey = std::tuple<merlin::FilterMode, merlin::FilterMode,
    merlin::AddressMode, merlin::AddressMode>;

MaterialSamplerKey SamplerKey(const merlin::SamplerDescriptor& descriptor) {
  return {descriptor.min_filter, descriptor.mag_filter,
      descriptor.address_u, descriptor.address_v};
}

bool ReadVec4Parameter(const HdMaterialNode2& node, const char* name,
    merlin::Vec4& result) {
  const auto* value = FindParameter(node, name);
  if (value == nullptr) {
    return false;
  }
  if (value->IsHolding<GfVec4f>()) {
    const auto& v = value->UncheckedGet<GfVec4f>();
    result = {v[0], v[1], v[2], v[3]};
    return true;
  }
  if (value->IsHolding<GfVec4d>()) {
    const auto& v = value->UncheckedGet<GfVec4d>();
    result = {static_cast<float>(v[0]), static_cast<float>(v[1]),
        static_cast<float>(v[2]), static_cast<float>(v[3])};
    return true;
  }
  return false;
}

// UsdPreviewSurface.normal is a tangent-space normal produced by its
// UsdUVTexture's `texel * scale + bias`. The surface contract decodes
// 2 * texel - 1, optionally with a negated green channel and a scaled xy, so
// that is the encoding accepted here: z must be 2t - 1 and xy s * (2t - 1),
// with y possibly negated. A texture without scale/bias is decoded as 2t - 1
// and reported; any other encoding disables normal mapping.
void ParseNormalMap(const HdMaterialNetwork2& network,
    const HdMaterialNode2& surface, ParsedMaterial& result) {
  const auto connection = surface.inputConnections.find(TfToken("normal"));
  if (connection == surface.inputConnections.end() ||
      connection->second.empty()) {
    return;
  }
  const auto node =
      network.nodes.find(connection->second.front().upstreamNode);
  if (node == network.nodes.end() ||
      node->second.nodeTypeId != TfToken("UsdUVTexture")) {
    result.normal_diagnostic =
        "normal connection is not a UsdUVTexture; normal mapping is disabled";
    return;
  }
  merlin::Vec4 scale{1.0F, 1.0F, 1.0F, 1.0F};
  merlin::Vec4 bias{};
  const bool authored_scale = ReadVec4Parameter(node->second, "scale", scale);
  const bool authored_bias = ReadVec4Parameter(node->second, "bias", bias);
  const auto approx = [](float a, float b) { return std::abs(a - b) <= 1.0e-5F; };
  const bool identity = approx(scale.x, 1.0F) && approx(scale.y, 1.0F) &&
                        approx(scale.z, 1.0F) && approx(bias.x, 0.0F) &&
                        approx(bias.y, 0.0F) && approx(bias.z, 0.0F);
  float normal_scale = 1.0F;
  bool flip_green = false;
  if (identity) {
    result.normal_diagnostic =
        std::string(authored_scale || authored_bias ? "identity" : "no") +
        " normal texture scale/bias; decoding it as 2 * texel - 1";
  } else {
    normal_scale = 0.5F * scale.x;
    flip_green = scale.y < 0.0F;
    const float green = flip_green ? -normal_scale : normal_scale;
    if (!(normal_scale > 0.0F) || !std::isfinite(normal_scale) ||
        !approx(bias.x, -normal_scale) || !approx(scale.y, 2.0F * green) ||
        !approx(bias.y, -green) || !approx(scale.z, 2.0F) ||
        !approx(bias.z, -1.0F)) {
      result.normal_diagnostic =
          "normal texture scale/bias is not a 2 * texel - 1 tangent-space "
          "encoding; normal mapping is disabled";
      return;
    }
  }
  std::string load_diagnostic;
  auto texture = LoadTexture(node->second, HioImage::Raw, false, load_diagnostic);
  if (!texture) {
    result.normal_diagnostic =
        load_diagnostic + "; normal mapping is disabled";
    return;
  }
  result.normal_texture = std::move(texture);
  result.normal_sampler.address_u = ReadAddressMode(node->second, "wrapS");
  result.normal_sampler.address_v = ReadAddressMode(node->second, "wrapT");
  result.material.parameters.normal_scale = normal_scale;
  result.material.parameters.normal_flip_green = flip_green;
  result.material.features |= merlin::MaterialFeature::NormalTexture;
}

const std::vector<HdMaterialConnection2>& InputConnections(
    const HdMaterialNode2& node, const char* name) {
  static const std::vector<HdMaterialConnection2> empty;
  const auto found = node.inputConnections.find(TfToken(name));
  return found == node.inputConnections.end() ? empty : found->second;
}

const HdMaterialNode2* ConnectedNode(const HdMaterialNetwork2& network,
    const HdMaterialNode2& node, const char* input) {
  const auto& connections = InputConnections(node, input);
  if (connections.size() != 1) {
    return nullptr;
  }
  const auto found = network.nodes.find(connections.front().upstreamNode);
  return found == network.nodes.end() ? nullptr : &found->second;
}

template <typename T>
bool ReadMaterialXConstant(const HdMaterialNetwork2& network,
    const HdMaterialNode2& surface, const char* input, const char* node_type,
    T& result, bool (*read_value)(const VtValue&, T&)) {
  const auto& connections = InputConnections(surface, input);
  if (connections.empty()) {
    const auto* value = FindParameter(surface, input);
    return value == nullptr || read_value(*value, result);
  }

  const auto* node = ConnectedNode(network, surface, input);
  const auto& output = connections.front().upstreamOutputName;
  if (node == nullptr || node->nodeTypeId != TfToken(node_type) ||
      (!output.IsEmpty() && output != TfToken("out")) ||
      !InputConnections(*node, "value").empty()) {
    return false;
  }
  const auto space = ReadTokenParameter(*node, "colorSpace:value");
  if (!space.empty() && space != "lin_rec709" && space != "raw") {
    return false;
  }
  if (const auto* value = FindParameter(*node, "value")) {
    return read_value(*value, result);
  }
  // Both the float and color3 constant nodedefs default to zero.
  result = {};
  return true;
}

std::string MaterialXImageUvDiagnostic(const HdMaterialNetwork2& network,
    const HdMaterialNode2& image) {
  if (InputConnections(image, "texcoord").empty()) {
    return FindParameter(image, "texcoord") == nullptr
        ? ""
        : "constant MaterialX image texture coordinates are unsupported";
  }

  const auto* uv = ConnectedNode(network, image, "texcoord");
  if (uv != nullptr && uv->nodeTypeId == TfToken("ND_texcoord_vector2")) {
    const auto* index = FindParameter(*uv, "index");
    if (InputConnections(*uv, "index").empty() &&
        (index == nullptr ||
            (index->IsHolding<int>() && index->UncheckedGet<int>() == 0))) {
      return {};
    }
  } else if (uv != nullptr &&
      uv->nodeTypeId == TfToken("ND_geompropvalue_vector2")) {
    // Hydra Mesh maps primvars:st to the surface's UV0 stream.
    if (!InputConnections(*uv, "geomprop").empty() ||
        ReadTokenParameter(*uv, "geomprop") != "st") {
      return "MaterialX image geompropvalue requires the st primvar";
    }
    const auto* fallback = FindParameter(*uv, "default");
    if (!InputConnections(*uv, "default").empty() ||
        (fallback != nullptr && (!fallback->IsHolding<GfVec2f>() ||
            fallback->UncheckedGet<GfVec2f>() != GfVec2f(0.0F)))) {
      return "custom MaterialX geompropvalue defaults are unsupported";
    }
    return {};
  }
  return "MaterialX image requires texture coordinate set 0";
}

std::optional<merlin::TextureDescriptor> ReadMaterialXImage(
    const HdMaterialNetwork2& network, const HdMaterialNode2* image,
    const char* node_type, merlin::SamplerDescriptor& sampler,
    std::string& diagnostic) {
  const TfToken expected(node_type);
  const bool color = expected == TfToken("ND_image_color3");
  const bool scalar = expected == TfToken("ND_image_float");
  if (image == nullptr || image->nodeTypeId != expected) {
    diagnostic = "expected a direct MaterialX " + expected.GetString() + " connection";
    return std::nullopt;
  }
  if (auto uv_diagnostic = MaterialXImageUvDiagnostic(network, *image);
      !uv_diagnostic.empty()) {
    diagnostic = std::move(uv_diagnostic);
    return std::nullopt;
  }
  const auto space = ReadTokenParameter(*image, "colorSpace:file");
  if (color && !space.empty() && space != "srgb_texture" && space != "srgb" &&
      space != "lin_rec709" && space != "raw") {
    diagnostic = "unsupported MaterialX image color space " + space;
    return std::nullopt;
  }
  if (scalar && !space.empty() && space != "lin_rec709" && space != "raw") {
    diagnostic = "scalar MaterialX image color space must be linear";
    return std::nullopt;
  }
  for (const auto* name : {"uaddressmode", "vaddressmode"}) {
    const auto mode = ReadTokenParameter(*image, name);
    if (!InputConnections(*image, name).empty() ||
        (!mode.empty() && mode != "periodic" && mode != "clamp" &&
            mode != "mirror")) {
      diagnostic = "unsupported MaterialX image address mode";
      return std::nullopt;
    }
  }
  const auto filter = ReadTokenParameter(*image, "filtertype");
  if (!InputConnections(*image, "filtertype").empty() ||
      (!filter.empty() && filter != "closest" && filter != "linear")) {
    diagnostic = "unsupported MaterialX image filter";
    return std::nullopt;
  }
  sampler.address_u = ReadAddressMode(*image, "uaddressmode");
  sampler.address_v = ReadAddressMode(*image, "vaddressmode");
  if (filter == "closest") {
    sampler.min_filter = sampler.mag_filter = merlin::FilterMode::Nearest;
  }
  const auto color_space = color && (space == "srgb_texture" || space == "srgb")
      ? HioImage::SRGB
      : HioImage::Raw;
  // MaterialX UV0 has a lower-left origin. Load the bottom source row first
  // so it is sampled at v=0 on both backends without changing the tangent frame.
  return LoadTexture(*image, color_space, true, diagnostic);
}

void ParseMaterialXNormal(const HdMaterialNetwork2& network,
    const HdMaterialNode2& surface, ParsedMaterial& result) {
  if (InputConnections(surface, "normal").empty()) {
    return;
  }
  const auto* normal = ConnectedNode(network, surface, "normal");
  if (normal == nullptr ||
      (normal->nodeTypeId != TfToken("ND_normalmap_float") &&
          normal->nodeTypeId != TfToken("ND_normalmap_vector2"))) {
    result.normal_diagnostic = "expected a MaterialX normalmap connection";
    return;
  }
  for (const auto* input : {"normal", "tangent", "bitangent"}) {
    if (!InputConnections(*normal, input).empty() ||
        FindParameter(*normal, input) != nullptr) {
      result.normal_diagnostic = "custom MaterialX normalmap frame is unsupported";
      return;
    }
  }
  float scale = 1.0F;
  bool flip_green = false;
  bool valid_scale = InputConnections(*normal, "scale").empty();
  if (const auto* value = FindParameter(*normal, "scale")) {
    if (value->IsHolding<GfVec2f>()) {
      const auto& xy = value->UncheckedGet<GfVec2f>();
      scale = xy[0];
      flip_green = xy[1] < 0.0F;
      valid_scale = valid_scale && std::abs(xy[1]) == scale;
    } else {
      valid_scale = valid_scale && ReadScalar(*value, scale);
    }
  }
  if (!valid_scale || !std::isfinite(scale) || scale < 0.0F) {
    result.normal_diagnostic = "unsupported MaterialX normalmap scale";
    return;
  }
  result.normal_texture = ReadMaterialXImage(network,
      ConnectedNode(network, *normal, "in"), "ND_image_vector3", result.normal_sampler,
      result.normal_diagnostic);
  if (result.normal_texture) {
    result.material.parameters.normal_scale = scale;
    result.material.parameters.normal_flip_green = flip_green;
    result.material.features |= merlin::MaterialFeature::NormalTexture;
  }
}

void ParseMaterialXEmission(const HdMaterialNetwork2& network,
    const HdMaterialNode2& surface, ParsedMaterial& result) {
  const auto valid_component = [](float value) {
    return std::isfinite(value) && value >= 0.0F;
  };
  float weight = 0.0F;
  const bool valid_weight = ReadMaterialXConstant(network, surface, "emission",
      "ND_constant_float", weight, ReadScalar) && valid_component(weight);
  if (!valid_weight) {
    result.materialx_unsupported_inputs.emplace_back(
        "emission (expected a finite nonnegative constant)");
  }

  merlin::Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};
  ParsedMaterial::ImageInput image;
  bool valid_color = true;
  const auto* color_node = ConnectedNode(network, surface, "emission_color");
  if (color_node != nullptr && color_node->nodeTypeId == TfToken("ND_image_color3")) {
    std::string diagnostic;
    image.texture = ReadMaterialXImage(network, color_node, "ND_image_color3",
        image.sampler, diagnostic);
    valid_color = image.texture.has_value();
    if (!valid_color) {
      result.materialx_unsupported_inputs.emplace_back(
          "emission_color (" + diagnostic + ")");
    }
  } else {
    valid_color = ReadMaterialXConstant(network, surface, "emission_color",
        "ND_constant_color3", color, ReadColor) && valid_component(color.x) &&
        valid_component(color.y) && valid_component(color.z);
    if (!valid_color) {
      result.materialx_unsupported_inputs.emplace_back(
          "emission_color (expected a finite nonnegative scene-linear RGB constant "
          "or direct MaterialX ND_image_color3 connection)");
    }
  }
  if (!valid_weight || !valid_color) {
    return;
  }

  const merlin::Vec3 radiance{weight * color.x, weight * color.y, weight * color.z};
  if (!valid_component(radiance.x) || !valid_component(radiance.y) ||
      !valid_component(radiance.z)) {
    result.materialx_unsupported_inputs.emplace_back("emission (radiance is out of range)");
    return;
  }
  result.material.parameters.emissive_radiance = radiance;
  if (image.texture) {
    result.emissive = std::move(image);
    result.material.features |= merlin::MaterialFeature::EmissiveTexture;
  }
}

void ParseMaterialXSurface(const HdMaterialNetwork2& network,
    const HdMaterialNode2& surface, ParsedMaterial& result) {
  result.flip_texture_rows = true;
  auto& parameters = result.material.parameters;
  parameters.base_color = {0.8F, 0.8F, 0.8F, 1.0F};
  if (const auto* value = FindParameter(surface, "base_color")) {
    (void)ReadColor(*value, parameters.base_color);
  }
  float base = 1.0F;
  if (const auto* value = FindParameter(surface, "base")) {
    (void)ReadScalar(*value, base);
  }
  if (!InputConnections(surface, "base").empty()) {
    result.materialx_unsupported_inputs.emplace_back("base");
  }
  if (!InputConnections(surface, "base_color").empty()) {
    result.texture = ReadMaterialXImage(network,
        ConnectedNode(network, surface, "base_color"), "ND_image_color3", result.sampler,
        result.diagnostic);
    if (result.texture) {
      parameters.base_color = {1.0F, 1.0F, 1.0F, 1.0F};
      result.material.features |= merlin::MaterialFeature::BaseColorTexture;
    }
  }
  parameters.base_color.x *= base;
  parameters.base_color.y *= base;
  parameters.base_color.z *= base;
  merlin::Vec4 opacity{1.0F, 1.0F, 1.0F, 1.0F};
  if (const auto* value = FindParameter(surface, "opacity")) {
    (void)ReadColor(*value, opacity);
  }
  if (!InputConnections(surface, "opacity").empty()) {
    std::string diagnostic;
    result.opacity.texture = ReadMaterialXImage(network,
        ConnectedNode(network, surface, "opacity"), "ND_image_color3",
        result.opacity.sampler, diagnostic);
    if (result.opacity.texture) {
      result.material.alpha_mode = merlin::AlphaMode::Masked;
      result.material.features |= merlin::MaterialFeature::OpacityTexture;
    } else {
      result.materialx_unsupported_inputs.emplace_back("opacity (" + diagnostic + ")");
    }
  } else if (opacity.x != opacity.y || opacity.x != opacity.z) {
    result.materialx_unsupported_inputs.emplace_back("opacity");
  } else if (opacity.x < 1.0F) {
    parameters.base_color.w = opacity.x;
    result.material.alpha_mode = merlin::AlphaMode::Blended;
  }
  result.material.features |= merlin::MaterialFeature::ForwardSpecular;
  parameters.roughness = 0.2F;
  const auto parse_scalar = [&](const char* input, float& parameter,
                                ParsedMaterial::ImageInput& image,
                                merlin::MaterialFeature feature) {
    if (const auto* authored = FindParameter(surface, input)) {
      float value{};
      if (ReadScalar(*authored, value) && std::isfinite(value)) {
        parameter = std::clamp(value, 0.0F, 1.0F);
      } else {
        result.materialx_unsupported_inputs.emplace_back(input);
      }
    }
    if (!InputConnections(surface, input).empty()) {
      std::string diagnostic;
      image.texture = ReadMaterialXImage(network,
          ConnectedNode(network, surface, input), "ND_image_float",
          image.sampler, diagnostic);
      if (image.texture) {
        result.material.features |= feature;
      } else {
        result.materialx_unsupported_inputs.emplace_back(
            std::string(input) + " (" + diagnostic + ")");
      }
    }
  };
  parse_scalar("metalness", parameters.metallic, result.metallic,
      merlin::MaterialFeature::MetallicTexture);
  parse_scalar("specular_roughness", parameters.roughness, result.roughness,
      merlin::MaterialFeature::RoughnessTexture);
  ParseMaterialXEmission(network, surface, result);
  const auto parse_specular_constant = [&](const char* input, float& parameter,
                                           float minimum, float maximum) {
    float value = parameter;
    if (ReadMaterialXConstant(network, surface, input, "ND_constant_float",
            value, ReadScalar) &&
        value >= minimum && value <= maximum) {
      parameter = value;
    } else {
      result.materialx_unsupported_inputs.emplace_back(
          std::string(input) + " (expected a constant in range)");
    }
  };
  parse_specular_constant("specular", parameters.specular_weight, 0.0F, 1.0F);
  parse_specular_constant("specular_IOR", parameters.specular_ior, 1.0F,
      std::numeric_limits<float>::max());
  merlin::Vec4 specular_color{1.0F, 1.0F, 1.0F, 1.0F};
  const auto unit_component = [](float value) { return value >= 0.0F && value <= 1.0F; };
  if (ReadMaterialXConstant(network, surface, "specular_color",
          "ND_constant_color3", specular_color, ReadColor) &&
      unit_component(specular_color.x) && unit_component(specular_color.y) &&
      unit_component(specular_color.z)) {
    parameters.specular_color = {specular_color.x, specular_color.y, specular_color.z};
  } else {
    result.materialx_unsupported_inputs.emplace_back(
        "specular_color (expected a constant RGB in [0, 1])");
  }
  for (const auto& [input, default_value] :
      {std::pair{"specular_anisotropy", 0.0F}, {"specular_rotation", 0.0F},
          {"subsurface", 0.0F}, {"transmission", 0.0F}, {"coat", 0.0F},
          {"sheen", 0.0F}, {"thin_film_thickness", 0.0F}}) {
    float value = default_value;
    if (const auto* parameter = FindParameter(surface, input)) {
      (void)ReadScalar(*parameter, value);
    }
    if (!InputConnections(surface, input).empty() || value != default_value) {
      result.materialx_unsupported_inputs.emplace_back(input);
    }
  }
  if (network.terminals.contains(HdMaterialTerminalTokens->displacement)) {
    result.materialx_unsupported_inputs.emplace_back("displacement");
  }
  ParseMaterialXNormal(network, surface, result);
}

ParsedMaterial ParseMaterialResource(const SdfPath& path,
    const VtValue& resource) {
  ParsedMaterial result;
  result.material.label = path.GetString();
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
  const bool is_materialx = surface != nullptr &&
      surface->nodeTypeId == TfToken("ND_standard_surface_surfaceshader");
  if (surface == nullptr || (!is_materialx &&
      surface->nodeTypeId != TfToken("UsdPreviewSurface"))) {
    result.diagnostic =
        "unsupported surface network; using constant fallback material";
    return result;
  }

  result.material.features = static_cast<merlin::MaterialFeature>(
      static_cast<std::uint32_t>(result.material.features) &
      ~static_cast<std::uint32_t>(merlin::MaterialFeature::VertexColor));

  if (is_materialx) {
    ParseMaterialXSurface(network, *surface, result);
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

  ParseNormalMap(network, *surface, result);

  const auto color_connection =
      surface->inputConnections.find(TfToken("diffuseColor"));
  if (color_connection == surface->inputConnections.end() ||
      color_connection->second.empty()) {
    return result;
  }
  const auto texture_node =
      network.nodes.find(color_connection->second.front().upstreamNode);
  if (texture_node != network.nodes.end() &&
      texture_node->second.nodeTypeId.GetString().starts_with(
          "UsdPrimvarReader_")) {
    const auto* varname = FindParameter(texture_node->second, "varname");
    const bool reads_display_color =
        varname != nullptr &&
        ((varname->IsHolding<TfToken>() &&
             varname->UncheckedGet<TfToken>() == TfToken("displayColor")) ||
            (varname->IsHolding<std::string>() &&
                varname->UncheckedGet<std::string>() == "displayColor"));
    if (reads_display_color) {
      // USD resolves a connected input by ignoring its authored value, so any
      // diffuseColor constant captured above must not tint the vertex colors.
      result.material.parameters.base_color.x = 1.0F;
      result.material.parameters.base_color.y = 1.0F;
      result.material.parameters.base_color.z = 1.0F;
      result.material.features |= merlin::MaterialFeature::VertexColor;
      return result;
    }
  }
  if (texture_node == network.nodes.end() ||
      texture_node->second.nodeTypeId != TfToken("UsdUVTexture")) {
    result.diagnostic =
        "diffuseColor connection is unsupported; using constant color";
    return result;
  }
  const auto source_color_space =
      ReadTokenParameter(texture_node->second, "sourceColorSpace");
  const auto color_space = source_color_space == "raw" ? HioImage::Raw
      : source_color_space == "sRGB" ? HioImage::SRGB : HioImage::Auto;
  result.texture = LoadTexture(
      texture_node->second, color_space, false, result.diagnostic);
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

using MeshDisplayVertices = std::vector<merlin::render::MeshDisplayVertex>;

struct MeshDisplayGeometry {
  std::shared_ptr<const MeshDisplayVertices> wire;
  std::shared_ptr<const MeshDisplayVertices> points;
};

bool IsWireRepr(const TfToken& token) {
  return token == HdReprTokens->wire || token == HdReprTokens->refinedWire;
}

bool IsWireOnSurfaceRepr(const TfToken& token) {
  return token == HdReprTokens->wireOnSurf ||
         token == HdReprTokens->refinedWireOnSurf;
}

bool IsUsdviewHiddenSurfacePrepass() {
#ifdef MERLIN_HYDRA2_HAVE_OPENGL
  // usdview uses polygon offset around its Geom Only prepass, then sends
  // the same Hydra collection as ordinary Wireframe for the second pass.
  if (glIsEnabled(GL_POLYGON_OFFSET_FILL) == GL_TRUE) {
    GLfloat factor{}, units{};
    glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &factor);
    glGetFloatv(GL_POLYGON_OFFSET_UNITS, &units);
    return factor == 1.0F && units == 1.0F;
  }
#endif
  return false;
}

struct MeshState {
  merlin::MeshHandle mesh;
  std::vector<merlin::InstanceHandle> instances;
  merlin::MeshDescriptor mesh_descriptor;
  std::vector<merlin::InstanceDescriptor> instance_descriptors;
  MeshDisplayGeometry display;
  // Key into SceneBridge::materials_ for the currently bound material, or empty
  // when the mesh uses the shared fallback material. Holds one reference.
  std::string material_key;
  bool authored_visible{true};
};

struct GaussianState {
  merlin::GaussianHandle gaussian;
  merlin::GaussianDescriptor descriptor;
};

// Shared material tracked by authored path so the render-world material and its
// map entry are released once no mesh references it.
struct MaterialEntry {
  merlin::MaterialHandle handle;
  std::size_t references{};
  bool authored{};
  merlin::MaterialDescriptor descriptor;
  std::optional<MaterialTextureKey> base_color_image;
  std::optional<MaterialTextureKey> normal_image;
  std::optional<MaterialTextureKey> metallic_image;
  std::optional<MaterialTextureKey> roughness_image;
  std::optional<MaterialTextureKey> emissive_image;
  std::optional<MaterialTextureKey> opacity_image;
};

struct MaterialTextureEntry {
  merlin::TextureHandle handle;
  std::size_t references{};
};

struct MaterialSamplerEntry {
  merlin::SamplerHandle handle;
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
  explicit SceneBridge(
      std::shared_ptr<merlin::render::Backend> backend = {},
      std::shared_ptr<HdMerlinHgiVulkanBridge> hgi_vulkan_bridge = {},
      std::shared_ptr<HdMerlinHgiMetalBridge> hgi_metal_bridge = {})
      : renderer_(std::move(backend)),
        hgi_vulkan_bridge_(std::move(hgi_vulkan_bridge)),
        hgi_metal_bridge_(std::move(hgi_metal_bridge)) {
    merlin::MaterialDescriptor material;
    material.label = "Hydra fallback material";
    // Unbound Hydra meshes use displayColor as their authored viewport color.
    // Keep the fallback neutral so it does not tint the entire scene.
    material.parameters.base_color = {1.0F, 1.0F, 1.0F, 1.0F};
    fallback_material_ = world_.CreateMaterial(std::move(material));
    render_settings_descriptor_.label = "Hydra render pass";
    render_settings_descriptor_.color_aov = false;
    render_settings_descriptor_.depth_aov = false;
    render_settings_ =
        world_.CreateRenderSettings(render_settings_descriptor_);
  }

  void SetCameraFrontFaceWinding(merlin::FrontFaceWinding winding) {
    std::scoped_lock lock(mutex_);
    camera_front_face_ = winding;
  }

  void SetHgiProjectionYReflection(bool reflect) {
    std::scoped_lock lock(mutex_);
    if (reflect != reflect_hgi_projection_y_) {
      // Reflection reverses projected winding as well as the viewport Y axis.
      camera_front_face_ = camera_front_face_ == merlin::FrontFaceWinding::Clockwise
                               ? merlin::FrontFaceWinding::CounterClockwise
                               : merlin::FrontFaceWinding::Clockwise;
    }
    reflect_hgi_projection_y_ = reflect;
  }

  void SetOutputTransformSettings(merlin::render::OutputTransformSettings settings) {
    std::scoped_lock lock(mutex_);
    output_transform_ = settings;
  }

  merlin::render::OutputTransformSettings EffectiveOutputTransformSettings() const {
    std::scoped_lock lock(mutex_);
    return output_transform_;
  }

  void SetDebugView(merlin::render::DebugView view) {
    std::scoped_lock lock(mutex_);
    debug_view_ = view;
  }

  merlin::render::DebugView EffectiveDebugView() const {
    std::scoped_lock lock(mutex_);
    return debug_view_;
  }

  void SetGpuDrivenIndexedSettings(
      merlin::render::GpuDrivenIndexedSettings settings) {
    std::scoped_lock lock(mutex_);
    gpu_driven_indexed_ = settings;
  }

  // Applies settings when the backend accepts them. Without a backend the
  // request stays pending and is validated before the first frame, so the
  // returned rejection is only the immediate one.
  [[nodiscard]] std::optional<merlin::render::RendererSettingsValidationError>
  RequestGpuDrivenGaussianSettings(
      merlin::render::GpuDrivenGaussianSettings settings,
      const TfToken& source) {
    std::scoped_lock lock(mutex_);
    if (!renderer_) {
      pending_gpu_driven_gaussian_ = PendingGaussianSettings{settings, source};
      return std::nullopt;
    }
    pending_gpu_driven_gaussian_.reset();
    auto error = ValidateGpuDrivenGaussianSettingsLocked(settings);
    if (!error) {
      gpu_driven_gaussian_ = settings;
    }
    return error;
  }

  [[nodiscard]] merlin::render::GpuDrivenGaussianSettings
  EffectiveGpuDrivenGaussianSettings() const {
    std::scoped_lock lock(mutex_);
    return pending_gpu_driven_gaussian_
               ? pending_gpu_driven_gaussian_->settings
               : gpu_driven_gaussian_;
  }

  [[nodiscard]] HdMerlinViewportFrame GetLatestViewportFrame() const {
    std::scoped_lock lock(mutex_);
    auto result = latest_viewport_frame_;
    result.diagnostics = g_diagnostic_sink.ReadSince(diagnostic_cursor_);
    return result;
  }

  void SyncMesh(const SdfPath& path, merlin::MeshDescriptor mesh,
      const SdfPath& material_path,
      const std::vector<merlin::Mat4>& transforms, bool visible,
      const MeshDisplayGeometry& display,
      merlin::ChangeAspect mesh_aspects,
      merlin::ChangeAspect instance_aspects,
      std::optional<std::vector<merlin::ElementRange>> vertex_ranges =
          std::nullopt,
      std::optional<std::vector<merlin::ElementRange>> index_ranges =
          std::nullopt) {
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
      state.display = display;
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
    state.display = display;
    state.mesh_descriptor = std::move(mesh);
    if (mesh_aspects != merlin::ChangeAspect::None) {
      world_.UpdateMesh(state.mesh, state.mesh_descriptor, mesh_aspects,
          std::move(vertex_ranges), std::move(index_ranges));
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

  void SyncGaussian(
      const SdfPath& path, merlin::GaussianDescriptor descriptor,
      merlin::ChangeAspect aspects,
      std::optional<std::vector<merlin::ElementRange>> particle_ranges =
          std::nullopt) {
    ScopedAtomicTimer update_timer(g_hydra_telemetry.render_world_update_ns);
    std::scoped_lock lock(mutex_);
    const auto key = path.GetString();
    auto found = gaussians_.find(key);
    if (descriptor.positions.empty()) {
      if (found != gaussians_.end()) {
        world_.Remove(found->second.gaussian);
        gaussians_.erase(found);
      }
      return;
    }
    if (found == gaussians_.end()) {
      GaussianState state;
      state.descriptor = std::move(descriptor);
      state.gaussian = world_.CreateGaussian(state.descriptor);
      gaussians_.emplace(key, std::move(state));
      return;
    }
    found->second.descriptor = std::move(descriptor);
    if (aspects != merlin::ChangeAspect::None) {
      world_.UpdateGaussian(found->second.gaussian,
          found->second.descriptor, aspects,
          std::move(particle_ranges));
    }
  }

  void RemoveGaussian(const SdfPath& path) {
    std::scoped_lock lock(mutex_);
    const auto found = gaussians_.find(path.GetString());
    if (found == gaussians_.end()) {
      return;
    }
    world_.Remove(found->second.gaussian);
    gaussians_.erase(found);
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

  // Sampler state is immutable and shared by all material slots that use it.
  // Retire the previous sampler only after updating the material binding.
  merlin::SamplerHandle AcquireMaterialSamplerLocked(
      merlin::SamplerDescriptor descriptor,
      const std::optional<merlin::TextureBinding>& previous) {
    const auto key = SamplerKey(descriptor);
    if (previous && SamplerKey(world_.Get(previous->sampler)) == key) {
      return previous->sampler;
    }
    auto found = material_samplers_.find(key);
    if (found == material_samplers_.end()) {
      descriptor.label = "hydra:materialSampler";
      found = material_samplers_.emplace(key,
          MaterialSamplerEntry{world_.CreateSampler(std::move(descriptor)), 0})
                       .first;
    }
    ++found->second.references;
    return found->second.handle;
  }

  void ReleaseMaterialSamplerLocked(
      const std::optional<merlin::TextureBinding>& previous,
      const std::optional<merlin::TextureBinding>& current = std::nullopt) {
    if (!previous || (current && previous->sampler == current->sampler)) {
      return;
    }
    const auto found = material_samplers_.find(
        SamplerKey(world_.Get(previous->sampler)));
    if (--found->second.references == 0) {
      world_.Remove(found->second.handle);
      material_samplers_.erase(found);
    }
  }

  // Image bytes are shared by resolved asset, color encoding and row order.
  std::optional<merlin::TextureBinding> AcquireTextureSlotLocked(
      std::optional<merlin::TextureDescriptor> parsed_texture,
      merlin::SamplerDescriptor parsed_sampler, bool flipped,
      const std::optional<merlin::TextureBinding>& previous,
      const std::optional<MaterialTextureKey>& previous_key,
      std::optional<MaterialTextureKey>& current_key) {
    if (!parsed_texture) {
      return std::nullopt;
    }
    current_key.emplace(parsed_texture->label, parsed_texture->format, flipped);
    auto found = material_textures_.find(*current_key);
    if (found == material_textures_.end()) {
      found = material_textures_.emplace(*current_key, MaterialTextureEntry{
          world_.CreateTexture(std::move(*parsed_texture)), 0}).first;
    } else if (!TextureDescriptorsEqual(
                   world_.Get(found->second.handle), *parsed_texture)) {
      world_.UpdateTexture(found->second.handle, std::move(*parsed_texture));
    }
    if (previous_key != current_key) {
      ++found->second.references;
    }
    const auto sampler =
        AcquireMaterialSamplerLocked(std::move(parsed_sampler), previous);
    return merlin::TextureBinding{found->second.handle, sampler, 0};
  }

  void ReleaseMaterialTextureLocked(
      const std::optional<MaterialTextureKey>& previous,
      const std::optional<MaterialTextureKey>& current = std::nullopt) {
    if (!previous || previous == current) {
      return;
    }
    const auto found = material_textures_.find(*previous);
    if (--found->second.references == 0) {
      world_.Remove(found->second.handle);
      material_textures_.erase(found);
    }
  }

  void SyncMaterial(const SdfPath& path, ParsedMaterial parsed) {
    ScopedAtomicTimer update_timer(g_hydra_telemetry.render_world_update_ns);
    std::scoped_lock lock(mutex_);
    const auto key = path.GetString();
    auto found = materials_.find(key);
    const bool created = found == materials_.end();
    if (created) {
      found = materials_.emplace(key, MaterialEntry{}).first;
    }
    auto& entry = found->second;
    entry.authored = true;
    auto descriptor = std::move(parsed.material);
    std::optional<MaterialTextureKey> base_color_image;
    std::optional<MaterialTextureKey> normal_image;
    std::optional<MaterialTextureKey> metallic_image;
    std::optional<MaterialTextureKey> roughness_image;
    std::optional<MaterialTextureKey> emissive_image;
    std::optional<MaterialTextureKey> opacity_image;
    descriptor.base_color_texture = AcquireTextureSlotLocked(
        std::move(parsed.texture), std::move(parsed.sampler),
        parsed.flip_texture_rows, entry.descriptor.base_color_texture,
        entry.base_color_image, base_color_image);
    descriptor.normal_texture = AcquireTextureSlotLocked(
        std::move(parsed.normal_texture), std::move(parsed.normal_sampler),
        parsed.flip_texture_rows, entry.descriptor.normal_texture,
        entry.normal_image, normal_image);
    descriptor.metallic_texture = AcquireTextureSlotLocked(
        std::move(parsed.metallic.texture), std::move(parsed.metallic.sampler),
        parsed.flip_texture_rows, entry.descriptor.metallic_texture,
        entry.metallic_image, metallic_image);
    descriptor.roughness_texture = AcquireTextureSlotLocked(
        std::move(parsed.roughness.texture), std::move(parsed.roughness.sampler),
        parsed.flip_texture_rows, entry.descriptor.roughness_texture,
        entry.roughness_image, roughness_image);
    descriptor.emissive_texture = AcquireTextureSlotLocked(
        std::move(parsed.emissive.texture), std::move(parsed.emissive.sampler),
        parsed.flip_texture_rows, entry.descriptor.emissive_texture,
        entry.emissive_image, emissive_image);
    descriptor.opacity_texture = AcquireTextureSlotLocked(
        std::move(parsed.opacity.texture), std::move(parsed.opacity.sampler),
        parsed.flip_texture_rows, entry.descriptor.opacity_texture,
        entry.opacity_image, opacity_image);
    if (created) {
      entry.handle = world_.CreateMaterial(descriptor);
      entry.base_color_image = std::move(base_color_image);
      entry.normal_image = std::move(normal_image);
      entry.metallic_image = std::move(metallic_image);
      entry.roughness_image = std::move(roughness_image);
      entry.emissive_image = std::move(emissive_image);
      entry.opacity_image = std::move(opacity_image);
      entry.descriptor = std::move(descriptor);
      return;
    }
    merlin::ChangeAspect aspects = merlin::ChangeAspect::None;
    if (!MaterialParametersEqual(entry.descriptor.parameters,
            descriptor.parameters)) {
      aspects |= merlin::ChangeAspect::MaterialParameters;
    }
    if (!MaterialFeaturesEqual(entry.descriptor, descriptor)) {
      aspects |= merlin::ChangeAspect::MaterialFeatures;
    }
    if (aspects != merlin::ChangeAspect::None) {
      world_.UpdateMaterial(entry.handle, descriptor, aspects);
    }
    ReleaseMaterialSamplerLocked(
        entry.descriptor.base_color_texture, descriptor.base_color_texture);
    ReleaseMaterialSamplerLocked(
        entry.descriptor.normal_texture, descriptor.normal_texture);
    ReleaseMaterialTextureLocked(entry.base_color_image, base_color_image);
    ReleaseMaterialSamplerLocked(
        entry.descriptor.metallic_texture, descriptor.metallic_texture);
    ReleaseMaterialSamplerLocked(
        entry.descriptor.roughness_texture, descriptor.roughness_texture);
    ReleaseMaterialSamplerLocked(
        entry.descriptor.emissive_texture, descriptor.emissive_texture);
    ReleaseMaterialSamplerLocked(
        entry.descriptor.opacity_texture, descriptor.opacity_texture);
    ReleaseMaterialTextureLocked(entry.normal_image, normal_image);
    ReleaseMaterialTextureLocked(entry.metallic_image, metallic_image);
    ReleaseMaterialTextureLocked(entry.roughness_image, roughness_image);
    ReleaseMaterialTextureLocked(entry.emissive_image, emissive_image);
    ReleaseMaterialTextureLocked(entry.opacity_image, opacity_image);
    entry.base_color_image = std::move(base_color_image);
    entry.normal_image = std::move(normal_image);
    entry.metallic_image = std::move(metallic_image);
    entry.roughness_image = std::move(roughness_image);
    entry.emissive_image = std::move(emissive_image);
    entry.opacity_image = std::move(opacity_image);
    entry.descriptor = std::move(descriptor);
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

  void SyncDomeLight(const SdfPath& path, const std::filesystem::path& image,
      const GfMatrix4d& world_to_environment, const merlin::Vec3& radiance_scale) {
    std::scoped_lock lock(mutex_);
    auto found = dome_textures_.find(image);
    if (found == dome_textures_.end()) {
      merlin::TextureDescriptor texture;
      texture.label = image.filename().string();
      const auto diffuse = merlin::render::LoadDiffuseEnvironment(image, &texture.pixels);
      texture.width = diffuse.width;
      texture.height = diffuse.height;
      auto specular = merlin::render::BuildSpecularEnvironment(
          texture.width, texture.height, texture.pixels);
      specular.label = texture.label + " specular environment";
      found = dome_textures_.emplace(image,
          DomeTexture{world_.CreateTexture(std::move(texture)),
              world_.CreateTexture(std::move(specular)), diffuse.coefficients}).first;
    }
    dome_lights_[path] = {found->second, world_to_environment, radiance_scale};
  }

  void RemoveDomeLight(const SdfPath& path) {
    std::scoped_lock lock(mutex_);
    dome_lights_.erase(path);
  }

  void SetDomeBackgroundVisible(bool visible) {
    std::scoped_lock lock(mutex_);
    dome_background_visible_ = visible;
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

    const HdCamera* camera = state.GetCamera();
    const auto camera_key = camera ? camera->GetId().GetString() : "__merlinViewportCamera";
    const auto view_matrix = state.GetWorldToViewMatrix();
    auto projection_matrix = state.GetProjectionMatrix();
    if (reflect_hgi_projection_y_) {
      GfMatrix4d reflection(1.0);
      reflection[1][1] = -1.0;
      projection_matrix *= reflection;
    }
    const auto camera_update_start = CpuClock::now();
    const auto active_camera = SyncCameraLocked(camera_key,
        ToMerlinMatrix(view_matrix), ToMerlinMatrix(projection_matrix));
    render_world_update_ns += ElapsedNanoseconds(camera_update_start);
    extractor_.SetActiveCamera(active_camera);

    std::uint32_t width{};
    std::uint32_t height{};
    bool color_aov{};
    bool depth_aov{};
    merlin::Vec4 clear_color = merlin::kDefaultClearColor;
    for (const auto& binding : bindings) {
      color_aov = color_aov || binding.aovName == HdAovTokens->color;
      if (binding.aovName == HdAovTokens->color) {
        (void)ReadColor(binding.clearValue, clear_color);
      }
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
    if ((width == 0 || height == 0) && renderer_ &&
        renderer_->default_presentation_target()) {
      const auto& viewport = state.GetViewport();
      if (viewport[2] > 0.0F && viewport[3] > 0.0F) {
        width = static_cast<std::uint32_t>(viewport[2]);
        height = static_cast<std::uint32_t>(viewport[3]);
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
#ifdef MERLIN_HYDRA2_ENABLE_VULKAN
      const auto shader_dir =
          PluginDirectory() / merlin::vulkan::shader_abi::ArtifactDirectory();
      merlin::vulkan::BackendFactoryOptions factory_options;
      if (hgi_vulkan_bridge_) {
        if (const auto borrowed = hgi_vulkan_bridge_->BorrowedContext()) {
          factory_options.renderer.borrowed_context =
              merlin::vulkan::BorrowedVulkanContext{
                  borrowed->instance,
                  borrowed->physical_device,
                  borrowed->device,
                  borrowed->graphics_queue,
                  borrowed->graphics_queue_family,
                  borrowed->graphics_queue_index,
                  borrowed->timeline_semaphore_enabled,
                  borrowed->validation_enabled,
                  borrowed->debug_utils_enabled,
                  borrowed->draw_indirect_first_instance_enabled};
          factory_options.renderer.descriptor_backend =
              merlin::vulkan::DescriptorBackendRequest::Conventional;
          factory_options.renderer.enable_async_transfer = false;
        }
      }
      factory_options.shaders = {
          shader_dir / "triangle.vert.spv", shader_dir / "triangle.frag.spv",
          shader_dir / "triangle.bindless.vert.spv",
          shader_dir / "triangle.bindless.frag.spv",
          shader_dir / "environment.hdr", shader_dir / "gaussian.vert.spv",
          shader_dir / "gaussian.frag.spv"};
      merlin::vulkan::BackendFactory factory(std::move(factory_options));
      std::vector<merlin::render::BackendFactory*> factories{&factory};
      merlin::render::BackendCreateInfo create_info;
      create_info.backend = merlin::render::BackendRequest::Vulkan;
      create_info.enable_validation = validation_requested;
      renderer_ = std::shared_ptr<merlin::render::Backend>(
          merlin::render::CreateBackend(create_info, factories));
#elif defined(MERLIN_HYDRA2_ENABLE_METAL)
      merlin::metal::BackendOptions backend_options;
      if (const auto capacity =
              MetalCapacityBytesFromEnvironment("MERLIN_METAL_HEAP_MIB")) {
        backend_options.heap_capacity_bytes = *capacity;
      }
      if (const auto capacity =
              MetalCapacityBytesFromEnvironment("MERLIN_METAL_GAUSSIAN_RESIDENCY_MIB")) {
        backend_options.gaussian_residency_budget_bytes = *capacity;
      }
      if (const auto capacity =
              MetalCapacityBytesFromEnvironment("MERLIN_METAL_GAUSSIAN_SCRATCH_MIB")) {
        backend_options.gaussian_scratch_budget_bytes = *capacity;
      }
      merlin::metal::BackendFactory factory(backend_options);
      std::vector<merlin::render::BackendFactory*> factories{&factory};
      merlin::render::BackendCreateInfo create_info;
      create_info.backend = merlin::render::BackendRequest::Metal;
      create_info.enable_validation = validation_requested;
      renderer_ = std::shared_ptr<merlin::render::Backend>(
          merlin::render::CreateBackend(create_info, factories));
#endif
      if (validation_requested &&
          !renderer_->capabilities().validation_enabled) {
        throw std::runtime_error(
            "Hydra regression requested unavailable backend validation");
      }
    }
    if (pending_gpu_driven_gaussian_) {
      const auto pending = *std::exchange(pending_gpu_driven_gaussian_, {});
      if (const auto error =
              ValidateGpuDrivenGaussianSettingsLocked(pending.settings)) {
        ReportRenderSettingRejection(
            pending.source, *error, gpu_driven_gaussian_);
      } else {
        gpu_driven_gaussian_ = pending.settings;
      }
    }
    const auto snapshot = extractor_.snapshot();
    const auto snapshot_build_counters =
        changes.empty() ? merlin::extraction::SnapshotBuildCounters{}
                        : snapshot->build_counters;
    ReportTextureDisablementLocked(*snapshot);
    merlin::render::RenderRequest request;
    request.snapshot = snapshot;
    request.output_transform = output_transform_;
    request.debug_view = debug_view_;
    request.forward_lighting.explicit_lights = true;
    auto& display = request.mesh_display;
    using Mode = merlin::render::MeshDisplayMode;
    const auto& repr = collection.GetReprSelector();
    const bool hidden_prepass = color_aov && !state.GetLightingEnabled() &&
        IsUsdviewHiddenSurfacePrepass();
    const bool hidden_wire = std::exchange(hidden_surface_prepass_, hidden_prepass);
    if (repr.Contains(HdReprTokens->points)) {
      display.mode = Mode::Points;
      display.point_size = state.GetPointSize();
    } else if (repr.Contains(HdReprTokens->wire) ||
               repr.Contains(HdReprTokens->refinedWire)) {
      display.mode = hidden_wire ? Mode::HiddenSurfaceWireframe : Mode::Wireframe;
    } else if (repr.Contains(HdReprTokens->wireOnSurf) ||
               repr.Contains(HdReprTokens->refinedWireOnSurf)) {
      display.mode = Mode::WireframeOnSurface;
    } else if (!state.GetLightingEnabled()) {
      display.mode = Mode::Geometry;
    } else if (repr.Contains(HdReprTokens->hull)) {
      display.mode = Mode::Flat;
    }
    if (display.mode == Mode::Points || display.mode == Mode::Wireframe ||
        display.mode == Mode::WireframeOnSurface ||
        display.mode == Mode::HiddenSurfaceWireframe) {
      const auto& wire = state.GetWireframeColor();
      display.wire_color = {wire[0], wire[1], wire[2], wire[3]};
      if (wire[3] == 0.0F) {
        const float gray = display.mode == Mode::WireframeOnSurface ? 0.0F : 0.035F;
        display.wire_color = {gray, gray, gray, 1.0F};
      }
      for (const auto& [path, mesh] : meshes_) {
        const auto& vertices = display.mode == Mode::Points
                                   ? mesh.display.points : mesh.display.wire;
        if (!vertices) {
          continue;
        }
        for (std::size_t i = 0; i < mesh.instances.size(); ++i) {
          const auto& instance = mesh.instance_descriptors[i];
          if (instance.visible) {
            display.draws.push_back({vertices, instance.transform,
                static_cast<std::uint32_t>(mesh.mesh.value()),
                static_cast<std::uint32_t>(mesh.instances[i].value())});
          }
        }
      }
    }
    if (!dome_lights_.empty()) {
      const auto& dome = dome_lights_.begin()->second;
      auto view_to_world = view_matrix.GetInverse();
      view_to_world.SetTranslateOnly(GfVec3d(0.0));
      request.forward_lighting.dome = merlin::render::ForwardDomeLight{
          dome.texture.handle, ToMerlinMatrix(dome.world_to_environment),
          ToMerlinMatrix(projection_matrix.GetInverse() * view_to_world *
              dome.world_to_environment),
          dome.texture.diffuse, dome.radiance_scale,
          dome_background_visible_ && color_aov, dome.texture.specular};
    }
    request.gpu_driven_indexed = gpu_driven_indexed_;
    request.gpu_driven_gaussian = gpu_driven_gaussian_;
    request.width = width;
    request.height = height;
    request.clear_color = clear_color;
    request.products.clear();
    const auto presentation = renderer_->default_presentation_target();
    if (presentation) {
      request.presentation = *presentation;
      request.products.push_back({merlin::Aov::Color, false});
    }
    std::vector<HdMerlinRenderBuffer*> color_buffers;
    std::vector<HdMerlinRenderBuffer*> gpu_color_candidates;
    for (const auto& binding : bindings) {
      if (binding.aovName != HdAovTokens->color) {
        continue;
      }
      HdRenderBuffer* base = binding.renderBuffer;
      if (base == nullptr && !binding.renderBufferId.IsEmpty()) {
        base = dynamic_cast<HdRenderBuffer*>(render_index->GetBprim(
            HdPrimTypeTokens->renderBuffer, binding.renderBufferId));
      }
      auto* buffer = dynamic_cast<HdMerlinRenderBuffer*>(base);
      if (buffer == nullptr) {
        continue;
      }
      color_buffers.push_back(buffer);
      if (buffer->CanGpuCopyColor()) {
        gpu_color_candidates.push_back(buffer);
      }
    }
    HdMerlinRenderBuffer* gpu_color_buffer =
        HdMerlinCanUseExclusiveGpuColorCopy(color_buffers.size(),
            gpu_color_candidates.size())
            ? gpu_color_candidates.front()
            : nullptr;

    const bool regression_coverage = RegressionCoverageRequested();
    std::array<HdMerlinRenderBuffer*,
        static_cast<std::size_t>(merlin::Aov::MotionVector) + 1>
        gpu_aov_buffers{};
    gpu_aov_buffers[static_cast<std::size_t>(merlin::Aov::Color)] = gpu_color_buffer;
#if defined(MERLIN_HYDRA2_ENABLE_VULKAN) || defined(MERLIN_HYDRA2_ENABLE_METAL)
    for (const auto aov : {merlin::Aov::Depth, merlin::Aov::PrimId,
             merlin::Aov::InstanceId}) {
      std::size_t consumers{};
      HdMerlinRenderBuffer* candidate{};
      for (const auto& binding : bindings) {
        const bool matches = aov == merlin::Aov::Depth
                                 ? HdAovHasDepthSemantic(binding.aovName)
                                 : binding.aovName == (aov == merlin::Aov::PrimId
                                                              ? HdAovTokens->primId
                                                              : HdAovTokens->instanceId);
        if (!matches) {
          continue;
        }
        HdRenderBuffer* base = binding.renderBuffer;
        if (base == nullptr && !binding.renderBufferId.IsEmpty()) {
          base = dynamic_cast<HdRenderBuffer*>(render_index->GetBprim(
              HdPrimTypeTokens->renderBuffer, binding.renderBufferId));
        }
        if (base != nullptr) {
          ++consumers;
          auto* buffer = dynamic_cast<HdMerlinRenderBuffer*>(base);
          if (buffer && buffer->CanGpuCopyAov(aov)) {
            candidate = buffer;
          }
        }
      }
      if (consumers == 1) {
        gpu_aov_buffers[static_cast<std::size_t>(aov)] = candidate;
      }
    }
#endif

    const auto request_product = [&](merlin::Aov aov,
                                     bool cpu_readback = true) {
      const auto found = std::find_if(
          request.products.begin(), request.products.end(),
          [aov](const merlin::render::RenderProductRequest& product) {
            return product.aov == aov;
          });
      if (found == request.products.end()) {
        request.products.push_back({aov, cpu_readback});
      } else if (cpu_readback) {
        found->cpu_readback = true;
      }
    };
    for (const auto& binding : bindings) {
      if (binding.aovName == HdAovTokens->color) {
        HdRenderBuffer* buffer = binding.renderBuffer;
        if (buffer == nullptr && !binding.renderBufferId.IsEmpty()) {
          buffer = dynamic_cast<HdRenderBuffer*>(render_index->GetBprim(
              HdPrimTypeTokens->renderBuffer, binding.renderBufferId));
        }
        // A host may bind color only to supply its clear value while native
        // presentation remains the sole consumer. Do not turn that clear-only
        // binding into an implicit CPU readback.
        if (buffer != nullptr) {
          request_product(merlin::Aov::Color, gpu_color_buffer == nullptr);
        }
      } else if (HdAovHasDepthSemantic(binding.aovName)) {
        request_product(merlin::Aov::Depth,
            gpu_aov_buffers[static_cast<std::size_t>(merlin::Aov::Depth)] == nullptr);
      } else if (binding.aovName == HdAovTokens->primId) {
        request_product(merlin::Aov::PrimId,
            gpu_aov_buffers[static_cast<std::size_t>(merlin::Aov::PrimId)] == nullptr);
      } else if (binding.aovName == HdAovTokens->instanceId) {
        request_product(merlin::Aov::InstanceId,
            gpu_aov_buffers[static_cast<std::size_t>(merlin::Aov::InstanceId)] == nullptr);
      }
    }
    // Regression evidence uses depth coverage even when the host binds only a
    // display color product.
    if (regression_coverage) {
      request_product(merlin::Aov::Depth);
    }
    auto token = renderer_->Submit(request);
    std::vector<HdMerlinRenderBuffer*> gpu_copied_buffers;
    bool gpu_copy_failed{};
#if defined(MERLIN_HYDRA2_ENABLE_VULKAN) || defined(MERLIN_HYDRA2_ENABLE_METAL)
    for (std::size_t i = 0; i < gpu_aov_buffers.size(); ++i) {
      auto* buffer = gpu_aov_buffers[i];
      if (buffer == nullptr) {
        continue;
      }
      bool copied{};
#ifdef MERLIN_HYDRA2_ENABLE_VULKAN
      using Exporter = merlin::vulkan::AovImageExporter;
#else
      using Exporter = merlin::metal::AovImageExporter;
#endif
      if (auto* exporter = dynamic_cast<Exporter*>(renderer_.get())) {
        try {
          copied = buffer->CopyAov(
              exporter->AcquireAovImage(token, static_cast<merlin::Aov>(i)), renderer_);
        } catch (const merlin::render::RendererError&) {
          copied = false;
        }
      }
      if (copied) {
        gpu_copied_buffers.push_back(buffer);
      } else {
        gpu_copy_failed = true;
      }
    }
#endif
    auto result = renderer_->Resolve(token);
    if (gpu_copy_failed) {
      for (auto& product : request.products) {
        product.cpu_readback = true;
      }
      result = renderer_->Resolve(renderer_->Submit(request));
      gpu_copied_buffers.clear();
    }
    latest_viewport_frame_.timings = result.timings;
    latest_viewport_frame_.telemetry = result.telemetry;
    latest_viewport_frame_.material_diagnostics =
        result.material_diagnostics;
    latest_viewport_frame_.geometries = snapshot->geometries.size();
    latest_viewport_frame_.gaussians = snapshot->gaussians.size();
    latest_viewport_frame_.gaussian_particles = 0;
    latest_viewport_frame_.gaussian_visible_resources = 0;
    latest_viewport_frame_.gaussian_sh_degree_resources.fill(0);
    latest_viewport_frame_.gaussian_perspective_resources = 0;
    latest_viewport_frame_.gaussian_tangential_resources = 0;
    latest_viewport_frame_.gaussian_z_depth_resources = 0;
    latest_viewport_frame_.gaussian_camera_distance_resources = 0;
    for (const auto& gaussian : snapshot->gaussians) {
      if (gaussian.positions) {
        latest_viewport_frame_.gaussian_particles +=
            gaussian.positions->size();
      }
      if (gaussian.visible) {
        ++latest_viewport_frame_.gaussian_visible_resources;
      }
      const auto degree = std::min<std::size_t>(
          gaussian.spherical_harmonics_degree,
          latest_viewport_frame_.gaussian_sh_degree_resources.size() - 1U);
      ++latest_viewport_frame_.gaussian_sh_degree_resources[degree];
      if (gaussian.projection_mode ==
          merlin::GaussianProjectionMode::Tangential) {
        ++latest_viewport_frame_.gaussian_tangential_resources;
      } else {
        ++latest_viewport_frame_.gaussian_perspective_resources;
      }
      if (gaussian.sorting_mode ==
          merlin::GaussianSortingMode::CameraDistance) {
        ++latest_viewport_frame_.gaussian_camera_distance_resources;
      } else {
        ++latest_viewport_frame_.gaussian_z_depth_resources;
      }
    }
    latest_viewport_frame_.textures = snapshot->textures.size();
    latest_viewport_frame_.samplers = snapshot->samplers.size();
    latest_viewport_frame_.materials = snapshot->materials.size();
    latest_viewport_frame_.instances = snapshot->instances.size();
    latest_viewport_frame_.lights = snapshot->lights.size();
    latest_viewport_frame_.available = true;
    const auto renderer_statistics = renderer_->statistics();
    if (ValidationRequested() &&
        renderer_statistics.validation_messages != 0) {
      throw std::runtime_error("backend validation reported Hydra warnings");
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
      if (std::find(gpu_copied_buffers.begin(), gpu_copied_buffers.end(), buffer) !=
          gpu_copied_buffers.end()) {
        written = true;
      } else if (binding.aovName == HdAovTokens->color) {
        written = buffer->WriteColor(result.color.pixels,
            result.color.product.width, result.color.product.height);
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
        for (std::uint32_t y = 0; !result.depth.pixels.empty() &&
                                  y < result.depth.product.height;
            ++y) {
          for (std::uint32_t x = 0; x < result.depth.product.width; ++x) {
            const auto index =
                static_cast<std::size_t>(y) *
                    (result.depth.row_pitch_bytes / sizeof(float)) +
                x;
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
        const auto vertex_color_materials = static_cast<std::size_t>(
            std::count_if(snapshot->materials.begin(),
                snapshot->materials.end(), [](const auto& material) {
                  return merlin::HasMaterialFeature(
                      material.features,
                      merlin::MaterialFeature::VertexColor);
                }));
        const auto neutral_vertex_color_materials = static_cast<std::size_t>(
            std::count_if(snapshot->materials.begin(), snapshot->materials.end(),
                [](const auto& material) {
                  return merlin::HasMaterialFeature(
                             material.features,
                             merlin::MaterialFeature::VertexColor) &&
                         material.parameters.base_color.x == 1.0F &&
                         material.parameters.base_color.y == 1.0F &&
                         material.parameters.base_color.z == 1.0F;
                }));
        const auto texcoord_geometries = static_cast<std::size_t>(
            std::count_if(snapshot->geometries.begin(),
                snapshot->geometries.end(), [](const auto& geometry) {
                  return geometry.has_texcoords;
                }));
        const auto missing_texcoord_geometries =
            snapshot->geometries.size() - texcoord_geometries;
        const auto normal_map_materials = static_cast<std::size_t>(
            std::count_if(snapshot->materials.begin(),
                snapshot->materials.end(), [](const auto& material) {
                  return material.normal_texture.has_value();
                }));
        const auto count_images = [&](auto binding) {
          return std::count_if(snapshot->materials.begin(),
              snapshot->materials.end(), [&](const auto& material) {
                return (material.*binding).has_value();
              });
        };
        const auto metallic_map_materials = count_images(
            &merlin::extraction::MaterialRecord::metallic_texture);
        const auto roughness_map_materials = count_images(
            &merlin::extraction::MaterialRecord::roughness_texture);
        const auto emissive_map_materials = count_images(
            &merlin::extraction::MaterialRecord::emissive_texture);
        const auto opacity_map_materials = count_images(
            &merlin::extraction::MaterialRecord::opacity_texture);
        const auto normal_map_disabled_draws =
            merlin::render::CountForwardNormalMapDisabledDraws(*snapshot);
        const auto hgi_vulkan_telemetry =
            hgi_vulkan_bridge_
                ? hgi_vulkan_bridge_->telemetry()
                : HdMerlinHgiVulkanBridgeTelemetry{};
        const auto hgi_vulkan_status =
            hgi_vulkan_bridge_
                ? hgi_vulkan_bridge_->status()
                : HdMerlinHgiVulkanBridgeStatus{};
#ifdef MERLIN_HYDRA2_ENABLE_HGI_METAL_BRIDGE
        const auto hgi_metal_telemetry =
            hgi_metal_bridge_ ? hgi_metal_bridge_->telemetry()
                              : HdMerlinHgiMetalBridgeTelemetry{};
        const auto hgi_metal_status =
            hgi_metal_bridge_ ? hgi_metal_bridge_->status()
                              : HdMerlinHgiMetalBridgeStatus{};
#else
        const HdMerlinHgiMetalBridgeTelemetry hgi_metal_telemetry{};
        const HdMerlinHgiMetalBridgeStatus hgi_metal_status{};
#endif
        stream << "schema_version=4"
               << " phase=" << RegressionPhase()
               << " scene_revision=" << result.scene_revision
               << " completion_value=" << result.completion_value
               << " draw_count=" << snapshot->draws.size()
               << " gaussian_resources=" << snapshot->gaussians.size()
               << " gaussian_candidate_count="
               << result.telemetry.gaussian_candidate_count
               << " gaussian_visible_count="
               << result.telemetry.gaussian_visible_count
               << " gaussian_hidden_count="
               << result.telemetry.gaussian_hidden_count
               << " gaussian_opacity_culled_count="
               << result.telemetry.gaussian_opacity_culled_count
               << " gaussian_frustum_culled_count="
               << result.telemetry.gaussian_frustum_culled_count
               << " gaussian_invalid_culled_count="
               << result.telemetry.gaussian_invalid_culled_count
               << " gaussian_sorted_count="
               << result.telemetry.gaussian_sorted_count
               << " gaussian_sorting_policy_fallback_count="
               << result.telemetry.gaussian_sorting_policy_fallback_count
               << " gaussian_preparation_cache_hits="
               << result.telemetry.gaussian_preparation_cache_hits
               << " gaussian_preparation_cache_misses="
               << result.telemetry.gaussian_preparation_cache_misses
               << " gaussian_draw_count="
               << result.telemetry.gaussian_draw_count
               << " gaussian_upload_bytes="
               << result.telemetry.gaussian_upload_bytes
               << " gaussian_attribute_upload_bytes="
               << result.telemetry.gaussian_attribute_upload_bytes
               << " gaussian_attribute_copy_ranges="
               << result.telemetry.gaussian_attribute_copy_range_count
               << " gaussian_attribute_generations="
               << result.telemetry.gaussian_attribute_generation_count
               << " gaussian_preparation_ns="
               << result.timings.gaussian_preparation_ns
               << " gaussian_attribute_upload_ns="
               << result.timings.gaussian_attribute_upload_ns
               << " gaussian_prepared_upload_ns="
               << result.timings.gaussian_prepared_upload_ns
               << " gaussian_gpu_timestamps_available="
               << renderer_->capabilities().gaussian_gpu_stage_timestamps
               << " gaussian_gpu_preparation_ns="
               << result.timings.gaussian_gpu_preparation_ns
               << " gaussian_gpu_sort_ns="
               << result.timings.gaussian_gpu_sort_ns
               << " gaussian_gpu_tile_ns="
               << result.timings.gaussian_gpu_tile_ns
               << " gaussian_raster_ns="
               << result.timings.gaussian_raster_ns
               << " gaussian_gpu_mode="
               << merlin::render::GpuDrivenGaussianModeName(
                      request.gpu_driven_gaussian.mode)
               << " gaussian_raster_path="
               << merlin::render::GaussianRasterPathName(
                      request.gpu_driven_gaussian.raster)
               << " gaussian_adaptive_tile_bounds="
               << request.gpu_driven_gaussian.adaptive_tile_bounds
               << " gaussian_gpu_pre_sort_culled_count="
               << result.telemetry.gaussian_gpu_pre_sort_culled_count
               << " gaussian_gpu_sorted_count="
               << result.telemetry.gaussian_gpu_sorted_count
               << " gaussian_gpu_raster_instance_count="
               << result.telemetry.gaussian_gpu_raster_instance_count
               << " gaussian_gpu_tile_pair_capacity="
               << result.telemetry.gaussian_gpu_tile_pair_capacity
               << " gaussian_gpu_tile_requested_pair_count="
               << result.telemetry.gaussian_gpu_tile_requested_pair_count
               << " gaussian_gpu_tile_rejected_pair_count="
               << result.telemetry.gaussian_gpu_tile_rejected_pair_count
               << " gaussian_gpu_tile_culled_record_count="
               << result.telemetry.gaussian_gpu_tile_culled_record_count
               << " gaussian_gpu_tile_clamped_record_count="
               << result.telemetry.gaussian_gpu_tile_clamped_record_count
               << " gaussian_gpu_tile_raster_frame_count="
               << result.telemetry.gaussian_gpu_tile_raster_frame_count
               << " gaussian_gpu_tile_saturated_count="
               << result.telemetry.gaussian_gpu_tile_saturated_count
               << " gaussian_gpu_tile_early_terminated_count="
               << result.telemetry.gaussian_gpu_tile_early_terminated_count
               << " gaussian_float_color_frame_count="
               << result.telemetry.gaussian_float_color_frame_count
               << " gaussian_gpu_tile_raster_overflow_fallback_count="
               << result.telemetry
                      .gaussian_gpu_tile_raster_overflow_fallback_count
               << " gaussian_gpu_fallback_count="
               << result.telemetry.gaussian_gpu_fallback_count
               << " buffers_written=" << buffers_written
               << " width=" << result.depth.product.width
               << " height=" << result.depth.product.height
               << " covered_pixels=" << covered_pixels
               << " covered_x_sum=" << covered_x_sum
               << " covered_y_sum=" << covered_y_sum
               << " textured_materials=" << textured_materials
               << " vertex_color_materials=" << vertex_color_materials
               << " neutral_vertex_color_materials="
               << neutral_vertex_color_materials
               << " texcoord_geometries=" << texcoord_geometries
               << " missing_texcoord_geometries="
               << missing_texcoord_geometries
               << " normal_map_materials=" << normal_map_materials
               << " metallic_map_materials=" << metallic_map_materials
               << " roughness_map_materials=" << roughness_map_materials
               << " emissive_map_materials=" << emissive_map_materials
               << " opacity_map_materials=" << opacity_map_materials
               << " normal_map_disabled_draws=" << normal_map_disabled_draws
               << " material_fallbacks=" << snapshot->material_fallbacks.size()
               << " generated_material_draw_count="
               << result.telemetry.generated_material_draw_count
               << " generated_material_fallback_count="
               << result.telemetry.generated_material_fallback_count
               << " material_fallback_recorded_count="
               << result.telemetry.material_fallbacks.recorded_count
               << " material_effective_fallback="
               << merlin::MaterialFallbackName(
                      result.telemetry.material_fallbacks.effective_fallback)
               << " texture_cache_hits="
               << result.telemetry.texture_cache_hits
               << " texture_cache_misses="
               << result.telemetry.texture_cache_misses
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
               << " snapshot_visited_records="
               << snapshot_build_counters.visited_records
               << " snapshot_copied_records="
               << snapshot_build_counters.copied_records
               << " snapshot_rebuilt_draws="
               << snapshot_build_counters.rebuilt_draws
               << " snapshot_fully_rebuilt_tables="
               << snapshot_build_counters.fully_rebuilt_tables
               << " gpu_scene_update_ns=" << result.timings.upload_ns
               << " command_recording_ns="
               << result.timings.command_recording_ns
               << " queue_submission_ns="
               << result.timings.queue_submission_ns
               << " gpu_execution_ns="
               << result.timings.gpu_execution_ns
               << " completion_wait_ns="
               << result.timings.completion_wait_ns
               << " readback_ns=" << result.timings.readback_ns
               << " backend_total_ns="
               << result.timings.backend_total_ns
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
               << " triangulation_rebuild_count="
               << frame_telemetry.triangulation_rebuild_count
               << " packed_mesh_rebuild_count="
               << frame_telemetry.packed_mesh_rebuild_count
               << " changed_vertex_count="
               << frame_telemetry.changed_vertex_count
               << " diagnostic_count="
               << frame_telemetry.diagnostic_count
               << " coarse_primvar_invalidation_count="
               << frame_telemetry.coarse_primvar_invalidation_count
               << " subdivision_plan_count=" << frame_telemetry.subdivision_plan_count
               << " subdivision_eval_count=" << frame_telemetry.subdivision_eval_count
               << " subdivision_eval_ns=" << frame_telemetry.subdivision_eval_ns
               // Resolve, Map, and CPU-to-Hgi upload happen after this render
               // pass returns. Their current-frame scopes are therefore
               // sourced exclusively from the enclosing OpenUSD host trace.
               << " render_buffer_resolve_ns=0"
               << " render_buffer_resolve_ns_available=0"
               << " render_buffer_map_ns=0"
               << " render_buffer_map_ns_available=0"
               << " host_upload_bytes=0 host_upload_bytes_available=0"
               << " host_upload_ns=0 host_upload_ns_available=0"
               << " gpu_copy_ns=0 gpu_copy_ns_available=0"
               << " host_composite_ns=0 host_composite_ns_available=0"
               << " presentation_ns=0 presentation_ns_available=0"
               << " requested_aov_count="
               << result.telemetry.requested_aov_count
               << " rendered_aov_count="
               << result.telemetry.rendered_aov_count
               << " cpu_readback_aov_count="
               << result.telemetry.cpu_readback_aov_count
               << " requested_aov_mask="
               << result.telemetry.requested_aov_mask
               << " rendered_aov_mask="
               << result.telemetry.rendered_aov_mask
               << " cpu_readback_aov_mask="
               << result.telemetry.cpu_readback_aov_mask
               << " aov_image_export_count="
               << result.telemetry.aov_image_export_count
               << " hgi_gpu_copy_count="
               << hgi_vulkan_telemetry.gpu_copy_count
               << " hgi_gpu_copy_completion_count="
               << hgi_vulkan_telemetry.gpu_copy_completion_count
               << " hgi_gpu_copy_pending_count="
               << hgi_vulkan_telemetry.gpu_copy_pending_count
               << " hgi_cpu_download_count=" << hgi_vulkan_telemetry.cpu_download_count
               << " hgi_cpu_download_bytes=" << hgi_vulkan_telemetry.cpu_download_bytes
               << " hgi_cpu_download_ns=" << hgi_vulkan_telemetry.cpu_download_ns
               << " hgi_transfer_mode="
               << HdMerlinHgiVulkanTransferModeName(
                      hgi_vulkan_status.selected_mode)
               << " hgi_direct_share_rejection="
               << HdMerlinHgiVulkanDirectShareRejectionName(
                      hgi_vulkan_status.direct_share_rejection)
               << " hgi_direct_share_evaluation_count="
               << hgi_vulkan_telemetry.direct_share_evaluation_count
               << " hgi_direct_share_rejection_count="
               << hgi_vulkan_telemetry.direct_share_rejection_count
               << " hgi_coarse_wait_count="
               << hgi_vulkan_telemetry.coarse_wait_count
#ifdef MERLIN_HYDRA2_ENABLE_HGI_METAL_BRIDGE
               << " hgi_metal_cpu_download_count=" << hgi_metal_telemetry.cpu_download_count
               << " hgi_metal_cpu_download_bytes=" << hgi_metal_telemetry.cpu_download_bytes
               << " hgi_metal_cpu_download_ns=" << hgi_metal_telemetry.cpu_download_ns
               << " hgi_metal_gpu_copy_count="
               << hgi_metal_telemetry.gpu_copy_count
               << " hgi_metal_gpu_copy_completion_count="
               << hgi_metal_telemetry.gpu_copy_completion_count
               << " hgi_metal_gpu_copy_pending_count="
               << hgi_metal_telemetry.gpu_copy_pending_count
               << " hgi_metal_transfer_mode="
               << HdMerlinHgiMetalTransferModeName(
                      hgi_metal_status.selected_mode)
               << " hgi_metal_direct_share_rejection="
               << HdMerlinHgiMetalDirectShareRejectionName(
                      hgi_metal_status.direct_share_rejection)
               << " hgi_metal_direct_share_evaluation_count="
               << hgi_metal_telemetry.direct_share_evaluation_count
               << " hgi_metal_direct_share_rejection_count="
               << hgi_metal_telemetry.direct_share_rejection_count
               << " hgi_metal_coarse_wait_count="
               << hgi_metal_telemetry.coarse_wait_count
#endif
               << " upload_bytes=" << result.telemetry.upload_bytes
               << " readback_bytes=" << result.telemetry.readback_bytes
               << " allocation_count=" << result.telemetry.allocation_count
               << " visible_primitive_count="
               << result.telemetry.visible_primitive_count
               << " wait_count=" << result.telemetry.wait_count
               << " map_count=" << result.telemetry.map_count
               << " resolve_count=" << result.telemetry.resolve_count
               << " descriptor_pool_creation_count="
               << result.telemetry.descriptor_pool_creation_count
               << " descriptor_allocation_count="
               << result.telemetry.descriptor_allocation_count
               << " descriptor_update_count="
               << result.telemetry.descriptor_update_count
               << " bindless_sampled_image_descriptor_update_count="
               << result.telemetry
                      .bindless_sampled_image_descriptor_update_count
               << " bindless_sampler_descriptor_update_count="
               << result.telemetry.bindless_sampler_descriptor_update_count
               << " geometry_reconcile_count="
               << result.telemetry.geometry_reconcile_count
               << " texture_reconcile_count="
               << result.telemetry.texture_reconcile_count
               << " sampler_reconcile_count="
               << result.telemetry.sampler_reconcile_count
               << " pipeline_creation_count="
               << result.telemetry.pipeline_creation_count
               << " shader_module_cache_misses="
               << result.telemetry.shader_module_cache_misses
               << " geometry_cache_misses="
               << result.telemetry.geometry_cache_misses
               << " buffer_allocation_bytes="
               << result.telemetry.buffer_allocation_bytes
               << " image_allocation_bytes="
               << result.telemetry.image_allocation_bytes
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
    auto& entry = found->second;
    world_.Remove(entry.handle);
    ReleaseMaterialTextureLocked(entry.base_color_image);
    ReleaseMaterialTextureLocked(entry.normal_image);
    ReleaseMaterialSamplerLocked(entry.descriptor.base_color_texture);
    ReleaseMaterialSamplerLocked(entry.descriptor.normal_texture);
    ReleaseMaterialTextureLocked(entry.metallic_image);
    ReleaseMaterialTextureLocked(entry.roughness_image);
    ReleaseMaterialTextureLocked(entry.emissive_image);
    ReleaseMaterialTextureLocked(entry.opacity_image);
    ReleaseMaterialSamplerLocked(entry.descriptor.metallic_texture);
    ReleaseMaterialSamplerLocked(entry.descriptor.roughness_texture);
    ReleaseMaterialSamplerLocked(entry.descriptor.emissive_texture);
    ReleaseMaterialSamplerLocked(entry.descriptor.opacity_texture);
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
      descriptor.front_face = camera_front_face_;
      const auto handle = world_.CreateCamera(std::move(descriptor));
      cameras_.emplace(key, handle);
      return handle;
    }
    const auto handle = found->second;
    const auto& old = world_.Get(handle);
    if (!MatricesEqual(old.view, view) ||
        !MatricesEqual(old.projection, projection) ||
        old.front_face != camera_front_face_) {
      auto descriptor = old;
      descriptor.view = view;
      descriptor.projection = projection;
      descriptor.front_face = camera_front_face_;
      world_.UpdateCamera(handle, std::move(descriptor),
          merlin::ChangeAspect::Camera);
    }
    return handle;
  }

  // Binds only the Gaussian policy to the backend's capabilities: every
  // other field of the contract keeps its default, and validation matches
  // the backend's creation so it cannot be the rejected field.
  [[nodiscard]] std::optional<merlin::render::RendererSettingsValidationError>
  ValidateGpuDrivenGaussianSettingsLocked(
      const merlin::render::GpuDrivenGaussianSettings& settings) const {
    merlin::render::RendererSettings contract;
    contract.validation = renderer_->capabilities().validation_enabled;
    contract.gpu_driven_gaussian = settings;
    return merlin::render::ValidateRendererSettings(
        contract, &renderer_->capabilities());
  }

  struct PendingGaussianSettings {
    merlin::render::GpuDrivenGaussianSettings settings;
    TfToken source;
  };

  struct DomeTexture {
    merlin::TextureHandle handle;
    merlin::TextureHandle specular;
    std::array<merlin::Vec4, 9> diffuse;
  };

  struct DomeLight {
    DomeTexture texture;
    GfMatrix4d world_to_environment;
    merlin::Vec3 radiance_scale;
  };

  // Report changed counts of draws that cannot sample their bound textures.
  void ReportTextureDisablementLocked(
      const merlin::extraction::FrameSnapshot& snapshot) {
    if (snapshot.revision == texture_report_revision_) {
      return;
    }
    texture_report_revision_ = snapshot.revision;
    const auto disabled =
        merlin::render::CountForwardNormalMapDisabledDraws(snapshot);
    if (disabled != 0U && disabled != reported_normal_map_disabled_draws_) {
      ReportHydraDiagnostic("surface.normal-map.disabled",
          SdfPath::AbsoluteRootPath(),
          std::to_string(disabled) +
              " draw(s) bind a normal map to geometry without texture "
              "coordinates; normal mapping is disabled for them",
          merlin::DiagnosticDisposition::Fallback, "use-shading-normal-base");
    }
    reported_normal_map_disabled_draws_ = disabled;
    const auto emissive_disabled = std::count_if(snapshot.draws.begin(),
        snapshot.draws.end(), [&](const auto& draw) {
          return !snapshot.geometries[draw.geometry_index].has_texcoords &&
              snapshot.materials[draw.material_index].emissive_texture.has_value();
        });
    if (emissive_disabled != 0 &&
        emissive_disabled != reported_emissive_map_disabled_draws_) {
      ReportHydraDiagnostic("surface.emissive-map.disabled",
          SdfPath::AbsoluteRootPath(),
          std::to_string(emissive_disabled) +
              " draw(s) bind an emissive map to geometry without UV0; "
              "emission is disabled for them",
          merlin::DiagnosticDisposition::Fallback, "disable-emission");
    }
    reported_emissive_map_disabled_draws_ = emissive_disabled;
    const auto opacity_disabled = std::count_if(snapshot.draws.begin(),
        snapshot.draws.end(), [&](const auto& draw) {
          return !snapshot.geometries[draw.geometry_index].has_texcoords &&
              snapshot.materials[draw.material_index].opacity_texture.has_value();
        });
    if (opacity_disabled != 0 &&
        opacity_disabled != reported_opacity_map_disabled_draws_) {
      ReportHydraDiagnostic("surface.opacity-map.disabled",
          SdfPath::AbsoluteRootPath(),
          std::to_string(opacity_disabled) +
              " draw(s) bind an opacity map to geometry without UV0; "
              "using their existing alpha",
          merlin::DiagnosticDisposition::Fallback, "use-existing-alpha");
    }
    reported_opacity_map_disabled_draws_ = opacity_disabled;
  }

  mutable std::mutex mutex_;
  std::uint64_t texture_report_revision_{};
  std::size_t reported_normal_map_disabled_draws_{};
  std::ptrdiff_t reported_emissive_map_disabled_draws_{};
  std::ptrdiff_t reported_opacity_map_disabled_draws_{};
  merlin::RenderWorld world_;
  merlin::FrontFaceWinding camera_front_face_{
      merlin::FrontFaceWinding::Clockwise};
  bool reflect_hgi_projection_y_{};
  merlin::render::OutputTransformSettings output_transform_;
  merlin::render::DebugView debug_view_{merlin::render::DebugView::None};
  merlin::render::GpuDrivenIndexedSettings gpu_driven_indexed_;
  merlin::render::GpuDrivenGaussianSettings gpu_driven_gaussian_;
  std::optional<PendingGaussianSettings> pending_gpu_driven_gaussian_;
  merlin::extraction::SceneExtractor extractor_;
  std::shared_ptr<merlin::render::Backend> renderer_;
  std::shared_ptr<HdMerlinHgiVulkanBridge> hgi_vulkan_bridge_;
  std::shared_ptr<HdMerlinHgiMetalBridge> hgi_metal_bridge_;
  merlin::MaterialHandle fallback_material_;
  merlin::RenderSettingsHandle render_settings_;
  merlin::RenderSettingsDescriptor render_settings_descriptor_;
  std::unordered_map<std::string, MeshState> meshes_;
  std::unordered_map<std::string, GaussianState> gaussians_;
  std::unordered_map<std::string, MaterialEntry> materials_;
  std::map<MaterialTextureKey, MaterialTextureEntry> material_textures_;
  std::map<MaterialSamplerKey, MaterialSamplerEntry> material_samplers_;
  std::unordered_map<std::string, merlin::CameraHandle> cameras_;
  std::unordered_map<std::string, merlin::LightHandle> lights_;
  std::map<std::filesystem::path, DomeTexture> dome_textures_;
  std::map<SdfPath, DomeLight> dome_lights_;
  bool dome_background_visible_{true};
  bool hidden_surface_prepass_{};
  HdMerlinViewportFrame latest_viewport_frame_;
  mutable std::uint64_t diagnostic_cursor_{};
};

class HdMerlinInstancer final : public HdInstancer {
public:
  HdMerlinInstancer(HdSceneDelegate* delegate, const SdfPath& id)
      : HdInstancer(delegate, id) {
  }

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
      ReportHydraDiagnostic(
          "hydra.instancer.parent-unavailable", GetId(),
          "parent instancer " + GetParentId().GetString() +
              " is unavailable",
          merlin::DiagnosticDisposition::Fallback,
          "local-instancer-transform");
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
      : HdMaterial(id), bridge_(std::move(bridge)) {
  }

  ~HdMerlinMaterial() override {
    bridge_->RemoveMaterial(GetId());
  }

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
        ReportHydraDiagnostic(
            "hydra.material.translation", GetId(), parsed.diagnostic,
            merlin::DiagnosticDisposition::Fallback, "constant-material");
      }
      if (!parsed.normal_diagnostic.empty()) {
        ReportHydraDiagnostic("hydra.material.normal-map", GetId(),
            parsed.normal_diagnostic, merlin::DiagnosticDisposition::Fallback,
            parsed.normal_texture ? "decode-2t-minus-1" : "disable-normal-map");
      }
      if (!parsed.materialx_unsupported_inputs.empty()) {
        std::string message = "MaterialX inputs outside the Forward subset:";
        for (const auto& input : parsed.materialx_unsupported_inputs) {
          message += " " + input;
        }
        ReportHydraDiagnostic("hydra.material.materialx-subset", GetId(), message,
            merlin::DiagnosticDisposition::Fallback, "render-supported-inputs");
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

  ~HdMerlinDistantLight() override {
    bridge_->RemoveLight(GetId());
  }

  void Sync(HdSceneDelegate* delegate, HdRenderParam* render_param,
      HdDirtyBits* dirty_bits) override {
    ScopedHydraSync sync_timer;
    g_hydra_telemetry.light_sync_count.fetch_add(1,
        std::memory_order_relaxed);
    (void)render_param;
    if (*dirty_bits == HdLight::Clean) {
      return;
    }
    merlin::ChangeAspect aspects = merlin::ChangeAspect::None;
    auto requested = descriptor_;
    if ((*dirty_bits & HdLight::DirtyTransform) != 0) {
      requested.transform = ToMerlinMatrix(delegate->GetTransform(GetId()));
      aspects |= merlin::ChangeAspect::Transform;
    }
    if ((*dirty_bits & HdLight::DirtyParams) != 0 || !applied_) {
      merlin::Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};
      const auto color_value =
          delegate->GetLightParamValue(GetId(), HdLightTokens->color);
      bool valid = color_value.IsEmpty() || ReadColor(color_value, color);
      merlin::hydra::detail::DistantLightEnergy energy;
      const auto read_scalar = [&](const TfToken& name, float& destination) {
        const auto value = delegate->GetLightParamValue(GetId(), name);
        return value.IsEmpty() || ReadScalar(value, destination);
      };
      valid &= read_scalar(HdLightTokens->intensity, energy.intensity);
      valid &= read_scalar(HdLightTokens->exposure, energy.exposure);
      valid &= read_scalar(HdLightTokens->angle, energy.angle_degrees);
      valid &= read_scalar(HdLightTokens->diffuse, energy.diffuse);
      const auto normalize =
          delegate->GetLightParamValue(GetId(), HdLightTokens->normalize);
      if (!normalize.IsEmpty()) {
        valid &= normalize.IsHolding<bool>();
        if (normalize.IsHolding<bool>()) {
          energy.normalize = normalize.UncheckedGet<bool>();
        }
      }
      const auto nonnegative_finite = [](float value) {
        return std::isfinite(value) && value >= 0.0F;
      };
      valid &= nonnegative_finite(color.x) && nonnegative_finite(color.y) &&
               nonnegative_finite(color.z);
      const auto response =
          merlin::hydra::detail::NormalizeDistantLightEnergy(energy);
      if (!valid || !response) {
        ReportHydraDiagnostic("hydra.light.invalid-parameters", GetId(),
            "Distant light requires finite nonnegative color/intensity/diffuse, "
            "finite exposure, angle in [0, 180], bool normalize and a "
            "representable diffuse response",
            merlin::DiagnosticDisposition::Rejected,
            applied_ ? "keep-previous-light" : "omit-invalid-light");
        *dirty_bits = HdLight::Clean;
        return;
      }
      requested.color = {color.x, color.y, color.z};
      requested.intensity = *response;
      aspects |= merlin::ChangeAspect::LightParameters;
    }
    descriptor_ = std::move(requested);
    bridge_->SyncLight(GetId(), descriptor_, aspects);
    applied_ = true;
    *dirty_bits = HdLight::Clean;
  }

  HdDirtyBits GetInitialDirtyBitsMask() const override {
    return HdLight::AllDirty;
  }

private:
  std::shared_ptr<SceneBridge> bridge_;
  merlin::LightDescriptor descriptor_;
  bool applied_{};
};

class HdMerlinDomeLight final : public HdLight {
public:
  HdMerlinDomeLight(const SdfPath& id, std::shared_ptr<SceneBridge> bridge)
      : HdLight(id), bridge_(std::move(bridge)) {}

  ~HdMerlinDomeLight() override { bridge_->RemoveDomeLight(GetId()); }

  void Sync(HdSceneDelegate* delegate, HdRenderParam*, HdDirtyBits* dirty_bits) override {
    if (*dirty_bits == HdLight::Clean || GetId().IsEmpty()) {
      return;
    }
    ScopedHydraSync sync_timer;
    g_hydra_telemetry.light_sync_count.fetch_add(1, std::memory_order_relaxed);
    *dirty_bits = HdLight::Clean;
    const auto read = [&](const TfToken& name) {
      return delegate->GetLightParamValue(GetId(), name);
    };
    const auto texture = read(HdLightTokens->textureFile);
    std::filesystem::path image;
    if (texture.IsHolding<SdfAssetPath>()) {
      const auto& asset = texture.UncheckedGet<SdfAssetPath>();
      image = asset.GetResolvedPath().empty() ? asset.GetAssetPath() : asset.GetResolvedPath();
    }
    if (image.empty()) {
      const auto params = read(HdLightTokens->params);
      if (params.IsHolding<GlfSimpleLight>()) {
        const auto hdx = PlugRegistry::GetInstance().GetPluginWithName("hdx");
        if (hdx) {
          image = std::filesystem::path(hdx->GetResourcePath()) /
                  "textures" / "StinsonBeach.hdr";
        }
      }
    } else if (image.filename() == "StinsonBeach.tex") {
      image.replace_extension(".hdr");
    }
    const auto invalid = [&](const std::string& message) {
      ReportHydraDiagnostic("hydra.dome.invalid-parameters", GetId(), message,
          merlin::DiagnosticDisposition::Rejected, "keep-previous-dome");
    };
    if (image.empty()) {
      invalid("A dome light requires a resolved Radiance HDR image");
      return;
    }
    float intensity = 1.0F, exposure = 0.0F;
    const auto intensity_value = read(HdLightTokens->intensity);
    const auto exposure_value = read(HdLightTokens->exposure);
    merlin::Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};
    const auto color_value = read(HdLightTokens->color);
    if ((!intensity_value.IsEmpty() && !ReadScalar(intensity_value, intensity)) ||
        (!exposure_value.IsEmpty() && !ReadScalar(exposure_value, exposure)) ||
        (!color_value.IsEmpty() && !ReadColor(color_value, color))) {
      invalid("Dome color, intensity and exposure must be numeric");
      return;
    }
    const auto energy = intensity * std::exp2(exposure);
    const merlin::Vec3 scale{color.x * energy, color.y * energy, color.z * energy};
    if (!std::isfinite(energy) || intensity < 0.0F || !std::isfinite(exposure) ||
        !std::isfinite(scale.x) || !std::isfinite(scale.y) || !std::isfinite(scale.z) ||
        color.x < 0.0F || color.y < 0.0F || color.z < 0.0F) {
      invalid("Dome light requires finite color, intensity and exposure");
      return;
    }
    auto rotation = delegate->GetTransform(GetId());
    rotation.SetTranslateOnly(GfVec3d(0.0));
    if (!rotation.Orthonormalize()) {
      invalid("Dome light requires a valid orientation");
      return;
    }
    // OpenUSD maps +X to the quarter-width meridian of a lat-long image.
    const auto meridian = GfMatrix4d().SetRotate(GfRotation(GfVec3d::YAxis(), -90.0));
    try {
      bridge_->SyncDomeLight(GetId(), image, rotation.GetInverse() * meridian, scale);
    } catch (const merlin::render::RendererError& error) {
      invalid(error.what());
    }
  }

  HdDirtyBits GetInitialDirtyBitsMask() const override { return HdLight::AllDirty; }

private:
  std::shared_ptr<SceneBridge> bridge_;
};

class HdMerlinMesh final : public HdMesh {
public:
  HdMerlinMesh(const SdfPath& id, std::shared_ptr<SceneBridge> bridge,
      HydraDirtyTracker* dirty_tracker)
      : HdMesh(id),
        bridge_(std::move(bridge)),
        dirty_tracker_(dirty_tracker) {
    descriptor_.label = id.GetString();
  }

  ~HdMerlinMesh() override {
    bridge_->RemoveMesh(GetId());
  }

  HdDirtyBits GetInitialDirtyBitsMask() const override {
    return HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyTopology |
           HdChangeTracker::DirtyPrimvar |
           HdChangeTracker::DirtySubdivTags | HdChangeTracker::DirtyDisplayStyle |
           HdChangeTracker::DirtyMaterialId |
           HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyVisibility |
           HdChangeTracker::DirtyRenderTag |
           HdChangeTracker::DirtyInstancer |
           HdChangeTracker::DirtyDoubleSided;
  }

  void Sync(HdSceneDelegate* delegate, HdRenderParam* render_param,
      HdDirtyBits* dirty_bits, const TfToken& repr_token) override {
    ScopedHydraSync sync_timer;
    g_hydra_telemetry.mesh_sync_count.fetch_add(1,
        std::memory_order_relaxed);
    (void)render_param;
    _UpdateInstancer(delegate, dirty_bits);
    const auto previous_descriptor = descriptor_;
    const auto dirty_locators =
        dirty_tracker_ == nullptr ? std::nullopt
                                  : dirty_tracker_->Consume(GetId());
    const bool topology_dirty = !geometry_initialized_ ||
        (dirty_locators
                ? (dirty_locators->Intersects(HdMeshSchema::GetTopologyLocator()) ||
                      dirty_locators->Intersects(HdMeshSchema::GetSubdivisionSchemeLocator()))
                : (*dirty_bits & HdChangeTracker::DirtyTopology) != 0);
    const bool subdivision_tags_dirty = dirty_locators
        ? dirty_locators->Intersects(HdMeshSchema::GetSubdivisionTagsLocator())
        : (*dirty_bits & HdChangeTracker::DirtySubdivTags) != 0;
    const bool display_style_dirty = dirty_locators
        ? dirty_locators->Intersects(HdLegacyDisplayStyleSchema::GetRefineLevelLocator())
        : (*dirty_bits & HdChangeTracker::DirtyDisplayStyle) != 0;
    const bool primvar_bit =
        (*dirty_bits & HdChangeTracker::DirtyPrimvar) != 0;
    auto& change_tracker =
        delegate->GetRenderIndex().GetChangeTracker();
    const auto current_points_sources =
        dirty_tracker_ == nullptr
            ? SceneIndexPrimvarSources{}
            : dirty_tracker_->GetPrimvarSources(GetId(), HdTokens->points);
    const bool points_locator_dirty =
        dirty_locators && dirty_locators->Intersects(
                              HdPrimvarsSchema::GetPointsLocator());
    const bool points_dirty =
        dirty_locators ? points_locator_dirty
                       : (*dirty_bits & HdChangeTracker::DirtyPoints) != 0;
    merlin::ChangeAspect mesh_aspects = merlin::ChangeAspect::None;
    merlin::ChangeAspect instance_aspects = merlin::ChangeAspect::None;
    bool points_changed{};
    if (points_dirty) {
      points_changed = FetchPoints(delegate, current_points_sources);
      if (points_changed) {
        mesh_aspects |= merlin::ChangeAspect::Points;
      }
    }
    if (topology_dirty) {
      mesh_aspects |= merlin::ChangeAspect::Topology;
      mesh_aspects |= merlin::ChangeAspect::VertexLayout;
      g_hydra_telemetry.topology_fetch_count.fetch_add(
          1, std::memory_order_relaxed);
      topology_ = GetMeshTopology(delegate);
    }
    if (topology_dirty || subdivision_tags_dirty) {
      subdivision_topology_.reset();
    }
    if (topology_dirty || display_style_dirty) {
      display_refine_level_ = GetDisplayStyle(delegate).refineLevel;
    }
    const bool primvars_dirty =
        dirty_locators
            ? HasNonPointPrimvarDirtiness(*dirty_locators)
            : (primvar_bit &&
                  (!topology_dirty || !primvar_cache_initialized_ ||
                      AnyCachedPrimvarDirty(change_tracker)));
    if (dirty_locators && dirty_locators->Contains(
                              HdPrimvarsSchema::GetDefaultLocator())) {
      g_hydra_telemetry.coarse_primvar_invalidation_count.fetch_add(
          1, std::memory_order_relaxed);
    }
    const bool relevant_primvars_changed =
        primvars_dirty && RefreshPrimvars(delegate, dirty_locators);
    if (relevant_primvars_changed) {
      mesh_aspects |= merlin::ChangeAspect::Primvars;
      subdivision_primvars_initialized_ = false;
    }
    const bool refined_repr = repr_token == HdReprTokens->refined ||
        repr_token == HdReprTokens->refinedWire ||
        repr_token == HdReprTokens->refinedWireOnSurf;
    const int level = refined_repr && !topology_.GetScheme().IsEmpty() &&
            topology_.GetScheme() != PxOsdOpenSubdivTokens->none
        ? display_refine_level_ : 0;
    const bool level_changed = level != active_refine_level_;
    const bool geometry_dirty = points_changed || topology_dirty ||
        level_changed || !geometry_initialized_ ||
        (level != 0 && (subdivision_tags_dirty || relevant_primvars_changed));
    if (geometry_dirty) {
      active_refine_level_ = level;
      geometry_initialized_ = true;
      if (level != 0) {
        RebuildSubdivision(delegate);
      } else {
        subdivision_.Reset();
        subdivision_status_ = merlin::SubdivisionStatus::Polygon;
      }
      if (subdivision_status_ == merlin::SubdivisionStatus::Polygon ||
          subdivision_status_ == merlin::SubdivisionStatus::Fallback) {
        RebuildTriangulation();
      }
      if (level_changed || (level != 0 && subdivision_tags_dirty)) {
        mesh_aspects |= merlin::ChangeAspect::Points |
            merlin::ChangeAspect::Primvars;
      }
    }
    if ((geometry_dirty || relevant_primvars_changed) &&
        (subdivision_status_ == merlin::SubdivisionStatus::Polygon ||
            subdivision_status_ == merlin::SubdivisionStatus::Fallback)) {
      RebuildPackedMesh();
    }
    if (previous_descriptor.indices.size() != descriptor_.indices.size()) {
      mesh_aspects |= merlin::ChangeAspect::Topology |
          merlin::ChangeAspect::VertexLayout;
    }
    const bool uses_display_geometry = IsWireRepr(repr_token) ||
        IsWireOnSurfaceRepr(repr_token) || repr_token == HdReprTokens->points;
    if ((uses_display_geometry && !display_.points) ||
        (display_.points && (geometry_dirty || relevant_primvars_changed))) {
      RebuildDisplayGeometry();
    }
    if ((*dirty_bits & HdChangeTracker::DirtyDoubleSided) != 0) {
      // Gprim doubleSided is surface state rather than a primvar; it travels
      // with the Primvars aspect, whose computed vertex ranges stay empty.
      const bool double_sided = delegate->GetDoubleSided(GetId());
      if (double_sided != descriptor_.double_sided) {
        descriptor_.double_sided = double_sided;
        mesh_aspects |= merlin::ChangeAspect::Primvars;
      }
    }
    auto vertex_ranges = ChangedVertexRanges(previous_descriptor, descriptor_);
    std::optional<std::vector<merlin::ElementRange>> known_vertex_ranges;
    if (merlin::HasAnyAspect(
            mesh_aspects, merlin::ChangeAspect::Points |
                              merlin::ChangeAspect::Primvars |
                              merlin::ChangeAspect::VertexLayout)) {
      known_vertex_ranges = vertex_ranges;
    }
    std::optional<std::vector<merlin::ElementRange>> known_index_ranges;
    if (merlin::HasAnyAspect(mesh_aspects,
            merlin::ChangeAspect::Topology)) {
      known_index_ranges =
          ChangedIndexRanges(previous_descriptor, descriptor_);
    }
    const auto changed_vertex_count = [&] {
      if (vertex_ranges.empty()) {
        return std::size_t{};
      }
      std::size_t result{};
      for (const auto& range : vertex_ranges) {
        result += range.count;
      }
      return result;
    }();
    if (mesh_aspects != merlin::ChangeAspect::None) {
      g_hydra_telemetry.changed_vertex_count.fetch_add(
          changed_vertex_count, std::memory_order_relaxed);
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
          ReportHydraDiagnostic(
              "hydra.mesh.instancer-unavailable", GetId(),
              "instancer " + GetInstancerId().GetString() +
                  " is unavailable",
              merlin::DiagnosticDisposition::Rejected,
              "reject-instanced-prim");
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
        display_, mesh_aspects, instance_aspects,
        std::move(known_vertex_ranges),
        std::move(known_index_ranges));
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
  struct DiscoveredPrimvar {
    TfToken name;
    TfToken role;
    HdInterpolation interpolation{HdInterpolationConstant};
    bool indexed{};
  };

  void ResetPackedMesh() {
    descriptor_.positions.clear();
    descriptor_.normals.clear();
    descriptor_.colors.clear();
    descriptor_.texcoords.clear();
    descriptor_.tangents.clear();
    descriptor_.indices.clear();
  }

  bool FetchPoints(HdSceneDelegate* delegate,
      SceneIndexPrimvarSources sources) {
    std::vector<GfVec3d> next_points;
    g_hydra_telemetry.points_fetch_count.fetch_add(
        1, std::memory_order_relaxed);
    const auto authored_points = GetPoints(delegate);
    if (authored_points.IsHolding<VtVec3fArray>()) {
      const auto& values = authored_points.UncheckedGet<VtVec3fArray>();
      next_points.reserve(values.size());
      for (const auto& value : values) {
        next_points.emplace_back(value);
      }
    } else if (authored_points.IsHolding<VtVec3dArray>()) {
      const auto& values = authored_points.UncheckedGet<VtVec3dArray>();
      next_points.assign(values.begin(), values.end());
    } else {
      ReportHydraDiagnostic(
          "hydra.mesh.points-type", GetId(),
          "points value is not a float3 or double3 array",
          merlin::DiagnosticDisposition::Rejected, "reject-prim");
    }
    const bool changed = next_points != points_;
    points_ = std::move(next_points);
    points_sources_ = std::move(sources);
    return changed;
  }

  bool RefreshPrimvars(
      HdSceneDelegate* delegate,
      const std::optional<HdDataSourceLocatorSet>& dirty_locators) {
    if (!primvar_cache_initialized_ ||
        PrimvarDescriptorsDirty(dirty_locators)) {
      constexpr std::array<HdInterpolation, 5> interpolations{
          HdInterpolationConstant, HdInterpolationUniform,
          HdInterpolationVertex, HdInterpolationVarying,
          HdInterpolationFaceVarying};
      primvar_descriptors_.clear();
      for (const auto interpolation : interpolations) {
        g_hydra_telemetry.primvar_descriptor_fetch_count.fetch_add(
            1, std::memory_order_relaxed);
        for (const auto& descriptor :
            GetPrimvarDescriptors(delegate, interpolation)) {
          primvar_descriptors_.push_back(
              {descriptor.name, descriptor.role, interpolation,
                  descriptor.indexed});
        }
      }
    }

    struct Request {
      PrimvarInput* cache{};
      TfToken name;
      bool texture_coordinate_fallback{};
    };
    std::array<Request, 5> requests{
        Request{&normals_, HdTokens->normals, false},
        Request{&colors_, HdTokens->displayColor, false},
        Request{&opacities_, HdTokens->displayOpacity, false},
        Request{&texcoords_, TfToken("st"), true},
        Request{&tangents_, TfToken("tangents"), false}};

    bool changed{};
    for (auto& request : requests) {
      const DiscoveredPrimvar* selected{};
      const auto exact = std::find_if(
          primvar_descriptors_.begin(), primvar_descriptors_.end(),
          [&](const auto& candidate) {
            return candidate.name == request.name;
          });
      if (exact != primvar_descriptors_.end()) {
        selected = &*exact;
      } else if (request.texture_coordinate_fallback) {
        const auto fallback = std::find_if(
            primvar_descriptors_.begin(), primvar_descriptors_.end(),
            [](const auto& candidate) {
              return candidate.role ==
                     HdPrimvarRoleTokens->textureCoordinate;
            });
        if (fallback != primvar_descriptors_.end()) {
          selected = &*fallback;
        }
      }

      auto& cache = *request.cache;
      if (selected == nullptr) {
        if (cache.present) {
          cache = {};
          changed = true;
        }
        continue;
      }
      const bool descriptor_changed =
          !cache.present || cache.source_name != selected->name ||
          cache.interpolation != selected->interpolation ||
          cache.indexed != selected->indexed;
      auto& change_tracker =
          delegate->GetRenderIndex().GetChangeTracker();
      const auto current_sources =
          dirty_tracker_ == nullptr
              ? SceneIndexPrimvarSources{}
              : dirty_tracker_->GetPrimvarSources(GetId(), selected->name);
      const bool value_dirty =
          !primvar_cache_initialized_ || descriptor_changed ||
          (dirty_locators
                  ? (dirty_locators->Intersects(
                         HdPrimvarsSchema::GetDefaultLocator().Append(
                             selected->name)) ||
                        // OpenUSD also emits primvars/name:indices for USD
                        // index edits, rather than primvars/name/indices.
                        dirty_locators->Intersects(
                            HdPrimvarsSchema::GetDefaultLocator().Append(
                                TfToken(selected->name.GetString() + ":indices"))))
                  : change_tracker.IsPrimvarDirty(GetId(), selected->name));
      if (!value_dirty) {
        continue;
      }
      PrimvarInput next;
      next.present = true;
      next.source_name = selected->name;
      next.interpolation = selected->interpolation;
      next.indexed = selected->indexed;
      next.sources = current_sources;
      g_hydra_telemetry.primvar_fetch_count.fetch_add(
          1, std::memory_order_relaxed);
      next.value = selected->indexed
                       ? GetIndexedPrimvar(delegate, selected->name,
                             &next.indices)
                       : GetPrimvar(delegate, selected->name);
      const bool input_changed =
          descriptor_changed || cache.value != next.value ||
          cache.indices != next.indices;
      cache = std::move(next);
      changed = changed || input_changed;
    }
    primvar_cache_initialized_ = true;
    return changed;
  }

  bool AnyCachedPrimvarDirty(HdChangeTracker& change_tracker) const {
    const std::array<const PrimvarInput*, 5> caches{
        &normals_, &colors_, &opacities_, &texcoords_, &tangents_};
    return std::any_of(caches.begin(), caches.end(), [&](const auto* cache) {
      return cache->present &&
             change_tracker.IsPrimvarDirty(GetId(), cache->source_name);
    });
  }

  static bool HasNonPointPrimvarDirtiness(
      const HdDataSourceLocatorSet& locators) {
    const auto& primvars = HdPrimvarsSchema::GetDefaultLocator();
    const auto& points = HdPrimvarsSchema::GetPointsLocator();
    bool found_primvar{};
    for (const auto& locator : locators) {
      if (!locator.Intersects(primvars)) {
        continue;
      }
      found_primvar = true;
      if (!locator.HasPrefix(points)) {
        return true;
      }
    }
    return found_primvar && locators.Contains(primvars);
  }

  static bool PrimvarDescriptorsDirty(
      const std::optional<HdDataSourceLocatorSet>& locators) {
    if (!locators) {
      return true;
    }
    const auto& primvars = HdPrimvarsSchema::GetDefaultLocator();
    if (locators->Contains(primvars)) {
      return true;
    }
    for (const auto& locator : *locators) {
      if (!locator.HasPrefix(primvars)) {
        continue;
      }
      // [primvars, name, member]. A primvar container or descriptor member can
      // add/remove a semantic or change interpolation, role, or indexing.
      const auto member_index = primvars.GetElementCount() + 1U;
      if (locator.GetElementCount() <= member_index) {
        return true;
      }
      const auto& member = locator.GetElement(member_index);
      if (member != HdPrimvarSchemaTokens->primvarValue &&
          member != HdPrimvarSchemaTokens->indexedPrimvarValue) {
        return true;
      }
    }
    return false;
  }

  std::vector<merlin::Vec3> MeshPositions() const {
    std::vector<merlin::Vec3> positions;
    positions.reserve(points_.size());
    for (const auto& point : points_) {
      positions.push_back({static_cast<float>(point[0]),
          static_cast<float>(point[1]), static_cast<float>(point[2])});
    }
    return positions;
  }

  void RebuildSubdivision(HdSceneDelegate* delegate) {
    if (!subdivision_topology_) {
      subdivision_topology_ = ToSubdivisionTopology(topology_, GetSubdivTags(delegate));
    }
    if (!subdivision_primvars_initialized_) {
      subdivision_texcoords_ = ToSubdivisionPrimvar<VtVec2fArray, VtVec2dArray>(
          texcoords_, [](const auto& value) {
            return merlin::Vec4{static_cast<float>(value[0]), static_cast<float>(value[1]), 0, 0};
          });
      subdivision_colors_ = ToSubdivisionPrimvar<VtVec3fArray, VtVec3dArray>(
          colors_, [](const auto& value) {
            return merlin::Vec4{static_cast<float>(value[0]), static_cast<float>(value[1]),
                static_cast<float>(value[2]), 0};
          });
      subdivision_opacities_ = ToSubdivisionPrimvar<VtFloatArray, VtDoubleArray>(
          opacities_, [](auto value) { return merlin::Vec4{static_cast<float>(value), 0, 0, 0}; });
      subdivision_primvars_initialized_ = true;
    }
    ScopedAtomicTimer refine_timer(g_hydra_telemetry.subdivision_eval_ns);
    auto result = subdivision_.Refine(*subdivision_topology_, MeshPositions(),
        {subdivision_texcoords_ ? &*subdivision_texcoords_ : nullptr,
            subdivision_colors_ ? &*subdivision_colors_ : nullptr,
            subdivision_opacities_ ? &*subdivision_opacities_ : nullptr},
        active_refine_level_, GetId().GetString(), &g_diagnostic_sink);
    subdivision_status_ = result.status;
    if (subdivision_status_ == merlin::SubdivisionStatus::Refined) {
      g_hydra_telemetry.subdivision_eval_count.fetch_add(1, std::memory_order_relaxed);
      if (result.rebuilt_plan) {
        g_hydra_telemetry.subdivision_plan_count.fetch_add(1, std::memory_order_relaxed);
      }
      result.mesh.double_sided = descriptor_.double_sided;
      descriptor_ = std::move(result.mesh);
    } else if (subdivision_status_ == merlin::SubdivisionStatus::Rejected) {
      ResetPackedMesh();
    }
  }

  void RebuildTriangulation() {
    g_hydra_telemetry.triangulation_rebuild_count.fetch_add(
        1, std::memory_order_relaxed);
    packed_corners_.clear();
    if (points_.empty()) {
      return;
    }

    const auto& counts = topology_.GetFaceVertexCounts();
    const auto& authored_indices = topology_.GetFaceVertexIndices();
    const auto& authored_holes = topology_.GetHoleIndices();
    const bool left_handed = topology_.GetOrientation() == HdTokens->leftHanded;
    std::unordered_set<int> holes;
    for (const auto hole : authored_holes) {
      if (hole < 0 || static_cast<std::size_t>(hole) >= counts.size() ||
          !holes.insert(hole).second) {
        ReportHydraDiagnostic(
            "hydra.mesh.hole-index", GetId(),
            "hole index " + std::to_string(hole) + " is invalid",
            merlin::DiagnosticDisposition::Rejected, "reject-prim");
        return;
      }
    }
    std::size_t offset{};
    for (std::uint32_t face_index = 0; face_index < counts.size();
        ++face_index) {
      const int count = counts[face_index];
      if (count < 3 || offset > authored_indices.size() ||
          static_cast<std::size_t>(count) >
              authored_indices.size() - offset) {
        ReportHydraDiagnostic(
            "hydra.mesh.face-count", GetId(),
            "face " + std::to_string(face_index) + " has vertex count " +
                std::to_string(count) + " inconsistent with topology",
            merlin::DiagnosticDisposition::Rejected, "reject-prim");
        packed_corners_.clear();
        return;
      }
      if (!holes.contains(face_index)) {
        std::vector<std::uint32_t> face_points;
        face_points.reserve(static_cast<std::size_t>(count));
        for (int corner = 0; corner < count; ++corner) {
          const int point = authored_indices[offset +
                                             static_cast<std::size_t>(corner)];
          if (point < 0 ||
              static_cast<std::size_t>(point) >= points_.size()) {
            ReportHydraDiagnostic(
                "hydra.mesh.point-index", GetId(),
                "face " + std::to_string(face_index) +
                    " references invalid point " + std::to_string(point),
                merlin::DiagnosticDisposition::Rejected, "reject-prim");
            packed_corners_.clear();
            return;
          }
          face_points.push_back(static_cast<std::uint32_t>(point));
        }
        std::string diagnostic;
        const auto triangles = TriangulateFace(face_points, diagnostic);
        if (triangles.empty()) {
          ReportHydraDiagnostic(
              "hydra.mesh.triangulation", GetId(),
              "face " + std::to_string(face_index) + ": " + diagnostic,
              merlin::DiagnosticDisposition::Rejected, "reject-prim");
          packed_corners_.clear();
          return;
        }
        for (auto triangle : triangles) {
          // Normalize USD's per-mesh orientation to the renderer's right-hand
          // convention before deriving normals or rasterizing. Keep authored
          // corner indices attached so face-varying primvars follow the swap;
          // explicitly authored normals already describe the outward surface.
          if (left_handed) {
            std::swap(triangle[1], triangle[2]);
          }
          for (const auto local_corner : triangle) {
            packed_corners_.push_back(
                {face_points[local_corner], face_index,
                    static_cast<std::uint32_t>(offset) + local_corner});
          }
        }
      }
      offset += static_cast<std::size_t>(count);
    }
    if (offset != authored_indices.size()) {
      packed_corners_.clear();
      ReportHydraDiagnostic(
          "hydra.mesh.trailing-indices", GetId(),
          "topology contains trailing face-vertex indices",
          merlin::DiagnosticDisposition::Rejected, "reject-prim");
    }
  }

  void RebuildPackedMesh() {
    g_hydra_telemetry.packed_mesh_rebuild_count.fetch_add(
        1, std::memory_order_relaxed);
    ResetPackedMesh();
    if (packed_corners_.empty()) {
      return;
    }
    const bool has_display_color = colors_.present || opacities_.present;
    if (packed_corners_.size() % 3U != 0U) {
      ReportHydraDiagnostic(
          "hydra.mesh.triangulation-cache", GetId(),
          "cached triangulation is not a multiple of three corners",
          merlin::DiagnosticDisposition::Rejected, "reject-prim");
      return;
    }
    // Shared Mesh attribute preparation owns the authored -> flat -> Ng
    // priority, renormalization and degenerate fallback. Missing normals
    // stay flat per triangle, matching the established Hydra baseline.
    const auto positions = MeshPositions();
    std::optional<merlin::NormalPrimvar> authored_normals;
    if (normals_.present) {
      authored_normals = ToNormalPrimvar(normals_);
      if (!authored_normals) {
        ReportHydraDiagnostic(
            "mesh.normals.malformed", GetId(),
            "normals primvar type, interpolation, or indices are malformed",
            merlin::DiagnosticDisposition::Rejected, "reject-prim");
        return;
      }
    }
    const auto normals = merlin::PrepareCornerNormals(positions,
        packed_corners_, authored_normals ? &*authored_normals : nullptr,
        merlin::NormalGeneration::Flat, GetId().GetString(),
        &g_diagnostic_sink);
    if (!normals.accepted()) {
      return;
    }
    std::vector<merlin::Vec2> corner_texcoords;
    if (texcoords_.present) {
      corner_texcoords.reserve(packed_corners_.size());
      for (const auto& corner : packed_corners_) {
        merlin::Vec2 texcoord;
        if (!ReadVec2(texcoords_, corner, texcoord)) {
          ReportHydraDiagnostic(
              "hydra.mesh.primvar.texcoord", GetId(),
              "texture-coordinate primvar type, interpolation, or indices are malformed",
              merlin::DiagnosticDisposition::Rejected, "reject-prim");
          return;
        }
        corner_texcoords.push_back(texcoord);
      }
    }
    // Shared preparation owns the authored -> generated tangent priority.
    // Generation welds corners by point, UV, normal and handedness, so a mesh
    // with flat generated normals keeps per-face frames.
    std::optional<merlin::TangentPrimvar> authored_tangents;
    if (tangents_.present) {
      authored_tangents = ToTangentPrimvar(tangents_);
      if (!authored_tangents) {
        ReportHydraDiagnostic(
            "mesh.tangents.malformed", GetId(),
            "tangents primvar type, interpolation, or indices are malformed",
            merlin::DiagnosticDisposition::Rejected, "reject-prim");
        return;
      }
    }
    std::optional<std::vector<merlin::Vec4>> corner_tangents;
    if (authored_tangents || !corner_texcoords.empty()) {
      auto tangents = merlin::PrepareCornerTangents(positions,
          packed_corners_, *normals.normals, corner_texcoords,
          authored_tangents ? &*authored_tangents : nullptr,
          GetId().GetString(), &g_diagnostic_sink);
      if (!tangents.accepted()) {
        return;
      }
      corner_tangents = std::move(tangents.tangents);
    }
    for (std::size_t triangle = 0; triangle < packed_corners_.size();
        triangle += 3U) {
      for (std::size_t local = 0; local < 3U; ++local) {
        const auto& corner = packed_corners_[triangle + local];
        descriptor_.positions.push_back(positions[corner.point]);
        descriptor_.normals.push_back((*normals.normals)[triangle + local]);
        merlin::Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};
        merlin::Vec3 rgb;
        if (colors_.present && !ReadVec3(colors_, corner, rgb)) {
          ReportHydraDiagnostic(
              "hydra.mesh.primvar.display-color", GetId(),
              "displayColor primvar type, interpolation, or indices are malformed",
              merlin::DiagnosticDisposition::Rejected, "reject-prim");
          ResetPackedMesh();
          return;
        }
        if (colors_.present) {
          color.x = rgb.x;
          color.y = rgb.y;
          color.z = rgb.z;
        }
        if (opacities_.present &&
            !ReadFloat(opacities_, corner, color.w)) {
          ReportHydraDiagnostic(
              "hydra.mesh.primvar.display-opacity", GetId(),
              "displayOpacity primvar type, interpolation, or indices are malformed",
              merlin::DiagnosticDisposition::Rejected, "reject-prim");
          ResetPackedMesh();
          return;
        }
        color.w = std::clamp(color.w, 0.0F, 1.0F);
        if (has_display_color) {
          descriptor_.colors.push_back(color);
        }
        if (!corner_texcoords.empty()) {
          descriptor_.texcoords.push_back(corner_texcoords[triangle + local]);
        }
        if (corner_tangents) {
          descriptor_.tangents.push_back((*corner_tangents)[triangle + local]);
        }
        descriptor_.indices.push_back(
            static_cast<std::uint32_t>(descriptor_.indices.size()));
      }
    }
  }

  void RebuildDisplayGeometry() {
    auto points = std::make_shared<MeshDisplayVertices>();
    auto wire = std::make_shared<MeshDisplayVertices>();
    display_ = {wire, points};
    if (descriptor_.indices.empty()) {
      return;
    }
    if (subdivision_status_ == merlin::SubdivisionStatus::Refined) {
      for (const auto& edge : subdivision_.WireEdges()) {
        wire->push_back({descriptor_.positions[edge[0]], {}});
        wire->push_back({descriptor_.positions[edge[1]], {}});
      }
      return;
    }

    const auto position = [&](std::uint32_t index) {
      const auto& point = points_[index];
      return merlin::Vec3{static_cast<float>(point[0]),
          static_cast<float>(point[1]), static_cast<float>(point[2])};
    };
    std::vector<bool> seen_points(points_.size());
    for (std::size_t i = 0; i < packed_corners_.size(); ++i) {
      const auto index = packed_corners_[i].point;
      if (!seen_points[index]) {
        seen_points[index] = true;
        points->push_back({position(index), descriptor_.colors.empty()
                ? merlin::Vec4{0.5F, 0.5F, 0.5F, 1.0F}
                : descriptor_.colors[i]});
      }
    }

    const auto& counts = topology_.GetFaceVertexCounts();
    const auto& indices = topology_.GetFaceVertexIndices();
    const auto& holes = topology_.GetHoleIndices();
    const std::unordered_set<int> hole_faces(holes.begin(), holes.end());
    std::unordered_set<std::uint64_t> edges;
    std::size_t offset{};
    for (std::size_t face = 0; face < counts.size(); ++face) {
      const auto count = static_cast<std::size_t>(counts[face]);
      if (!hole_faces.contains(static_cast<int>(face))) {
        for (std::size_t corner = 0; corner < count; ++corner) {
          const auto a = static_cast<std::uint32_t>(indices[offset + corner]);
          const auto b = static_cast<std::uint32_t>(
              indices[offset + (corner + 1) % count]);
          const auto key = (static_cast<std::uint64_t>(std::min(a, b)) << 32U) |
              std::max(a, b);
          if (a != b && edges.insert(key).second) {
            wire->push_back({position(a), {}});
            wire->push_back({position(b), {}});
          }
        }
      }
      offset += count;
    }
  }

  static std::vector<merlin::ElementRange> FullRange(std::size_t size) {
    if (size == 0U) {
      return {};
    }
    return {{0U, static_cast<std::uint32_t>(size)}};
  }

  static std::vector<merlin::ElementRange> ChangedIndexRanges(
      const merlin::MeshDescriptor& previous,
      const merlin::MeshDescriptor& current) {
    if (previous.indices.size() != current.indices.size()) {
      return FullRange(current.indices.size());
    }
    std::vector<merlin::ElementRange> ranges;
    std::size_t first = current.indices.size();
    for (std::size_t i = 0; i < current.indices.size(); ++i) {
      const bool changed = previous.indices[i] != current.indices[i];
      if (changed && first == current.indices.size()) {
        first = i;
      }
      if (!changed && first != current.indices.size()) {
        ranges.push_back({static_cast<std::uint32_t>(first),
            static_cast<std::uint32_t>(i - first)});
        first = current.indices.size();
      }
    }
    if (first != current.indices.size()) {
      ranges.push_back({static_cast<std::uint32_t>(first),
          static_cast<std::uint32_t>(current.indices.size() -
                                     first)});
    }
    return ranges;
  }

  static std::vector<merlin::ElementRange> ChangedVertexRanges(
      const merlin::MeshDescriptor& previous,
      const merlin::MeshDescriptor& current) {
    const auto count = current.positions.size();
    if (previous.positions.size() != count ||
        previous.normals.size() != current.normals.size() ||
        previous.colors.size() != current.colors.size() ||
        previous.texcoords.size() != current.texcoords.size() ||
        previous.tangents.size() != current.tangents.size()) {
      return FullRange(count);
    }
    const auto equal = [&](std::size_t index) {
      const auto& a_position = previous.positions[index];
      const auto& b_position = current.positions[index];
      if (a_position.x != b_position.x || a_position.y != b_position.y ||
          a_position.z != b_position.z) {
        return false;
      }
      if (!current.normals.empty()) {
        const auto& a = previous.normals[index];
        const auto& b = current.normals[index];
        if (a.x != b.x || a.y != b.y || a.z != b.z) {
          return false;
        }
      }
      if (!current.colors.empty()) {
        const auto& a = previous.colors[index];
        const auto& b = current.colors[index];
        if (a.x != b.x || a.y != b.y || a.z != b.z || a.w != b.w) {
          return false;
        }
      }
      if (!current.texcoords.empty()) {
        const auto& a = previous.texcoords[index];
        const auto& b = current.texcoords[index];
        if (a.x != b.x || a.y != b.y) {
          return false;
        }
      }
      if (!current.tangents.empty()) {
        const auto& a = previous.tangents[index];
        const auto& b = current.tangents[index];
        if (a.x != b.x || a.y != b.y || a.z != b.z || a.w != b.w) {
          return false;
        }
      }
      return true;
    };
    std::vector<merlin::ElementRange> ranges;
    std::size_t first = count;
    for (std::size_t i = 0; i < count; ++i) {
      const bool changed = !equal(i);
      if (changed && first == count) {
        first = i;
      }
      if (!changed && first != count) {
        ranges.push_back({static_cast<std::uint32_t>(first),
            static_cast<std::uint32_t>(i - first)});
        first = count;
      }
    }
    if (first != count) {
      ranges.push_back({static_cast<std::uint32_t>(first),
          static_cast<std::uint32_t>(count - first)});
    }
    return ranges;
  }

  std::shared_ptr<SceneBridge> bridge_;
  HydraDirtyTracker* dirty_tracker_{};
  merlin::MeshDescriptor descriptor_;
  HdMeshTopology topology_;
  std::vector<GfVec3d> points_;
  SceneIndexPrimvarSources points_sources_;
  std::vector<MeshCorner> packed_corners_;
  PrimvarInput normals_;
  PrimvarInput colors_;
  PrimvarInput opacities_;
  PrimvarInput texcoords_;
  PrimvarInput tangents_;
  merlin::MeshSubdivision subdivision_;
  std::optional<merlin::SubdivisionTopology> subdivision_topology_;
  std::optional<merlin::SubdivisionPrimvar> subdivision_texcoords_;
  std::optional<merlin::SubdivisionPrimvar> subdivision_colors_;
  std::optional<merlin::SubdivisionPrimvar> subdivision_opacities_;
  merlin::SubdivisionStatus subdivision_status_{merlin::SubdivisionStatus::Polygon};
  int display_refine_level_{};
  int active_refine_level_{};
  std::vector<DiscoveredPrimvar> primvar_descriptors_;
  SdfPath material_id_;
  MeshDisplayGeometry display_;
  GfMatrix4d hydra_transform_{1.0};
  std::vector<merlin::Mat4> transforms_{merlin::Mat4{}};
  bool visible_{true};
  bool primvar_cache_initialized_{};
  bool geometry_initialized_{};
  bool subdivision_primvars_initialized_{};
};

class HdMerlinGaussian final : public HdRprim {
public:
  HdMerlinGaussian(const SdfPath& id, std::shared_ptr<SceneBridge> bridge,
      HydraDirtyTracker* dirty_tracker)
      : HdRprim(id),
        bridge_(std::move(bridge)),
        dirty_tracker_(dirty_tracker) {
    source_.label = id.GetString();
    source_.source = source_.label;
  }

  ~HdMerlinGaussian() override {
    bridge_->RemoveGaussian(GetId());
  }

  HdDirtyBits GetInitialDirtyBitsMask() const override {
    return HdChangeTracker::DirtyPrimvar |
           HdChangeTracker::DirtyTransform |
           HdChangeTracker::DirtyVisibility |
           HdChangeTracker::DirtyRenderTag;
  }

  const TfTokenVector& GetBuiltinPrimvarNames() const override {
    static const TfTokenVector names{
        TfToken("positions"),
        TfToken("orientations"),
        TfToken("scales"),
        TfToken("opacities"),
        TfToken("radiance:sphericalHarmonicsCoefficients"),
        TfToken("radiance:sphericalHarmonicsDegree")};
    return names;
  }

  void Sync(HdSceneDelegate* delegate, HdRenderParam* render_param,
      HdDirtyBits* dirty_bits, const TfToken& repr_token) override {
    ScopedHydraSync sync_timer;
    (void)render_param;
    (void)repr_token;
    merlin::ChangeAspect aspects = merlin::ChangeAspect::None;
    std::optional<std::vector<merlin::ElementRange>> particle_ranges;
    const auto dirty_locators =
        dirty_tracker_ == nullptr ? std::nullopt
                                  : dirty_tracker_->Consume(GetId());
    const bool primvar_bit =
        (*dirty_bits & HdChangeTracker::DirtyPrimvar) != 0;
    auto& change_tracker = delegate->GetRenderIndex().GetChangeTracker();
    const auto is_primvar_dirty = [&](const TfToken& name) {
      if (!payload_initialized_) {
        return true;
      }
      if (dirty_locators) {
        return dirty_locators->Intersects(
            HdPrimvarsSchema::GetDefaultLocator().Append(name));
      }
      return primvar_bit && change_tracker.IsPrimvarDirty(GetId(), name);
    };
    const TfToken positions_token("positions");
    const TfToken orientations_token("orientations");
    const TfToken scales_token("scales");
    const TfToken opacities_token("opacities");
    const TfToken radiance_token(
        "radiance:sphericalHarmonicsCoefficients");
    const TfToken degree_token("radiance:sphericalHarmonicsDegree");
    const TfToken projection_token("projectionModeHint");
    const TfToken sorting_token("sortingModeHint");
    bool positions_dirty = is_primvar_dirty(positions_token);
    bool orientations_dirty = is_primvar_dirty(orientations_token);
    bool scales_dirty = is_primvar_dirty(scales_token);
    bool opacities_dirty = is_primvar_dirty(opacities_token);
    bool radiance_dirty = is_primvar_dirty(radiance_token);
    bool degree_dirty = is_primvar_dirty(degree_token);
    bool policy_dirty = !payload_initialized_;
    if (!dirty_locators && primvar_bit) {
      policy_dirty = policy_dirty ||
                     change_tracker.IsPrimvarDirty(GetId(), projection_token) ||
                     change_tracker.IsPrimvarDirty(GetId(), sorting_token);
      const bool recognized =
          positions_dirty || orientations_dirty || scales_dirty ||
          opacities_dirty || radiance_dirty || degree_dirty || policy_dirty;
      if (!recognized) {
        positions_dirty = true;
        orientations_dirty = true;
        scales_dirty = true;
        opacities_dirty = true;
        radiance_dirty = true;
        degree_dirty = true;
        policy_dirty = true;
        g_hydra_telemetry.coarse_primvar_invalidation_count.fetch_add(
            1, std::memory_order_relaxed);
      }
    }
    const bool source_dirty =
        positions_dirty || orientations_dirty || scales_dirty ||
        opacities_dirty || radiance_dirty || degree_dirty;
    if ((*dirty_bits & HdChangeTracker::DirtyTransform) != 0) {
      const auto transform = ToMerlinMatrix(delegate->GetTransform(GetId()));
      if (!payload_valid_ || transform.values != transform_.values) {
        aspects |= merlin::ChangeAspect::Transform;
      }
      transform_ = transform;
    }
    if ((*dirty_bits & (HdChangeTracker::DirtyVisibility |
                           HdChangeTracker::DirtyRenderTag)) != 0) {
      const bool visible = delegate->GetVisible(GetId());
      if (!payload_valid_ || visible != visible_) {
        aspects |= merlin::ChangeAspect::Visibility;
      }
      visible_ = visible;
    }
    if (policy_dirty) {
      FetchPolicy(delegate);
    }
    if (source_dirty) {
      if (!FetchSource(delegate, positions_dirty, orientations_dirty,
              scales_dirty, opacities_dirty, radiance_dirty,
              degree_dirty)) {
        InvalidatePayload();
        *dirty_bits = HdChangeTracker::Clean;
        return;
      }
      const auto normalized =
          merlin::NormalizeGaussianSource(source_, &g_diagnostic_sink);
      if (!normalized.accepted()) {
        InvalidatePayload();
        *dirty_bits = HdChangeTracker::Clean;
        return;
      }
      auto descriptor = *normalized.resource;
      if (!payload_valid_) {
        aspects |= merlin::ChangeAspect::GaussianPositions |
                   merlin::ChangeAspect::GaussianCovariance |
                   merlin::ChangeAspect::GaussianOpacity |
                   merlin::ChangeAspect::GaussianRadiance |
                   merlin::ChangeAspect::GaussianPolicy;
      } else {
        if (!Same(descriptor_.positions, descriptor.positions)) {
          aspects |= merlin::ChangeAspect::GaussianPositions;
        }
        if (!Same(descriptor_.covariances, descriptor.covariances)) {
          aspects |= merlin::ChangeAspect::GaussianCovariance;
        }
        if (descriptor_.opacities != descriptor.opacities) {
          aspects |= merlin::ChangeAspect::GaussianOpacity;
        }
        if (descriptor_.spherical_harmonics_degree !=
                descriptor.spherical_harmonics_degree ||
            !Same(descriptor_.spherical_harmonics_coefficients,
                descriptor.spherical_harmonics_coefficients)) {
          aspects |= merlin::ChangeAspect::GaussianRadiance;
        }
        if (descriptor_.projection_mode != descriptor.projection_mode ||
            descriptor_.sorting_mode != descriptor.sorting_mode) {
          aspects |= merlin::ChangeAspect::GaussianPolicy;
        }
        particle_ranges = ChangedParticleRanges(descriptor_, descriptor,
            aspects);
      }
      descriptor_ = std::move(descriptor);
      payload_valid_ = true;
      payload_initialized_ = true;
    } else if (policy_dirty && payload_valid_) {
      if (descriptor_.projection_mode != source_.projection_mode ||
          descriptor_.sorting_mode != source_.sorting_mode) {
        descriptor_.projection_mode = source_.projection_mode;
        descriptor_.sorting_mode = source_.sorting_mode;
        aspects |= merlin::ChangeAspect::GaussianPolicy;
      }
    }
    if (!payload_valid_) {
      *dirty_bits = HdChangeTracker::Clean;
      return;
    }
    descriptor_.transform = transform_;
    descriptor_.visible = visible_;
    bridge_->SyncGaussian(GetId(), descriptor_, aspects,
        std::move(particle_ranges));
    *dirty_bits = HdChangeTracker::Clean;
  }

protected:
  HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override {
    return bits;
  }

  void _InitRepr(const TfToken& repr_token,
      HdDirtyBits* dirty_bits) override {
    (void)repr_token;
    *dirty_bits |= GetInitialDirtyBitsMask();
  }

private:
  VtValue Read(HdSceneDelegate* delegate, const char* name) const {
    g_hydra_telemetry.primvar_fetch_count.fetch_add(
        1, std::memory_order_relaxed);
    return GetPrimvar(delegate, TfToken(name));
  }

  template <typename FloatArray, typename HalfArray, typename Convert>
  bool ReadPreferredArray(HdSceneDelegate* delegate, const char* name,
      Convert convert) {
    const auto value = Read(delegate, name);
    if (value.IsHolding<FloatArray>()) {
      convert(value.UncheckedGet<FloatArray>());
      return true;
    }
    if (value.IsHolding<HalfArray>()) {
      convert(value.UncheckedGet<HalfArray>());
      return true;
    }
    return false;
  }

  bool FetchSource(HdSceneDelegate* delegate, bool positions_dirty,
      bool orientations_dirty, bool scales_dirty,
      bool opacities_dirty, bool radiance_dirty,
      bool degree_dirty) {
    const auto copy_vec3 = [](auto& destination) {
      return [&destination](const auto& values) {
        destination.reserve(values.size());
        for (const auto& value : values) {
          destination.push_back(
              {static_cast<float>(value[0]), static_cast<float>(value[1]),
                  static_cast<float>(value[2])});
        }
      };
    };
    const auto copy_quaternion = [&](const auto& values) {
      source_.orientations.reserve(values.size());
      for (const auto& value : values) {
        const auto imaginary = value.GetImaginary();
        source_.orientations.push_back(
            {static_cast<float>(value.GetReal()),
                {static_cast<float>(imaginary[0]),
                    static_cast<float>(imaginary[1]),
                    static_cast<float>(imaginary[2])}});
      }
    };
    const auto copy_scalar = [&](const auto& values) {
      source_.opacities.reserve(values.size());
      for (const auto value : values) {
        source_.opacities.push_back(static_cast<float>(value));
      }
    };

    bool positions = true;
    if (positions_dirty) {
      source_.positions.clear();
      positions = ReadPreferredArray<VtVec3fArray, VtVec3hArray>(
          delegate, "positions", copy_vec3(source_.positions));
    }
    if (orientations_dirty) {
      source_.orientations.clear();
      (void)ReadPreferredArray<VtQuatfArray, VtQuathArray>(
          delegate, "orientations", copy_quaternion);
    }
    if (scales_dirty) {
      source_.scales.clear();
      (void)ReadPreferredArray<VtVec3fArray, VtVec3hArray>(
          delegate, "scales", copy_vec3(source_.scales));
    }
    if (opacities_dirty) {
      source_.opacities.clear();
      (void)ReadPreferredArray<VtFloatArray, VtArray<GfHalf>>(
          delegate, "opacities", copy_scalar);
    }
    if (radiance_dirty) {
      source_.spherical_harmonics_coefficients.clear();
      (void)ReadPreferredArray<VtVec3fArray, VtVec3hArray>(
          delegate, "radiance:sphericalHarmonicsCoefficients",
          copy_vec3(source_.spherical_harmonics_coefficients));
    }
    if (degree_dirty) {
      source_.spherical_harmonics_degree = 3;
      const auto degree = Read(
          delegate, "radiance:sphericalHarmonicsDegree");
      if (degree.IsHolding<int>()) {
        const int value = degree.UncheckedGet<int>();
        if (value < 0) {
          ReportHydraDiagnostic(
              "hydra.gaussian.negative-sh-degree", GetId(),
              "spherical-harmonic degree is negative",
              merlin::DiagnosticDisposition::Rejected,
              "reject-particle-field");
          return false;
        }
        source_.spherical_harmonics_degree =
            static_cast<std::uint32_t>(value);
      }
    }
    if (!positions) {
      ReportHydraDiagnostic(
          "hydra.gaussian.positions-type", GetId(),
          "positions value is not a float3 or half3 array",
          merlin::DiagnosticDisposition::Rejected,
          "reject-particle-field");
    }
    return positions;
  }

  void FetchPolicy(HdSceneDelegate* delegate) {
    source_.projection_mode = merlin::GaussianProjectionMode::Perspective;
    const auto projection =
        delegate->Get(GetId(), TfToken("projectionModeHint"));
    if (projection.IsHolding<TfToken>() &&
        projection.UncheckedGet<TfToken>() == TfToken("tangential")) {
      source_.projection_mode = merlin::GaussianProjectionMode::Tangential;
    } else if (projection.IsHolding<TfToken>() &&
               projection.UncheckedGet<TfToken>() != TfToken("perspective")) {
      ReportHydraDiagnostic(
          "hydra.gaussian.unsupported-projection-hint", GetId(),
          "unsupported Gaussian projection hint",
          merlin::DiagnosticDisposition::Fallback, "perspective");
    } else if (projection.IsEmpty()) {
      ReportHydraDiagnostic(
          "hydra.gaussian.projection-hint-unavailable", GetId(),
          "Hydra did not transport the Gaussian projection hint",
          merlin::DiagnosticDisposition::Fallback, "perspective");
    }
    source_.sorting_mode = merlin::GaussianSortingMode::ZDepth;
    const auto sorting = delegate->Get(GetId(), TfToken("sortingModeHint"));
    if (sorting.IsHolding<TfToken>() &&
        sorting.UncheckedGet<TfToken>() == TfToken("cameraDistance")) {
      source_.sorting_mode = merlin::GaussianSortingMode::CameraDistance;
    } else if (sorting.IsHolding<TfToken>() &&
               sorting.UncheckedGet<TfToken>() != TfToken("zDepth")) {
      ReportHydraDiagnostic(
          "hydra.gaussian.unsupported-sorting-hint", GetId(),
          "unsupported Gaussian sorting hint",
          merlin::DiagnosticDisposition::Fallback, "zDepth");
    } else if (sorting.IsEmpty()) {
      ReportHydraDiagnostic(
          "hydra.gaussian.sorting-hint-unavailable", GetId(),
          "Hydra did not transport the Gaussian sorting hint",
          merlin::DiagnosticDisposition::Fallback, "zDepth");
    }
  }

  static bool Same(merlin::Vec3 lhs, merlin::Vec3 rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
  }

  static bool Same(merlin::Covariance3 lhs, merlin::Covariance3 rhs) {
    return lhs.xx == rhs.xx && lhs.xy == rhs.xy && lhs.xz == rhs.xz &&
           lhs.yy == rhs.yy && lhs.yz == rhs.yz && lhs.zz == rhs.zz;
  }

  template <typename Value>
  static bool Same(const std::vector<Value>& lhs,
      const std::vector<Value>& rhs) {
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin(),
               [](const Value& a, const Value& b) {
                 return Same(a, b);
               });
  }

  static std::optional<std::vector<merlin::ElementRange>>
  ChangedParticleRanges(const merlin::GaussianDescriptor& previous,
      const merlin::GaussianDescriptor& next,
      merlin::ChangeAspect aspects) {
    constexpr auto payload_aspects =
        merlin::ChangeAspect::GaussianPositions |
        merlin::ChangeAspect::GaussianCovariance |
        merlin::ChangeAspect::GaussianOpacity |
        merlin::ChangeAspect::GaussianRadiance;
    if (!merlin::HasAnyAspect(aspects, payload_aspects)) {
      return std::nullopt;
    }
    if (previous.positions.size() != next.positions.size() ||
        previous.spherical_harmonics_degree !=
            next.spherical_harmonics_degree) {
      return std::nullopt;
    }
    const auto count = next.positions.size();
    const auto coefficient_count =
        static_cast<std::size_t>(next.spherical_harmonics_degree + 1U) *
        (next.spherical_harmonics_degree + 1U);
    std::vector<merlin::ElementRange> ranges;
    std::size_t first = count;
    for (std::size_t index = 0; index < count; ++index) {
      bool changed{};
      if (merlin::HasAnyAspect(
              aspects, merlin::ChangeAspect::GaussianPositions)) {
        changed = !Same(previous.positions[index], next.positions[index]);
      }
      if (!changed && merlin::HasAnyAspect(
                          aspects,
                          merlin::ChangeAspect::GaussianCovariance)) {
        changed = !Same(previous.covariances[index],
            next.covariances[index]);
      }
      if (!changed && merlin::HasAnyAspect(
                          aspects, merlin::ChangeAspect::GaussianOpacity)) {
        changed = previous.opacities[index] != next.opacities[index];
      }
      if (!changed && merlin::HasAnyAspect(
                          aspects, merlin::ChangeAspect::GaussianRadiance)) {
        const auto offset = index * coefficient_count;
        for (std::size_t coefficient = 0;
            coefficient < coefficient_count; ++coefficient) {
          if (!Same(previous.spherical_harmonics_coefficients[offset + coefficient],
                  next.spherical_harmonics_coefficients[offset + coefficient])) {
            changed = true;
            break;
          }
        }
      }
      if (changed && first == count) {
        first = index;
      } else if (!changed && first != count) {
        ranges.push_back(
            {static_cast<std::uint32_t>(first),
                static_cast<std::uint32_t>(index - first)});
        first = count;
      }
    }
    if (first != count) {
      ranges.push_back({static_cast<std::uint32_t>(first),
          static_cast<std::uint32_t>(count - first)});
    }
    return ranges;
  }

  void InvalidatePayload() {
    bridge_->RemoveGaussian(GetId());
    descriptor_ = {};
    source_.positions.clear();
    source_.orientations.clear();
    source_.scales.clear();
    source_.opacities.clear();
    source_.spherical_harmonics_coefficients.clear();
    payload_valid_ = false;
    payload_initialized_ = false;
  }

  std::shared_ptr<SceneBridge> bridge_;
  HydraDirtyTracker* dirty_tracker_{};
  merlin::GaussianSourceData source_;
  merlin::GaussianDescriptor descriptor_;
  merlin::Mat4 transform_;
  bool visible_{true};
  bool payload_valid_{};
  bool payload_initialized_{};
};

class HdMerlinCamera final : public HdCamera {
public:
  HdMerlinCamera(const SdfPath& id, std::shared_ptr<SceneBridge> bridge)
      : HdCamera(id), bridge_(std::move(bridge)) {
  }

  ~HdMerlinCamera() override {
    bridge_->RemoveCamera(GetId());
  }

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
      : HdRenderPass(index, collection), bridge_(std::move(bridge)) {
  }

private:
  void _Execute(const HdRenderPassStateSharedPtr& render_pass_state,
      const TfTokenVector& render_tags) override {
    bridge_->Render(*render_pass_state, render_pass_state->GetAovBindings(),
        GetRprimCollection(), render_tags, GetRenderIndex());
  }

  std::shared_ptr<SceneBridge> bridge_;
};

} // namespace

bool HdMerlinCanUseExclusiveGpuColorCopy(
    std::size_t color_buffer_count,
    std::size_t gpu_copy_candidate_count) noexcept {
  return color_buffer_count == 1 && gpu_copy_candidate_count == 1;
}

HdMerlinRenderBuffer::HdMerlinRenderBuffer(
    const SdfPath& id,
    std::shared_ptr<HdMerlinHgiVulkanBridge> hgi_vulkan_bridge,
    std::shared_ptr<HdMerlinHgiMetalBridge> hgi_metal_bridge)
    : HdRenderBuffer(id),
      hgi_vulkan_bridge_(std::move(hgi_vulkan_bridge)),
      hgi_metal_bridge_(std::move(hgi_metal_bridge)) {
}

HdMerlinRenderBuffer::~HdMerlinRenderBuffer() {
  std::scoped_lock lock(mutex_);
  DestroyHgiTargetLocked();
}

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
  // Hydra re-issues Allocate whenever an AOV binding is set up, so an
  // unchanged description keeps its target: retiring and recreating identical
  // GPU memory every frame would also make target_recreations report something
  // other than resizes.
  const bool has_target = static_cast<bool>(hgi_vulkan_target_) ||
                          static_cast<bool>(hgi_metal_target_);
  const bool reusable_target = has_target &&
                               dimensions_ == dimensions &&
                               format_ == format &&
                               multi_sampled_ == multi_sampled;
  const bool recreation = has_target && !reusable_target;
  if (!reusable_target) {
    DestroyHgiTargetLocked();
    hgi_vulkan_copy_submission_serial_ = 0;
  }
  dimensions_ = dimensions;
  format_ = format;
  multi_sampled_ = multi_sampled;
  gpu_only_ = false;
  gpu_copy_ready_ = false;
  converged_ = false;
  data_.assign(pixels * pixel_size, 0);
  // Native Hgi targets keep depth/IDs on the GPU. Host depth composition
  // consumes the texture; selection and picking download only when Map is used.
  // Non-native host compositions retain Tier 0 buffers.
  const HgiFormat hgi_format = HdMerlinHgiFormatForRenderBuffer(format);
  const bool native_aov_target =
      (hgi_vulkan_bridge_ && hgi_vulkan_bridge_->status().gpu_copy)
#ifdef MERLIN_HYDRA2_ENABLE_HGI_METAL_BRIDGE
      || (hgi_metal_bridge_ && hgi_metal_bridge_->status().gpu_copy)
#endif
      ;
  if (!reusable_target && pixels != 0 &&
      (format == HdFormatUNorm8Vec4 || native_aov_target) &&
      hgi_format != HgiFormatInvalid && dimensions[2] == 1) {
    HgiTextureDesc descriptor;
    descriptor.debugName = GetId().GetString();
    descriptor.usage = HgiTextureUsageBitsShaderRead;
    if (format == HdFormatUNorm8Vec4 && native_aov_target) {
      // Hdx selection highlighting composites directly into the color AOV.
      descriptor.usage |= HgiTextureUsageBitsColorTarget;
    } else if (format == HdFormatFloat32) {
      descriptor.usage |= HgiTextureUsageBitsDepthTarget;
    }
    descriptor.format = hgi_format;
    descriptor.type = HgiTextureType2D;
    descriptor.dimensions = dimensions;
    descriptor.layerCount = 1;
    descriptor.mipLevels = 1;
    descriptor.sampleCount = HgiSampleCount1;
    descriptor.pixelsByteSize = data_.size();
    if (hgi_vulkan_bridge_) {
      hgi_vulkan_target_ =
          hgi_vulkan_bridge_->CreateTarget(descriptor, recreation);
    }
#ifdef MERLIN_HYDRA2_ENABLE_HGI_METAL_BRIDGE
    if (hgi_metal_bridge_) {
      hgi_metal_target_ =
          hgi_metal_bridge_->CreateTarget(descriptor, recreation);
    }
#endif
  }
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
  if (gpu_only_) {
    // Color remains a presentation-only resource. Non-color CPU consumers
    // download the latest target exactly once between renderer submissions.
    if (format_ == HdFormatUNorm8Vec4) {
      return nullptr;
    }
    bool downloaded = hgi_vulkan_target_ && hgi_vulkan_bridge_ &&
                      hgi_vulkan_bridge_->Download(hgi_vulkan_target_, data_.data(), data_.size());
#ifdef MERLIN_HYDRA2_ENABLE_HGI_METAL_BRIDGE
    if (hgi_metal_target_ && hgi_metal_bridge_) {
      downloaded = hgi_metal_bridge_->Download(hgi_metal_target_, data_.data(), data_.size());
    }
#endif
    if (!downloaded) {
      return nullptr;
    }
    gpu_only_ = false;
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

VtValue HdMerlinRenderBuffer::GetResource(bool multi_sampled) const {
  std::scoped_lock lock(mutex_);
  if (multi_sampled || multi_sampled_ ||
      (!hgi_vulkan_target_ && !hgi_metal_target_)) {
    return {};
  }
  return VtValue(hgi_metal_target_ ? hgi_metal_target_ : hgi_vulkan_target_);
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
  gpu_only_ = false;
  gpu_copy_ready_ = false;
  UploadHgiTargetLocked();
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
  gpu_only_ = false;
  gpu_copy_ready_ = false;
  UploadHgiTargetLocked();
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
  gpu_only_ = false;
  gpu_copy_ready_ = false;
  UploadHgiTargetLocked();
  return true;
}

bool HdMerlinRenderBuffer::CanGpuCopyColor() const {
  return CanGpuCopyAov(merlin::Aov::Color);
}

bool HdMerlinRenderBuffer::CanGpuCopyAov(merlin::Aov aov) const {
  std::scoped_lock lock(mutex_);
  const auto expected_format = aov == merlin::Aov::Color   ? HdFormatUNorm8Vec4
                               : aov == merlin::Aov::Depth ? HdFormatFloat32
                                                           : HdFormatInt32;
  const bool supported = aov == merlin::Aov::Color || aov == merlin::Aov::Depth ||
                         aov == merlin::Aov::PrimId || aov == merlin::Aov::InstanceId;
  return supported && format_ == expected_format && !multi_sampled_ && map_count_ == 0 &&
         ((hgi_vulkan_target_ && hgi_vulkan_bridge_ &&
              hgi_vulkan_bridge_->status().gpu_copy) ||
#ifdef MERLIN_HYDRA2_ENABLE_HGI_METAL_BRIDGE
             (hgi_metal_target_ && hgi_metal_bridge_ &&
                 hgi_metal_bridge_->status().gpu_copy));
#else
             false);
#endif
}

bool HdMerlinRenderBuffer::CopyAov(
    merlin::vulkan::AovImageExport&& source,
    std::shared_ptr<merlin::render::Backend> backend) {
  std::scoped_lock lock(mutex_);
  if (map_count_ != 0 || !hgi_vulkan_target_ || !hgi_vulkan_bridge_) {
#ifdef MERLIN_HYDRA2_ENABLE_VULKAN
    if (auto* exporter = dynamic_cast<merlin::vulkan::AovImageExporter*>(backend.get());
        exporter != nullptr && source.lease) {
      exporter->ReleaseAovImage(std::move(source.lease));
    }
#endif
    return false;
  }
  const bool previous_copy_complete =
      hgi_vulkan_copy_submission_serial_ != 0 &&
      hgi_vulkan_bridge_->IsCopyComplete(
          hgi_vulkan_copy_submission_serial_);
  std::uint64_t submission_serial{};
  if (!hgi_vulkan_bridge_->Copy(hgi_vulkan_target_, std::move(source),
          std::move(backend), submission_serial)) {
    hgi_vulkan_bridge_->DestroyTarget(&hgi_vulkan_target_);
    gpu_only_ = false;
    gpu_copy_ready_ = false;
    hgi_vulkan_copy_submission_serial_ = 0;
    return false;
  }
  hgi_vulkan_copy_submission_serial_ = submission_serial;
  gpu_only_ = true;
  // The copy submitted above is intentionally asynchronous. Keep the buffer
  // unconverged until an earlier copy into the same retained target has
  // completed, so a host cannot stop on the target's initial black contents.
  gpu_copy_ready_ = previous_copy_complete;
  return true;
}

bool HdMerlinRenderBuffer::CopyAov(
    merlin::metal::AovImageExport&& source,
    std::shared_ptr<merlin::render::Backend> backend) {
#ifdef MERLIN_HYDRA2_ENABLE_HGI_METAL_BRIDGE
  std::scoped_lock lock(mutex_);
  if (map_count_ != 0 || !hgi_metal_target_ || !hgi_metal_bridge_) {
    if (auto* exporter = dynamic_cast<merlin::metal::AovImageExporter*>(backend.get());
        exporter != nullptr && source.lease) {
      exporter->ReleaseAovImage(std::move(source.lease));
    }
    return false;
  }
  if (!hgi_metal_bridge_->Copy(hgi_metal_target_, std::move(source),
          std::move(backend))) {
    hgi_metal_bridge_->DestroyTarget(&hgi_metal_target_);
    gpu_only_ = false;
    return false;
  }
  gpu_only_ = true;
  gpu_copy_ready_ = true;
  return true;
#else
  (void)source;
  (void)backend;
  return false;
#endif
}

void HdMerlinRenderBuffer::SetConverged(bool converged) {
  std::scoped_lock lock(mutex_);
  converged_ = converged && (!gpu_only_ || gpu_copy_ready_);
}

void HdMerlinRenderBuffer::_Deallocate() {
  std::scoped_lock lock(mutex_);
  if (map_count_ != 0) {
    return;
  }
  DestroyHgiTargetLocked();
  dimensions_ = GfVec3i(0);
  format_ = HdFormatInvalid;
  multi_sampled_ = false;
  gpu_only_ = false;
  gpu_copy_ready_ = false;
  converged_ = false;
  hgi_vulkan_copy_submission_serial_ = 0;
  data_.clear();
}

void HdMerlinRenderBuffer::UploadHgiTargetLocked() {
  if (!hgi_vulkan_target_ && !hgi_metal_target_) {
    return;
  }
  // A failed upload retires the target so GetResource stops advertising a
  // stale texture; the bridge records why and the CPU buffer stays the truth.
  if (hgi_vulkan_target_ &&
      (!hgi_vulkan_bridge_ || !hgi_vulkan_bridge_->Upload(
                                  hgi_vulkan_target_, data_.data(), data_.size()))) {
    if (hgi_vulkan_bridge_) {
      hgi_vulkan_bridge_->DestroyTarget(&hgi_vulkan_target_);
    } else {
      hgi_vulkan_target_ = {};
    }
  }
#ifdef MERLIN_HYDRA2_ENABLE_HGI_METAL_BRIDGE
  if (hgi_metal_target_ &&
      (!hgi_metal_bridge_ || !hgi_metal_bridge_->Upload(
                                 hgi_metal_target_, data_.data(), data_.size()))) {
    if (hgi_metal_bridge_) {
      hgi_metal_bridge_->DestroyTarget(&hgi_metal_target_);
    } else {
      hgi_metal_target_ = {};
    }
  }
#endif
}

void HdMerlinRenderBuffer::DestroyHgiTargetLocked() {
  if (hgi_vulkan_bridge_ && hgi_vulkan_target_) {
    hgi_vulkan_bridge_->DestroyTarget(&hgi_vulkan_target_);
  } else {
    hgi_vulkan_target_ = {};
  }
#ifdef MERLIN_HYDRA2_ENABLE_HGI_METAL_BRIDGE
  if (hgi_metal_bridge_ && hgi_metal_target_) {
    hgi_metal_bridge_->DestroyTarget(&hgi_metal_target_);
  } else {
    hgi_metal_target_ = {};
  }
#else
  hgi_metal_target_ = {};
#endif
  hgi_vulkan_copy_submission_serial_ = 0;
  gpu_copy_ready_ = false;
}

namespace {

#ifdef MERLIN_HYDRA2_ENABLE_HGI_VULKAN_BRIDGE
constexpr bool kHgiVulkanBridgeEnabled = true;
#else
constexpr bool kHgiVulkanBridgeEnabled = false;
#endif
#ifdef MERLIN_HYDRA2_ENABLE_HGI_METAL_BRIDGE
bool HgiMetalBridgeEnabled() {
  const auto* disabled = std::getenv("MERLIN_HYDRA2_DISABLE_HGI_METAL_BRIDGE");
  return disabled == nullptr || std::string_view(disabled) != "1";
}
#endif

} // namespace

class HdMerlinRenderDelegate::Impl {
public:
  Impl()
      : hgi_vulkan_bridge(
            std::make_shared<HdMerlinHgiVulkanBridge>(
                kHgiVulkanBridgeEnabled)),
#ifdef MERLIN_HYDRA2_ENABLE_HGI_METAL_BRIDGE
        hgi_metal_bridge(std::make_shared<HdMerlinHgiMetalBridge>(
            HgiMetalBridgeEnabled())),
#endif
        bridge(std::make_shared<SceneBridge>(nullptr, hgi_vulkan_bridge,
            hgi_metal_bridge)) {
  }
  explicit Impl(std::shared_ptr<merlin::render::Backend> backend)
      : hgi_vulkan_bridge(
            std::make_shared<HdMerlinHgiVulkanBridge>(
                kHgiVulkanBridgeEnabled)),
#ifdef MERLIN_HYDRA2_ENABLE_HGI_METAL_BRIDGE
        hgi_metal_bridge(std::make_shared<HdMerlinHgiMetalBridge>(
            HgiMetalBridgeEnabled())) {
#else
        hgi_metal_bridge{} {
#endif
    if (!backend) {
      throw std::invalid_argument("Hydra renderer backend is null");
    }
    bridge = std::make_shared<SceneBridge>(std::move(backend),
        hgi_vulkan_bridge,
        hgi_metal_bridge);
  }

  std::shared_ptr<HdMerlinHgiVulkanBridge> hgi_vulkan_bridge{
      std::make_shared<HdMerlinHgiVulkanBridge>(kHgiVulkanBridgeEnabled)};
  std::shared_ptr<HdMerlinHgiMetalBridge> hgi_metal_bridge;
  std::shared_ptr<SceneBridge> bridge;
  HydraDirtyTracker dirty_tracker;
  HdSceneIndexBaseRefPtr terminal_scene_index;
};

HdMerlinRenderDelegate::HdMerlinRenderDelegate(
    const HdRenderSettingsMap& settings)
    : HdRenderDelegate(settings),
      impl_(std::make_unique<Impl>()),
      resources_(std::make_shared<HdResourceRegistry>()) {
  ApplyInitialRenderSettings();
}

HdMerlinRenderDelegate::HdMerlinRenderDelegate(
    std::shared_ptr<merlin::render::Backend> backend,
    const HdRenderSettingsMap& settings)
    : HdRenderDelegate(settings),
      impl_(std::make_unique<Impl>(std::move(backend))),
      resources_(std::make_shared<HdResourceRegistry>()) {
  ApplyInitialRenderSettings();
}

void HdMerlinRenderDelegate::ApplyInitialRenderSettings() {
  // Capture both host values before canonicalizing either setting.
  const auto exposure = HdRenderDelegate::GetRenderSetting(HdMerlinRenderSettingsTokens->exposureEv);
  const auto tone = HdRenderDelegate::GetRenderSetting(HdMerlinRenderSettingsTokens->toneMapping);
  if (!exposure.IsEmpty())
    SetRenderSetting(HdMerlinRenderSettingsTokens->exposureEv, exposure);
  if (!tone.IsEmpty())
    SetRenderSetting(HdMerlinRenderSettingsTokens->toneMapping, tone);
  const auto debug_view =
      HdRenderDelegate::GetRenderSetting(HdMerlinRenderSettingsTokens->debugView);
  if (!debug_view.IsEmpty())
    SetRenderSetting(HdMerlinRenderSettingsTokens->debugView, debug_view);
  const auto dome_visibility =
      HdRenderDelegate::GetRenderSetting(HdRenderSettingsTokens->domeLightCameraVisibility);
  if (dome_visibility.IsHolding<bool>()) {
    impl_->bridge->SetDomeBackgroundVisible(dome_visibility.UncheckedGet<bool>());
  }
  const merlin::render::GpuDrivenGaussianSettings defaults;
  merlin::render::GpuDrivenGaussianSettings settings;
  for (const auto& key : GaussianRenderSettingKeys()) {
    const auto value = HdRenderDelegate::GetRenderSetting(key);
    if (value.IsEmpty()) {
      continue;
    }
    if (const auto error = ReadGaussianRenderSetting(key, value, settings)) {
      ReportRenderSettingRejection(key, *error, defaults);
    }
  }
  // Also replaces any rejected host value in the settings map with the
  // canonical policy.
  RequestGpuDrivenGaussianSettings(
      settings, HdMerlinRenderSettingsTokens->gpuDrivenGaussianMode);
}

// Hosts list only the flags. usdview's settings dialog re-sends every listed
// setting as it was when the dialog opened, so listing the policy names as
// well would let an untouched stale value undo an edited one. The names stay
// settable for require and for scripts.
HdRenderSettingDescriptorList
HdMerlinRenderDelegate::GetRenderSettingDescriptors() const {
  return {{"GPU Gaussian execution",
              HdMerlinRenderSettingsTokens->gpuDrivenGaussianEnabled,
              VtValue(false)},
      {"Gaussian tile raster",
          HdMerlinRenderSettingsTokens->gpuDrivenGaussianTiled,
          VtValue(false)},
      {"Gaussian adaptive tile bounds",
          HdMerlinRenderSettingsTokens->gpuDrivenGaussianAdaptiveBounds,
          VtValue(false)},
      {"Forward exposure (EV)", HdMerlinRenderSettingsTokens->exposureEv, VtValue(0.0F)},
      {"Forward tone mapping", HdMerlinRenderSettingsTokens->toneMapping,
          VtValue(std::string("reinhard"))},
      {"Forward debug view", HdMerlinRenderSettingsTokens->debugView,
          VtValue(std::string("none"))}};
}

void HdMerlinRenderDelegate::SetRenderSetting(
    const TfToken& key, const VtValue& value) {
  if (key == HdRenderSettingsTokens->domeLightCameraVisibility && value.IsHolding<bool>()) {
    impl_->bridge->SetDomeBackgroundVisible(value.UncheckedGet<bool>());
  }
  if (key == HdMerlinRenderSettingsTokens->exposureEv ||
      key == HdMerlinRenderSettingsTokens->toneMapping) {
    const auto previous = impl_->bridge->EffectiveOutputTransformSettings();
    auto settings = previous;
    merlin::render::RendererSettingsValidationError error;
    if (key == HdMerlinRenderSettingsTokens->exposureEv) {
      if (value.IsHolding<double>()) {
        const auto ev = value.UncheckedGet<double>();
        if (!std::isfinite(ev) || ev < -32.0 || ev > 32.0) {
          error = {"renderer-settings.invalid-exposure",
              "Exposure must be finite and between -32 and 32 EV."};
        } else {
          settings.exposure_ev = static_cast<float>(ev);
        }
      } else if (value.IsHolding<float>()) {
        settings.exposure_ev = value.UncheckedGet<float>();
      } else {
        error = {"renderer-settings.invalid-exposure", "Exposure requires a float or double EV value."};
      }
    } else {
      const auto name = ReadRenderSettingString(value);
      if (name == "none")
        settings.tone_mapping = merlin::render::ToneMapping::None;
      else if (name == "reinhard")
        settings.tone_mapping = merlin::render::ToneMapping::Reinhard;
      else if (name == "aces")
        settings.tone_mapping = merlin::render::ToneMapping::Aces;
      else
        error = {"renderer-settings.invalid-tone-mapping", "Tone mapping requires None or Reinhard (none/reinhard)."};
    }
    merlin::render::RendererSettings contract;
    contract.exposure_ev = settings.exposure_ev;
    contract.tone_mapping = settings.tone_mapping;
    if (error.code.empty()) {
      if (const auto invalid = merlin::render::ValidateRendererSettings(contract))
        error = *invalid;
    }
    if (!error.code.empty()) {
      g_diagnostic_sink.Report({merlin::kDiagnosticSchemaVersion, error.code,
          merlin::DiagnosticSeverity::Warning, merlin::DiagnosticDisposition::Rejected,
          key.GetString(), error.message, "keep " + std::to_string(previous.exposure_ev) + " EV/" + std::string(merlin::render::ToneMappingName(previous.tone_mapping))});
      // A constructor may have inherited a rejected value from the host map.
      // Getter always returns effective state; rejected live edits don't revise it.
      return;
    }
    SetOutputTransformSettings(settings);
    return;
  }
  if (key == HdMerlinRenderSettingsTokens->debugView) {
    const auto previous = impl_->bridge->EffectiveDebugView();
    const auto name = ReadRenderSettingString(value);
    std::optional<merlin::render::DebugView> requested;
    for (const auto view : {merlin::render::DebugView::None,
             merlin::render::DebugView::Color, merlin::render::DebugView::Depth,
             merlin::render::DebugView::PrimId,
             merlin::render::DebugView::InstanceId,
             merlin::render::DebugView::Normal,
             merlin::render::DebugView::GeometricNormal,
             merlin::render::DebugView::ShadingNormalBase,
             merlin::render::DebugView::FrontFacing,
             merlin::render::DebugView::Tangent,
             merlin::render::DebugView::Bitangent}) {
      if (name == merlin::render::DebugViewName(view))
        requested = view;
    }
    merlin::render::RendererSettingsValidationError error{
        "renderer-settings.invalid-debug-view",
        "Debug view requires none, normal, geometric-normal, "
        "shading-normal-base, front-facing, tangent or bitangent."};
    if (requested) {
      // Representable views outside the surface context reject with the
      // validator's shared code.
      error = merlin::render::IsForwardSurfaceDebugView(*requested)
                  ? merlin::render::RendererSettingsValidationError{}
                  : merlin::render::RendererSettingsValidationError{
                        "renderer-settings.debug-view-unsupported",
                        "Only the Forward surface debug views are connected "
                        "to renderer settings."};
    }
    if (!error.code.empty()) {
      g_diagnostic_sink.Report({merlin::kDiagnosticSchemaVersion, error.code,
          merlin::DiagnosticSeverity::Warning, merlin::DiagnosticDisposition::Rejected,
          key.GetString(), error.message,
          "keep " + std::string(merlin::render::DebugViewName(previous))});
      return;
    }
    SetDebugView(*requested);
    return;
  }
  if (!IsGaussianRenderSetting(key)) {
    HdRenderDelegate::SetRenderSetting(key, value);
    return;
  }
  const auto effective = impl_->bridge->EffectiveGpuDrivenGaussianSettings();
  auto settings = effective;
  if (const auto error = ReadGaussianRenderSetting(key, value, settings)) {
    ReportRenderSettingRejection(key, *error, effective);
    return;
  }
  // Hosts such as usdview re-send every setting on apply; an unchanged
  // policy neither revalidates nor reports again.
  if (settings == effective) {
    return;
  }
  RequestGpuDrivenGaussianSettings(settings, key);
}

VtValue HdMerlinRenderDelegate::GetRenderSetting(const TfToken& key) const {
  if (key == HdMerlinRenderSettingsTokens->exposureEv) {
    return VtValue(static_cast<double>(impl_->bridge->EffectiveOutputTransformSettings().exposure_ev));
  }
  if (key == HdMerlinRenderSettingsTokens->toneMapping) {
    return VtValue(std::string(merlin::render::ToneMappingName(
        impl_->bridge->EffectiveOutputTransformSettings().tone_mapping)));
  }
  if (key == HdMerlinRenderSettingsTokens->debugView) {
    return VtValue(std::string(
        merlin::render::DebugViewName(impl_->bridge->EffectiveDebugView())));
  }
  if (!IsGaussianRenderSetting(key)) {
    return HdRenderDelegate::GetRenderSetting(key);
  }
  return GaussianRenderSettingValue(
      key, impl_->bridge->EffectiveGpuDrivenGaussianSettings());
}

void HdMerlinRenderDelegate::RequestGpuDrivenGaussianSettings(
    merlin::render::GpuDrivenGaussianSettings settings,
    const TfToken& source) {
  const auto previous = impl_->bridge->EffectiveGpuDrivenGaussianSettings();
  if (const auto error =
          impl_->bridge->RequestGpuDrivenGaussianSettings(settings, source)) {
    ReportRenderSettingRejection(source, *error, previous);
    return;
  }
  // Stores the canonical values so the settings version advances only when
  // the requested policy changes.
  for (const auto& key : GaussianRenderSettingKeys()) {
    HdRenderDelegate::SetRenderSetting(
        key, GaussianRenderSettingValue(key, settings));
  }
}

HdMerlinRenderDelegate::~HdMerlinRenderDelegate() = default;

void HdMerlinRenderDelegate::SetDrivers(const HdDriverVector& drivers) {
  HdRenderDelegate::SetDrivers(drivers);
  impl_->hgi_vulkan_bridge->SetDrivers(drivers);
#ifdef MERLIN_HYDRA2_ENABLE_HGI_METAL_BRIDGE
  impl_->hgi_metal_bridge->SetDrivers(drivers);
#endif
  // HgiMetal reflects the host projection even when presentation falls back
  // to CPU RenderBuffers. Camera orientation is a driver contract, independent
  // of whether a native-copy bridge was built or negotiated successfully.
  bool hgi_projection_y_reflection = false;
  for (const auto* driver : drivers) {
    if (driver && driver->name == HgiTokens->renderDriver &&
        driver->driver.IsHolding<Hgi*>()) {
      const auto* hgi = driver->driver.UncheckedGet<Hgi*>();
      hgi_projection_y_reflection = hgi && hgi->GetAPIName() == HgiTokens->Metal;
      break;
    }
  }
  impl_->bridge->SetHgiProjectionYReflection(hgi_projection_y_reflection);
}

void HdMerlinRenderDelegate::SetCameraFrontFaceCounterClockwise(
    bool counter_clockwise) {
  impl_->bridge->SetCameraFrontFaceWinding(
      counter_clockwise ? merlin::FrontFaceWinding::CounterClockwise
                        : merlin::FrontFaceWinding::Clockwise);
}

void HdMerlinRenderDelegate::SetHgiProjectionYReflection(bool reflect) {
  impl_->bridge->SetHgiProjectionYReflection(reflect);
}

void HdMerlinRenderDelegate::SetOutputTransformSettings(
    merlin::render::OutputTransformSettings settings) {
  // Validate both fields before either is changed.
  (void)merlin::render::PackForwardOutputTransform(settings);
  if (settings == impl_->bridge->EffectiveOutputTransformSettings())
    return;
  impl_->bridge->SetOutputTransformSettings(settings);
  HdRenderDelegate::SetRenderSetting(HdMerlinRenderSettingsTokens->exposureEv,
      VtValue(static_cast<double>(settings.exposure_ev)));
  HdRenderDelegate::SetRenderSetting(HdMerlinRenderSettingsTokens->toneMapping,
      VtValue(std::string(merlin::render::ToneMappingName(settings.tone_mapping))));
}

void HdMerlinRenderDelegate::SetDebugView(merlin::render::DebugView view) {
  if (!merlin::render::IsForwardSurfaceDebugView(view)) {
    throw merlin::render::RendererError(
        merlin::render::RendererErrorCode::InvalidRequest, "set debug view",
        "debug view " + std::string(merlin::render::DebugViewName(view)) +
            " is not executed by the Forward surface context");
  }
  if (view == impl_->bridge->EffectiveDebugView())
    return;
  impl_->bridge->SetDebugView(view);
  HdRenderDelegate::SetRenderSetting(HdMerlinRenderSettingsTokens->debugView,
      VtValue(std::string(merlin::render::DebugViewName(view))));
}

void HdMerlinRenderDelegate::SetGpuDrivenIndexedSettings(
    merlin::render::GpuDrivenIndexedSettings settings) {
  impl_->bridge->SetGpuDrivenIndexedSettings(settings);
}

void HdMerlinRenderDelegate::SetGpuDrivenGaussianSettings(
    merlin::render::GpuDrivenGaussianSettings settings) {
  RequestGpuDrivenGaussianSettings(
      settings, HdMerlinRenderSettingsTokens->gpuDrivenGaussianMode);
}

HdMerlinViewportFrame
HdMerlinRenderDelegate::GetLatestViewportFrame() const {
  auto frame = impl_->bridge->GetLatestViewportFrame();
  frame.hgi_vulkan_bridge = impl_->hgi_vulkan_bridge->status();
  frame.hgi_vulkan_telemetry = impl_->hgi_vulkan_bridge->telemetry();
#ifdef MERLIN_HYDRA2_ENABLE_HGI_METAL_BRIDGE
  frame.hgi_metal_bridge = impl_->hgi_metal_bridge->status();
  frame.hgi_metal_telemetry = impl_->hgi_metal_bridge->telemetry();
#endif
  return frame;
}

const TfTokenVector& HdMerlinRenderDelegate::GetSupportedRprimTypes() const {
  static const TfTokenVector types{HdPrimTypeTokens->mesh,
      HdPrimTypeTokens->particleField};
  return types;
}

TfTokenVector HdMerlinRenderDelegate::GetMaterialRenderContexts() const {
  return {TfToken(), TfToken("mtlx")};
}

const TfTokenVector& HdMerlinRenderDelegate::GetSupportedSprimTypes() const {
  static const TfTokenVector types{HdPrimTypeTokens->camera,
      HdPrimTypeTokens->material,
      HdPrimTypeTokens->distantLight, HdPrimTypeTokens->domeLight};
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
    return new HdMerlinMesh(rprim_id, impl_->bridge, &impl_->dirty_tracker);
  }
  if (type_id == HdPrimTypeTokens->particleField) {
    return new HdMerlinGaussian(rprim_id, impl_->bridge,
        &impl_->dirty_tracker);
  }
  return nullptr;
}

void HdMerlinRenderDelegate::DestroyRprim(HdRprim* rprim) {
  delete rprim;
}

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
  if (type_id == HdPrimTypeTokens->domeLight) {
    return new HdMerlinDomeLight(sprim_id, impl_->bridge);
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
  if (type_id == HdPrimTypeTokens->domeLight) {
    return new HdMerlinDomeLight(SdfPath(), impl_->bridge);
  }
  return nullptr;
}

void HdMerlinRenderDelegate::DestroySprim(HdSprim* sprim) {
  delete sprim;
}

HdBprim* HdMerlinRenderDelegate::CreateBprim(const TfToken& type_id,
    const SdfPath& bprim_id) {
  if (type_id == HdPrimTypeTokens->renderBuffer) {
    return new HdMerlinRenderBuffer(bprim_id, impl_->hgi_vulkan_bridge,
        impl_->hgi_metal_bridge);
  }
  return nullptr;
}

HdBprim* HdMerlinRenderDelegate::CreateFallbackBprim(const TfToken& type_id) {
  if (type_id == HdPrimTypeTokens->renderBuffer) {
    return new HdMerlinRenderBuffer(SdfPath("/__merlinFallbackRenderBuffer"),
        impl_->hgi_vulkan_bridge,
        impl_->hgi_metal_bridge);
  }
  return nullptr;
}

void HdMerlinRenderDelegate::DestroyBprim(HdBprim* bprim) {
  delete bprim;
}

void HdMerlinRenderDelegate::SetTerminalSceneIndex(
    const HdSceneIndexBaseRefPtr& terminal_scene_index) {
  if (impl_->terminal_scene_index) {
    impl_->terminal_scene_index->RemoveObserver(
        TfCreateWeakPtr(&impl_->dirty_tracker));
  }
  impl_->terminal_scene_index = terminal_scene_index;
  impl_->dirty_tracker.SetSceneIndex(terminal_scene_index);
  if (impl_->terminal_scene_index) {
    impl_->terminal_scene_index->AddObserver(
        TfCreateWeakPtr(&impl_->dirty_tracker));
  }
}

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
