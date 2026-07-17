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

#include <merlin/core/render_product.hpp>
#include <merlin/extraction/frame_snapshot.hpp>

namespace merlin::render {

inline constexpr std::uint32_t kBackendContractVersion = 1;

enum class BackendKind { Vulkan, Metal };
enum class BackendRequest { Automatic, Vulkan, Metal };

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
  RendererLimits limits;
};

struct RendererStatistics {
  std::uint64_t frames_submitted{};
  std::uint64_t frames_presented{};
  std::uint64_t presentation_recreates{};
  std::uint64_t validation_messages{};
  std::uint64_t uploaded_bytes{};
  std::uint64_t readback_bytes{};
  std::uint64_t presentation_copy_bytes{};
};

struct FrameTimings {
  std::uint64_t upload_ns{};
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
  std::uint32_t width{512};
  std::uint32_t height{512};
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
