#pragma once

#include <pxr/pxr.h>

#include <pxr/imaging/hd/driver.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/imaging/hgi/texture.h>
#include <pxr/imaging/hgi/types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

namespace merlin::render {
class Backend;
}

namespace merlin::vulkan {
struct AovImageExport;
}

PXR_NAMESPACE_OPEN_SCOPE

enum class HdMerlinHgiVulkanTransferMode {
  CpuReadback,
  GpuCopy,
  DirectSharedResource,
};

// A direct-share rejection is reported separately from fallback_reason:
// rejecting Tier 2 while selecting the supported Tier 1 GPU-copy path is a
// capability decision, not an operational bridge failure.
enum class HdMerlinHgiVulkanDirectShareRejection {
  None,
  NotEvaluated,
  PhysicalDeviceMismatch,
  LogicalDeviceMismatch,
  QueueOwnershipUnsupported,
  ApiIncompatible,
  RequiredExtensionMissing,
  FormatUsageMismatch,
  SampleCountUnsupported,
  TilingUnsupported,
  MemoryConstraintsUnsupported,
  PublicTextureImportUnavailable,
  HostConsumptionUnretained,
  CompletionRetentionUnavailable,
  ResizeRetirementUnsafe,
  DirectPathUnavailable,
};

enum class HdMerlinHgiVulkanFallbackReason {
  None,
  BridgeDisabled,
  MissingRenderDriver,
  NonVulkanRenderDriver,
  UnsupportedOpenUsd,
  GpuCopyUnavailable,
  DriverSwapRejected,
  InvalidTarget,
  TargetCreationFailed,
  TargetUploadFailed,
  NativeContextUnavailable,
  SourceMismatch,
  GpuCopyFailed,
};

[[nodiscard]] std::string_view HdMerlinHgiVulkanTransferModeName(
    HdMerlinHgiVulkanTransferMode mode) noexcept;
[[nodiscard]] std::string_view HdMerlinHgiVulkanDirectShareRejectionName(
    HdMerlinHgiVulkanDirectShareRejection reason) noexcept;
[[nodiscard]] std::string_view HdMerlinHgiVulkanFallbackReasonName(
    HdMerlinHgiVulkanFallbackReason reason) noexcept;

// Every field is affirmative: default construction rejects direct sharing.
// Keeping this evaluator independent of native headers lets package and host
// tests prove that no single same-GPU observation can bypass the remaining
// ownership, compatibility, public-API, or lifetime gates.
struct HdMerlinHgiVulkanDirectShareRequirements {
  bool physical_device_identity{};
  bool logical_device_identity{};
  bool queue_ownership_compatible{};
  bool api_compatible{};
  bool required_extensions_available{};
  bool format_usage_compatible{};
  bool single_sampled{};
  bool tiling_compatible{};
  bool memory_constraints_compatible{};
  bool public_texture_import_available{};
  bool host_consumption_retained{};
  bool completion_retention_available{};
  bool resize_retirement_safe{};
  bool direct_path_available{};
};

struct HdMerlinHgiVulkanDirectShareSupport {
  bool supported{};
  HdMerlinHgiVulkanDirectShareRejection rejection{
      HdMerlinHgiVulkanDirectShareRejection::NotEvaluated};
};

[[nodiscard]] HdMerlinHgiVulkanDirectShareSupport
HdMerlinEvaluateHgiVulkanDirectShare(
    const HdMerlinHgiVulkanDirectShareRequirements& requirements) noexcept;

struct HdMerlinHgiVulkanBridgeStatus {
  std::uint32_t openusd_version{};
  bool enabled{};
  bool render_driver_available{};
  bool vulkan_render_driver{};
  bool hgi_owned_targets{};
  bool gpu_copy{};
  bool direct_shared_resource{};
  HdMerlinHgiVulkanTransferMode selected_mode{
      HdMerlinHgiVulkanTransferMode::CpuReadback};
  HdMerlinHgiVulkanDirectShareRejection direct_share_rejection{
      HdMerlinHgiVulkanDirectShareRejection::NotEvaluated};
  HdMerlinHgiVulkanFallbackReason fallback_reason{
      HdMerlinHgiVulkanFallbackReason::BridgeDisabled};
};

struct HdMerlinHgiVulkanBridgeTelemetry {
  std::uint64_t target_generation{};
  std::uint64_t target_creations{};
  std::uint64_t target_recreations{};
  std::uint64_t target_retirements{};
  // Targets dropped without their creating Hgi being available to retire them.
  // SetDrivers refuses the driver swap that would produce these, so a non-zero
  // count means an ownership assumption broke rather than a texture merely
  // going away.
  std::uint64_t target_orphans{};
  std::uint64_t cpu_upload_count{};
  std::uint64_t cpu_upload_bytes{};
  std::uint64_t cpu_upload_encode_ns{};
  std::uint64_t gpu_copy_count{};
  std::uint64_t gpu_copy_completion_count{};
  std::uint64_t gpu_copy_pending_count{};
  std::uint64_t gpu_copy_bytes{};
  std::uint64_t gpu_copy_encode_ns{};
  std::uint64_t direct_share_evaluation_count{};
  std::uint64_t direct_share_rejection_count{};
  std::uint64_t coarse_wait_count{};
};

