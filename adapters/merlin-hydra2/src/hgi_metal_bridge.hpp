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

namespace merlin::metal {
struct AovImageExport;
}

namespace merlin::render {
class Backend;
}

PXR_NAMESPACE_OPEN_SCOPE

enum class HdMerlinHgiMetalTransferMode {
  CpuReadback,
  GpuCopy,
  DirectSharedResource,
};

enum class HdMerlinHgiMetalDirectShareRejection {
  None,
  NotEvaluated,
  DeviceMismatch,
  TextureStorageMismatch,
  TextureUsageMismatch,
  PixelFormatMismatch,
  CommandQueueMismatch,
  CompletionRetentionUnavailable,
  ResizeRetirementUnsafe,
  PublicTextureImportUnavailable,
  DirectPathUnavailable,
};

enum class HdMerlinHgiMetalFallbackReason {
  None,
  BridgeDisabled,
  MissingRenderDriver,
  NonMetalRenderDriver,
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

[[nodiscard]] std::string_view HdMerlinHgiMetalTransferModeName(
    HdMerlinHgiMetalTransferMode mode) noexcept;
[[nodiscard]] std::string_view HdMerlinHgiMetalDirectShareRejectionName(
    HdMerlinHgiMetalDirectShareRejection reason) noexcept;
[[nodiscard]] std::string_view HdMerlinHgiMetalFallbackReasonName(
    HdMerlinHgiMetalFallbackReason reason) noexcept;

struct HdMerlinHgiMetalDirectShareRequirements {
  bool device_identity{};
  bool texture_storage_compatible{};
  bool texture_usage_compatible{};
  bool pixel_format_compatible{};
  bool command_queue_compatible{};
  bool completion_retention_available{};
  bool resize_retirement_safe{};
  bool public_texture_import_available{};
  bool direct_path_available{};
};

struct HdMerlinHgiMetalDirectShareSupport {
  bool supported{};
  HdMerlinHgiMetalDirectShareRejection rejection{
      HdMerlinHgiMetalDirectShareRejection::NotEvaluated};
};

[[nodiscard]] HdMerlinHgiMetalDirectShareSupport
HdMerlinEvaluateHgiMetalDirectShare(
    const HdMerlinHgiMetalDirectShareRequirements& requirements) noexcept;

struct HdMerlinHgiMetalBridgeStatus {
  std::uint32_t openusd_version{};
  bool enabled{};
  bool render_driver_available{};
  bool metal_render_driver{};
  bool hgi_owned_targets{};
  bool gpu_copy{};
  bool direct_shared_resource{};
  HdMerlinHgiMetalTransferMode selected_mode{
      HdMerlinHgiMetalTransferMode::CpuReadback};
  HdMerlinHgiMetalDirectShareRejection direct_share_rejection{
      HdMerlinHgiMetalDirectShareRejection::NotEvaluated};
  HdMerlinHgiMetalFallbackReason fallback_reason{
      HdMerlinHgiMetalFallbackReason::BridgeDisabled};
};

struct HdMerlinHgiMetalBridgeTelemetry {
  std::uint64_t target_generation{};
  std::uint64_t target_creations{};
  std::uint64_t target_recreations{};
  std::uint64_t target_retirements{};
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

[[nodiscard]] HdMerlinHgiMetalBridgeStatus HdMerlinEvaluateHgiMetalBridgeSupport(
    bool enabled, std::uint32_t openusd_version,
    bool render_driver_available, bool metal_render_driver) noexcept;

[[nodiscard]] HgiFormat HdMerlinHgiMetalFormatForRenderBuffer(
    HdFormat format) noexcept;

class Hgi;

class HdMerlinHgiMetalBridge final
    : public std::enable_shared_from_this<HdMerlinHgiMetalBridge> {
 public:
  explicit HdMerlinHgiMetalBridge(bool enabled);
  ~HdMerlinHgiMetalBridge();

  HdMerlinHgiMetalBridge(const HdMerlinHgiMetalBridge&) = delete;
  HdMerlinHgiMetalBridge& operator=(const HdMerlinHgiMetalBridge&) = delete;

  void SetDrivers(const HdDriverVector& drivers);
  [[nodiscard]] HdMerlinHgiMetalBridgeStatus status() const;
  [[nodiscard]] HdMerlinHgiMetalBridgeTelemetry telemetry() const;
  [[nodiscard]] HgiTextureHandle CreateTarget(const HgiTextureDesc& descriptor,
                                              bool recreation);
  void DestroyTarget(HgiTextureHandle* target);
  [[nodiscard]] bool Upload(HgiTextureHandle target, const void* data,
                            std::size_t byte_size);
  [[nodiscard]] bool Copy(
      HgiTextureHandle target, merlin::metal::AovImageExport&& source,
      std::shared_ptr<merlin::render::Backend> backend);

 private:
  void SetOperationalFallbackLocked(
      HdMerlinHgiMetalFallbackReason reason) noexcept;

  const bool enabled_;
  mutable std::mutex mutex_;
  Hgi* hgi_{};
  std::uint64_t outstanding_targets_{};
  HdMerlinHgiMetalBridgeStatus status_;
  HdMerlinHgiMetalBridgeTelemetry telemetry_;
};

PXR_NAMESPACE_CLOSE_SCOPE
