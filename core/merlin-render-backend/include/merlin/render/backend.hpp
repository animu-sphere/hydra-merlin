#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <merlin/core/material_diagnostic.hpp>
#include <merlin/core/render_product.hpp>
#include <merlin/extraction/frame_snapshot.hpp>

namespace merlin::render {

struct GpuScenePackedFrameUpdate;

inline constexpr std::uint32_t kBackendContractVersion = 1;
inline constexpr std::uint32_t kRendererSettingsSchemaVersion = 1;

enum class BackendKind { Vulkan, Metal };
enum class BackendRequest { Automatic, Vulkan, Metal };
enum class PresentationMode { Automatic, Offscreen, Native, Host };
enum class RenderPath { Forward, ExperimentalVisibility };
enum class LightingMode { Diagnostic, Environment, Authored };
enum class ToneMapping { None, Reinhard, Aces };
enum class AlphaPolicy { Opaque, Mask, Blend };
enum class DebugView { None, Color, Depth, PrimId, InstanceId, Normal };
enum class TelemetryMode { Off, Basic, Detailed };

[[nodiscard]] constexpr std::string_view BackendKindName(
    BackendKind kind) noexcept {
  switch (kind) {
    case BackendKind::Vulkan: return "vulkan";
    case BackendKind::Metal: return "metal";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view BackendRequestName(
    BackendRequest request) noexcept {
  switch (request) {
    case BackendRequest::Automatic: return "automatic";
    case BackendRequest::Vulkan: return "vulkan";
    case BackendRequest::Metal: return "metal";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view PresentationModeName(
    PresentationMode mode) noexcept {
  switch (mode) {
    case PresentationMode::Automatic: return "automatic";
    case PresentationMode::Offscreen: return "offscreen";
    case PresentationMode::Native: return "native";
    case PresentationMode::Host: return "host";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view RenderPathName(
    RenderPath path) noexcept {
  switch (path) {
    case RenderPath::Forward: return "forward";
    case RenderPath::ExperimentalVisibility: return "experimental-visibility";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view LightingModeName(
    LightingMode mode) noexcept {
  switch (mode) {
    case LightingMode::Diagnostic: return "diagnostic";
    case LightingMode::Environment: return "environment";
    case LightingMode::Authored: return "authored";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view ToneMappingName(
    ToneMapping mode) noexcept {
  switch (mode) {
    case ToneMapping::None: return "none";
    case ToneMapping::Reinhard: return "reinhard";
    case ToneMapping::Aces: return "aces";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view AlphaPolicyName(
    AlphaPolicy policy) noexcept {
  switch (policy) {
    case AlphaPolicy::Opaque: return "opaque";
    case AlphaPolicy::Mask: return "mask";
    case AlphaPolicy::Blend: return "blend";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view DebugViewName(
    DebugView view) noexcept {
  switch (view) {
    case DebugView::None: return "none";
    case DebugView::Color: return "color";
    case DebugView::Depth: return "depth";
    case DebugView::PrimId: return "prim-id";
    case DebugView::InstanceId: return "instance-id";
    case DebugView::Normal: return "normal";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view TelemetryModeName(
    TelemetryMode mode) noexcept {
  switch (mode) {
    case TelemetryMode::Off: return "off";
    case TelemetryMode::Basic: return "basic";
    case TelemetryMode::Detailed: return "detailed";
  }
  return "unknown";
}

// Host-neutral configuration vocabulary. A field being representable does not
// imply that every backend implements it; ValidateRendererSettings binds the
// versioned request to the selected backend's reported capabilities.
struct RendererSettings {
  std::uint32_t schema_version{kRendererSettingsSchemaVersion};
  BackendRequest backend{BackendRequest::Automatic};
  PresentationMode presentation_mode{PresentationMode::Automatic};
  RenderPath render_path{RenderPath::Forward};
  Aov aov{Aov::Color};
  LightingMode lighting_mode{LightingMode::Diagnostic};
  float exposure_ev{};
  ToneMapping tone_mapping{ToneMapping::None};
  AlphaPolicy alpha_policy{AlphaPolicy::Opaque};
  DebugView debug_view{DebugView::None};
  bool validation{};
  TelemetryMode telemetry{TelemetryMode::Basic};

  friend constexpr bool operator==(const RendererSettings&,
                                   const RendererSettings&) = default;
};

struct RendererSettingsValidationError {
  std::string code;
  std::string message;
};

struct BackendSelection {
  BackendRequest requested{BackendRequest::Automatic};
  BackendKind selected{BackendKind::Vulkan};
  bool automatic{};
  std::string reason;
};

struct RendererLimits {
  std::uint32_t max_image_dimension_2d{};
  std::uint32_t max_frames_in_flight{};
  std::uint32_t sampled_image_slots{};
  std::uint32_t sampler_slots{};
};

// Only renderer-meaning capabilities cross this boundary. Driver/API details
// remain available from each backend's own diagnostic interface.
struct RendererCapabilities {
  std::uint32_t contract_version{kBackendContractVersion};
  BackendKind backend{BackendKind::Vulkan};
  std::string backend_name;
  std::string device_name;
  bool bindless_textures{};
  bool asynchronous_upload{};
  bool timestamp_queries{};
  bool external_presentation{};
  bool cpu_readback{};
  bool validation_enabled{};
  bool generated_materials{};
  RendererLimits limits;
};

// Returns a stable error code and human-readable reason for the first invalid
// field. When capabilities are supplied, backend and presentation/path
// capability mismatches are rejected as well as malformed schema values.
[[nodiscard]] std::optional<RendererSettingsValidationError>
ValidateRendererSettings(
    const RendererSettings& settings,
    const RendererCapabilities* capabilities = nullptr);

struct RendererStatistics {
  std::uint64_t frames_submitted{};
  std::uint64_t frames_presented{};
  std::uint64_t presentation_recreates{};
  std::uint64_t validation_messages{};
  std::uint64_t uploaded_bytes{};
  std::uint64_t readback_bytes{};
  std::uint64_t presentation_copy_bytes{};
  std::uint64_t aov_image_export_count{};
  std::uint64_t active_aov_image_leases{};
  struct Residency {
    bool memory_budget_available{};
    bool bindless_tables{};
    std::uint64_t vram_heap_capacity_bytes{};
    std::uint64_t vram_heap_budget_bytes{};
    std::uint64_t vram_heap_usage_bytes{};
    std::uint64_t vram_heap_available_bytes{};
    std::uint64_t configured_vram_limit_bytes{};
    std::uint64_t effective_vram_limit_bytes{};
    std::uint64_t renderer_allocated_bytes{};
    std::uint64_t renderer_peak_allocated_bytes{};
    std::uint64_t geometry_capacity_bytes{};
    std::uint64_t geometry_resident_bytes{};
    std::uint64_t geometry_peak_resident_bytes{};
    std::uint64_t geometry_retiring_bytes{};
    std::uint64_t gaussian_capacity_bytes{};
    std::uint64_t gaussian_resident_bytes{};
    std::uint64_t gaussian_peak_resident_bytes{};
    std::uint64_t gaussian_retiring_bytes{};
    std::uint64_t gaussian_upload_ring_capacity_bytes{};
    std::uint64_t gaussian_upload_ring_in_flight_bytes{};
    std::uint64_t upload_ring_capacity_bytes{};
    std::uint64_t upload_ring_in_flight_bytes{};
    std::uint64_t upload_ring_peak_in_flight_bytes{};
    std::uint32_t geometry_blocks{};
    std::uint32_t gaussian_blocks{};
    std::uint32_t gaussian_resources{};
    std::uint32_t texture_slots_capacity{};
    std::uint32_t texture_slots_in_use{};
    std::uint32_t texture_slots_retiring{};
    std::uint32_t sampler_slots_capacity{};
    std::uint32_t sampler_slots_in_use{};
    std::uint32_t sampler_slots_retiring{};
    std::uint32_t unique_sampler_count{};
  } residency;
};

struct FrameTimings {
  std::uint64_t upload_ns{};
  std::uint64_t gaussian_preparation_ns{};
  std::uint64_t gaussian_attribute_upload_ns{};
  std::uint64_t gaussian_prepared_upload_ns{};
  std::uint64_t gaussian_raster_ns{};
  std::uint64_t command_recording_ns{};
  std::uint64_t queue_submission_ns{};
  std::uint64_t completion_wait_ns{};
  std::uint64_t readback_ns{};
  std::uint64_t presentation_ns{};
  std::uint64_t gpu_execution_ns{};
  std::uint64_t backend_total_ns{};
};

struct FrameTelemetry {
  std::uint64_t draw_count{};
  std::uint64_t triangle_count{};
  std::uint64_t upload_bytes{};
  std::uint64_t readback_bytes{};
  std::uint64_t allocation_count{};
  std::uint64_t pipeline_creation_count{};
  std::uint64_t present_count{};
  std::uint64_t presentation_copy_bytes{};
  std::uint64_t visible_primitive_count{};
  std::uint64_t gaussian_candidate_count{};
  std::uint64_t gaussian_visible_count{};
  std::uint64_t gaussian_hidden_count{};
  std::uint64_t gaussian_opacity_culled_count{};
  std::uint64_t gaussian_frustum_culled_count{};
  std::uint64_t gaussian_invalid_culled_count{};
  std::uint64_t gaussian_sorted_count{};
  std::uint64_t gaussian_sorting_policy_fallback_count{};
  std::uint64_t gaussian_preparation_cache_hits{};
  std::uint64_t gaussian_preparation_cache_misses{};
  std::uint64_t gaussian_draw_count{};
  std::uint64_t gaussian_attribute_upload_bytes{};
  std::uint64_t gaussian_attribute_copy_range_count{};
  std::uint64_t gaussian_attribute_generation_count{};
  std::uint64_t gaussian_upload_bytes{};
  // ABI-v1 persistent GPU Scene payload copied from a caller-packed update.
  // Backends record one native copy for each packed dirty range. Staging
  // reservation/growth remains visible so a static frame can prove that it
  // performed no upload work.
  std::uint64_t gpu_scene_upload_bytes{};
  std::uint64_t gpu_scene_copy_range_count{};
  std::uint64_t gpu_scene_staging_reserved_bytes{};
  std::uint64_t gpu_scene_staging_growth_count{};
  std::uint64_t gpu_scene_staging_growth_bytes{};
  std::uint64_t gpu_scene_draw_count{};
  std::uint64_t gpu_driven_candidate_draw_count{};
  std::uint64_t gpu_driven_visible_draw_count{};
  std::uint64_t gpu_driven_visibility_mask_culled_count{};
  std::uint64_t gpu_driven_frustum_culled_count{};
  std::uint64_t gpu_driven_indirect_draw_count{};
  std::uint64_t gpu_driven_candidate_upload_bytes{};
  std::uint64_t gpu_driven_fallback_count{};
  std::uint64_t requested_aov_mask{};
  std::uint64_t rendered_aov_mask{};
  std::uint64_t cpu_readback_aov_mask{};
  std::uint64_t requested_aov_count{};
  std::uint64_t rendered_aov_count{};
  std::uint64_t cpu_readback_aov_count{};
  std::uint64_t wait_count{};
  std::uint64_t resolve_count{};
  std::uint64_t map_count{};
  std::uint64_t buffer_allocation_bytes{};
  std::uint64_t image_allocation_bytes{};
  std::uint64_t geometry_cache_misses{};
  std::uint64_t texture_cache_hits{};
  std::uint64_t texture_cache_misses{};
  std::uint64_t geometry_reconcile_count{};
  std::uint64_t texture_reconcile_count{};
  std::uint64_t sampler_reconcile_count{};
  std::uint64_t shader_module_cache_misses{};
  std::uint64_t descriptor_pool_creation_count{};
  std::uint64_t descriptor_allocation_count{};
  std::uint64_t descriptor_update_count{};
  std::uint64_t bindless_sampled_image_descriptor_update_count{};
  std::uint64_t bindless_sampler_descriptor_update_count{};
  std::uint64_t generated_material_draw_count{};
  std::uint64_t generated_material_fallback_count{};
  std::uint64_t aov_image_export_count{};
  MaterialFallbackEvidence material_fallbacks;
};

struct RenderProductRequest {
  Aov aov{Aov::Color};
  bool cpu_readback{true};

  friend constexpr bool operator==(const RenderProductRequest&,
                                   const RenderProductRequest&) = default;
};

class PresentationTarget {
 public:
  PresentationTarget() = default;
  PresentationTarget(std::uint64_t owner, std::uint64_t value) noexcept
      : owner_(owner), value_(value) {}

  [[nodiscard]] explicit operator bool() const noexcept { return value_ != 0; }
  [[nodiscard]] std::uint64_t owner() const noexcept { return owner_; }
  [[nodiscard]] std::uint64_t value() const noexcept { return value_; }

  friend constexpr bool operator==(const PresentationTarget&,
                                   const PresentationTarget&) = default;

 private:
  std::uint64_t owner_{};
  std::uint64_t value_{};
};

class CompletionToken {
 public:
  CompletionToken() = default;
  CompletionToken(std::uint64_t owner, std::uint64_t value) noexcept
      : owner_(owner), value_(value) {}

  [[nodiscard]] explicit operator bool() const noexcept { return value_ != 0; }
  [[nodiscard]] std::uint64_t owner() const noexcept { return owner_; }
  [[nodiscard]] std::uint64_t value() const noexcept { return value_; }

  friend constexpr bool operator==(const CompletionToken&,
                                   const CompletionToken&) = default;

 private:
  std::uint64_t owner_{};
  std::uint64_t value_{};
};

struct RenderRequest {
  std::shared_ptr<const extraction::FrameSnapshot> snapshot;
  // Optional packed ABI-v1 table update. A backend accepts this only when it
  // was created with matching persistent GPU Scene capacities.
  std::shared_ptr<const GpuScenePackedFrameUpdate> gpu_scene_update;
  std::uint32_t width{512};
  std::uint32_t height{512};
  Vec4 clear_color{kDefaultClearColor};
  std::vector<RenderProductRequest> products{
      {Aov::Color, true}, {Aov::Depth, true}};
  PresentationTarget presentation;
};

struct ImageRgba8 {
  RenderProduct product;
  std::uint32_t row_pitch_bytes{};
  std::vector<std::uint8_t> pixels;
};

struct ImageDepth32 {
  RenderProduct product;
  std::uint32_t row_pitch_bytes{};
  std::vector<float> pixels;
};

struct ImageUint32 {
  RenderProduct product;
  std::uint32_t row_pitch_bytes{};
  std::vector<std::uint32_t> pixels;
};

struct RenderResult {
  ImageRgba8 color;
  ImageDepth32 depth;
  ImageUint32 prim_id;
  ImageUint32 instance_id;
  std::vector<Aov> rendered_aovs;
  std::vector<Aov> cpu_readback_aovs;
  std::uint64_t scene_revision{};
  std::uint64_t completion_value{};
  FrameTimings timings;
  FrameTelemetry telemetry;
  std::vector<MaterialDiagnostic> material_diagnostics;
};

enum class RendererErrorCode {
  InvalidRequest,
  InvalidToken,
  ResourceBusy,
  Timeout,
  DeviceLost,
  Unsupported,
  BackendUnavailable,
  BackendFailure,
  ResourceExhausted,
};

[[nodiscard]] std::string_view RendererErrorCodeName(
    RendererErrorCode code) noexcept;

class RendererError : public std::runtime_error {
 public:
  RendererError(RendererErrorCode code, std::string operation,
                std::string detail, std::int32_t native_code = 0);

  [[nodiscard]] RendererErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& operation() const noexcept {
    return operation_;
  }
  [[nodiscard]] const std::string& detail() const noexcept { return detail_; }
  [[nodiscard]] std::int32_t native_code() const noexcept {
    return native_code_;
  }

 private:
  RendererErrorCode code_;
  std::string operation_;
  std::string detail_;
  std::int32_t native_code_{};
};

class Backend {
 public:
  virtual ~Backend() = default;

  [[nodiscard]] virtual const RendererCapabilities& capabilities()
      const noexcept = 0;
  [[nodiscard]] virtual RendererStatistics statistics() const noexcept = 0;
  [[nodiscard]] virtual std::optional<PresentationTarget>
  default_presentation_target() const noexcept = 0;
  // Declares the host's new presentation extent. Backends may defer the
  // native resize until the next presented RenderRequest, whose width/height
  // remain the authoritative extent for that frame.
  virtual void ResizePresentationTarget(PresentationTarget target,
                                        std::uint32_t width,
                                        std::uint32_t height) = 0;
  [[nodiscard]] virtual CompletionToken Submit(const RenderRequest& request) = 0;
  [[nodiscard]] virtual bool IsComplete(CompletionToken token) const = 0;
  [[nodiscard]] virtual RenderResult Resolve(
      CompletionToken token,
      std::chrono::nanoseconds timeout =
          std::chrono::nanoseconds::max()) = 0;
};

struct BackendCreateInfo {
  BackendRequest backend{BackendRequest::Automatic};
  bool enable_validation{};
  std::uint32_t frames_in_flight{3};
  // Optional host-owned sink for asynchronous backend diagnostics. The sink
  // must outlive the Backend and make Report thread-safe when the native API
  // can invoke diagnostics from worker threads.
  DiagnosticSink* diagnostic_sink{};
};

struct BackendAvailability {
  bool available{};
  std::string detail;
};

class BackendFactory {
 public:
  virtual ~BackendFactory() = default;
  [[nodiscard]] virtual BackendKind kind() const noexcept = 0;
  [[nodiscard]] virtual BackendAvailability availability() const = 0;
  [[nodiscard]] virtual std::unique_ptr<Backend> Create(
      const BackendCreateInfo& info) const = 0;
};

// Selects an explicit backend or the platform preference from the factories
// supplied by the application. Core never owns native device/window state.
[[nodiscard]] std::unique_ptr<Backend> CreateBackend(
    const BackendCreateInfo& info, std::span<BackendFactory* const> factories,
    BackendSelection* selection = nullptr);

}  // namespace merlin::render