struct HdMerlinBorrowedVulkanContext {
  std::uintptr_t instance{};
  std::uintptr_t physical_device{};
  std::uintptr_t device{};
  std::uintptr_t graphics_queue{};
  std::uint32_t graphics_queue_family{};
  std::uint32_t graphics_queue_index{};
  bool timeline_semaphore_enabled{};
  bool validation_enabled{};
  bool debug_utils_enabled{};
};

// Kept separately testable so OpenUSD package composition (not merely its
// version number) remains part of bridge capability selection.
[[nodiscard]] HdMerlinHgiVulkanBridgeStatus
HdMerlinEvaluateHgiVulkanBridgeSupport(bool enabled,
                                       std::uint32_t openusd_version,
                                       bool render_driver_available,
                                       bool vulkan_render_driver) noexcept;

// The one table mapping the RenderBuffer formats this adapter implements onto
// their Hgi equivalents. HgiFormatInvalid means "no Hgi target for this
// format", so a format the CPU path grows later cannot silently acquire a
// texture whose texel size disagrees with the byte size of the buffer feeding
// it.
[[nodiscard]] HgiFormat HdMerlinHgiFormatForRenderBuffer(
    HdFormat format) noexcept;

class Hgi;

// Hydra-adapter-owned Hgi target manager. Tier 0 moves the CPU upload into an
// Hgi-owned texture exposed through HdRenderBuffer::GetResource. Validated
// packages that expose the native HgiVulkan target use the same-device GPU-copy
// path and retain Tier 0 as the operational fallback.
//
// Threading: the internal mutex only serialises this bridge's own state. Hgi
// itself is not thread-safe and is shared with the host, so CreateTarget,
// DestroyTarget, Upload, and Copy must be reached from the Hydra execution thread:
// the thread that runs HdEngine::Execute and on which the host drives the same
// Hgi. Every current caller sits on the render-pass execution path, which
// satisfies that.
class HdMerlinHgiVulkanBridge final
    : public std::enable_shared_from_this<HdMerlinHgiVulkanBridge> {
 public:
  explicit HdMerlinHgiVulkanBridge(bool enabled);
  ~HdMerlinHgiVulkanBridge();

  HdMerlinHgiVulkanBridge(const HdMerlinHgiVulkanBridge&) = delete;
  HdMerlinHgiVulkanBridge& operator=(const HdMerlinHgiVulkanBridge&) = delete;

  // Binds the bridge to the application-owned Hgi. A driver declaration is
  // also the one point at which an operational rejection is re-evaluated, so
  // re-declaring the same driver clears a latched failure. A swap away from an
  // Hgi that still owns published targets is refused instead, because those
  // targets can only be retired by the Hgi that created them.
  void SetDrivers(const HdDriverVector& drivers);

  [[nodiscard]] HdMerlinHgiVulkanBridgeStatus status() const;
  [[nodiscard]] HdMerlinHgiVulkanBridgeTelemetry telemetry() const;
  [[nodiscard]] std::optional<HdMerlinBorrowedVulkanContext>
  BorrowedContext() const;

  [[nodiscard]] HgiTextureHandle CreateTarget(const HgiTextureDesc& descriptor,
                                              bool recreation);
  void DestroyTarget(HgiTextureHandle* target);
  [[nodiscard]] bool Upload(HgiTextureHandle target, const void* data,
                            std::size_t byte_size);
  [[nodiscard]] bool Copy(
      HgiTextureHandle target, merlin::vulkan::AovImageExport&& source,
      std::shared_ptr<merlin::render::Backend> backend);

 private:
  // A one-way latch for the delegate's lifetime, cleared only by a fresh
  // SetDrivers: an operational failure disables Hgi-owned targets for every
  // buffer, not just the one that failed. Retrying per frame would mean
  // re-entering a path the driver has already rejected once.
  void SetOperationalFallbackLocked(
      HdMerlinHgiVulkanFallbackReason reason) noexcept;

  const bool enabled_;
  mutable std::mutex mutex_;
  Hgi* hgi_{};
  std::uint64_t outstanding_targets_{};
  HdMerlinHgiVulkanBridgeStatus status_;
  HdMerlinHgiVulkanBridgeTelemetry telemetry_;
};

PXR_NAMESPACE_CLOSE_SCOPE
