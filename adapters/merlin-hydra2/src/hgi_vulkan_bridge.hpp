#pragma once

#include <pxr/pxr.h>

#include <pxr/imaging/hd/driver.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hgi/texture.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>

PXR_NAMESPACE_OPEN_SCOPE

enum class HdMerlinHgiVulkanTransferMode {
  CpuReadback,
  GpuCopy,
};

enum class HdMerlinHgiVulkanFallbackReason {
  None,
  BridgeDisabled,
  MissingRenderDriver,
  NonVulkanRenderDriver,
  UnsupportedOpenUsd,
  GpuCopyUnavailable,
  InvalidTarget,
  TargetCreationFailed,
  TargetUploadFailed,
};

[[nodiscard]] std::string_view HdMerlinHgiVulkanTransferModeName(
    HdMerlinHgiVulkanTransferMode mode) noexcept;
[[nodiscard]] std::string_view HdMerlinHgiVulkanFallbackReasonName(
    HdMerlinHgiVulkanFallbackReason reason) noexcept;

struct HdMerlinHgiVulkanBridgeStatus {
  std::uint32_t openusd_version{};
  bool enabled{};
  bool render_driver_available{};
  bool vulkan_render_driver{};
  bool hgi_owned_targets{};
  bool gpu_copy{};
  HdMerlinHgiVulkanTransferMode selected_mode{
      HdMerlinHgiVulkanTransferMode::CpuReadback};
  HdMerlinHgiVulkanFallbackReason fallback_reason{
      HdMerlinHgiVulkanFallbackReason::BridgeDisabled};
};

struct HdMerlinHgiVulkanBridgeTelemetry {
  std::uint64_t target_generation{};
  std::uint64_t target_creations{};
  std::uint64_t target_recreations{};
  std::uint64_t target_retirements{};
  std::uint64_t cpu_upload_count{};
  std::uint64_t cpu_upload_bytes{};
  std::uint64_t cpu_upload_encode_ns{};
  std::uint64_t gpu_copy_count{};
  std::uint64_t gpu_copy_bytes{};
  std::uint64_t gpu_copy_encode_ns{};
  std::uint64_t coarse_wait_count{};
};

// Kept separately testable so OpenUSD package composition (not merely its
// version number) remains part of bridge capability selection.
[[nodiscard]] HdMerlinHgiVulkanBridgeStatus
HdMerlinEvaluateHgiVulkanBridgeSupport(bool enabled,
                                       std::uint32_t openusd_version,
                                       bool render_driver_available,
                                       bool vulkan_render_driver) noexcept;

class Hgi;

// Hydra-adapter-owned Hgi target manager. This first v0.13.0 slice moves the
// Tier 0 CPU upload into an Hgi-owned texture and exposes that texture through
// HdRenderBuffer::GetResource. The selected mode remains CpuReadback until the
// renderer image export and distinct GPU-copy completion path are connected.
class HdMerlinHgiVulkanBridge final {
 public:
  explicit HdMerlinHgiVulkanBridge(bool enabled);
  ~HdMerlinHgiVulkanBridge();

  HdMerlinHgiVulkanBridge(const HdMerlinHgiVulkanBridge&) = delete;
  HdMerlinHgiVulkanBridge& operator=(const HdMerlinHgiVulkanBridge&) = delete;

  void SetDrivers(const HdDriverVector& drivers);

  [[nodiscard]] HdMerlinHgiVulkanBridgeStatus status() const;
  [[nodiscard]] HdMerlinHgiVulkanBridgeTelemetry telemetry() const;

  [[nodiscard]] HgiTextureHandle CreateTarget(const HgiTextureDesc& descriptor,
                                              bool recreation);
  void DestroyTarget(HgiTextureHandle* target);
  [[nodiscard]] bool Upload(HgiTextureHandle target, const void* data,
                            std::size_t byte_size);

 private:
  void SetOperationalFallbackLocked(
      HdMerlinHgiVulkanFallbackReason reason) noexcept;

  const bool enabled_;
  mutable std::mutex mutex_;
  Hgi* hgi_{};
  HdMerlinHgiVulkanBridgeStatus status_;
  HdMerlinHgiVulkanBridgeTelemetry telemetry_;
};

PXR_NAMESPACE_CLOSE_SCOPE
