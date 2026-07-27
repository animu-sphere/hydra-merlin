#include "hgi_vulkan_bridge.hpp"

#include <pxr/imaging/hgi/blitCmds.h>
#include <pxr/imaging/hgi/blitCmdsOps.h>
#include <pxr/imaging/hgi/hgi.h>
#include <pxr/imaging/hgi/tokens.h>

#include <chrono>
#include <exception>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t ElapsedNanoseconds(Clock::time_point start) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start)
          .count());
}

constexpr bool IsValidatedOpenUsd(std::uint32_t version) noexcept {
  return version == 2605 || version == 2608;
}

}  // namespace

std::string_view HdMerlinHgiVulkanTransferModeName(
    HdMerlinHgiVulkanTransferMode mode) noexcept {
  switch (mode) {
    case HdMerlinHgiVulkanTransferMode::CpuReadback:
      return "cpu-readback";
    case HdMerlinHgiVulkanTransferMode::GpuCopy:
      return "gpu-copy";
  }
  return "unknown";
}

std::string_view HdMerlinHgiVulkanFallbackReasonName(
    HdMerlinHgiVulkanFallbackReason reason) noexcept {
  switch (reason) {
    case HdMerlinHgiVulkanFallbackReason::None:
      return "none";
    case HdMerlinHgiVulkanFallbackReason::BridgeDisabled:
      return "bridge-disabled";
    case HdMerlinHgiVulkanFallbackReason::MissingRenderDriver:
      return "missing-render-driver";
    case HdMerlinHgiVulkanFallbackReason::NonVulkanRenderDriver:
      return "non-vulkan-render-driver";
    case HdMerlinHgiVulkanFallbackReason::UnsupportedOpenUsd:
      return "unsupported-openusd";
    case HdMerlinHgiVulkanFallbackReason::GpuCopyUnavailable:
      return "gpu-copy-unavailable";
    case HdMerlinHgiVulkanFallbackReason::InvalidTarget:
      return "invalid-target";
    case HdMerlinHgiVulkanFallbackReason::TargetCreationFailed:
      return "target-creation-failed";
    case HdMerlinHgiVulkanFallbackReason::TargetUploadFailed:
      return "target-upload-failed";
  }
  return "unknown";
}

HdMerlinHgiVulkanBridgeStatus HdMerlinEvaluateHgiVulkanBridgeSupport(
    bool enabled, std::uint32_t openusd_version, bool render_driver_available,
    bool vulkan_render_driver) noexcept {
  HdMerlinHgiVulkanBridgeStatus result;
  result.openusd_version = openusd_version;
  result.enabled = enabled;
  result.render_driver_available = render_driver_available;
  result.vulkan_render_driver = vulkan_render_driver;

  if (!enabled) {
    result.fallback_reason =
        HdMerlinHgiVulkanFallbackReason::BridgeDisabled;
  } else if (!IsValidatedOpenUsd(openusd_version)) {
    result.fallback_reason =
        HdMerlinHgiVulkanFallbackReason::UnsupportedOpenUsd;
  } else if (!render_driver_available) {
    result.fallback_reason =
        HdMerlinHgiVulkanFallbackReason::MissingRenderDriver;
  } else if (!vulkan_render_driver) {
    result.fallback_reason =
        HdMerlinHgiVulkanFallbackReason::NonVulkanRenderDriver;
  } else {
    result.hgi_owned_targets = true;
    // The first slice deliberately retains Tier 0. This explicit rejection is
    // replaced only when native source metadata and bridge completion exist.
    result.fallback_reason =
        HdMerlinHgiVulkanFallbackReason::GpuCopyUnavailable;
  }
  return result;
}

HdMerlinHgiVulkanBridge::HdMerlinHgiVulkanBridge(bool enabled)
    : enabled_(enabled),
      status_(HdMerlinEvaluateHgiVulkanBridgeSupport(
          enabled, PXR_VERSION, false, false)) {}

HdMerlinHgiVulkanBridge::~HdMerlinHgiVulkanBridge() = default;

void HdMerlinHgiVulkanBridge::SetDrivers(const HdDriverVector& drivers) {
  std::scoped_lock lock(mutex_);
  hgi_ = nullptr;
  for (const HdDriver* driver : drivers) {
    if (driver != nullptr && driver->name == HgiTokens->renderDriver &&
        driver->driver.IsHolding<Hgi*>()) {
      hgi_ = driver->driver.UncheckedGet<Hgi*>();
      break;
    }
  }
  const bool has_driver = hgi_ != nullptr;
  const bool is_vulkan =
      has_driver && hgi_->GetAPIName() == HgiTokens->Vulkan;
  status_ = HdMerlinEvaluateHgiVulkanBridgeSupport(
      enabled_, PXR_VERSION, has_driver, is_vulkan);
  if (!status_.hgi_owned_targets) {
    hgi_ = nullptr;
  }
}

HdMerlinHgiVulkanBridgeStatus HdMerlinHgiVulkanBridge::status() const {
  std::scoped_lock lock(mutex_);
  return status_;
}

HdMerlinHgiVulkanBridgeTelemetry
HdMerlinHgiVulkanBridge::telemetry() const {
  std::scoped_lock lock(mutex_);
  return telemetry_;
}

HgiTextureHandle HdMerlinHgiVulkanBridge::CreateTarget(
    const HgiTextureDesc& descriptor, bool recreation) {
  std::scoped_lock lock(mutex_);
  if (hgi_ == nullptr || !status_.hgi_owned_targets) {
    // Preserve the capability-level reason selected by SetDrivers.
    return {};
  }
  if (descriptor.format == HgiFormatInvalid ||
      descriptor.dimensions[0] <= 0 || descriptor.dimensions[1] <= 0 ||
      descriptor.dimensions[2] <= 0 ||
      descriptor.sampleCount != HgiSampleCount1) {
    SetOperationalFallbackLocked(
        HdMerlinHgiVulkanFallbackReason::InvalidTarget);
    return {};
  }

  try {
    HgiTextureHandle target = hgi_->CreateTexture(descriptor);
    if (!target) {
      SetOperationalFallbackLocked(
          HdMerlinHgiVulkanFallbackReason::TargetCreationFailed);
      return {};
    }
    ++telemetry_.target_generation;
    ++telemetry_.target_creations;
    telemetry_.target_recreations += recreation ? 1U : 0U;
    return target;
  } catch (const std::exception&) {
    SetOperationalFallbackLocked(
        HdMerlinHgiVulkanFallbackReason::TargetCreationFailed);
    return {};
  }
}

void HdMerlinHgiVulkanBridge::DestroyTarget(HgiTextureHandle* target) {
  if (target == nullptr || !*target) {
    return;
  }
  std::scoped_lock lock(mutex_);
  if (hgi_ != nullptr) {
    hgi_->DestroyTexture(target);
    ++telemetry_.target_retirements;
  } else {
    *target = {};
  }
}

bool HdMerlinHgiVulkanBridge::Upload(HgiTextureHandle target, const void* data,
                                     std::size_t byte_size) {
  std::scoped_lock lock(mutex_);
  if (hgi_ == nullptr || !target || data == nullptr || byte_size == 0) {
    SetOperationalFallbackLocked(
        HdMerlinHgiVulkanFallbackReason::InvalidTarget);
    return false;
  }

  const auto encode_start = Clock::now();
  try {
    HgiBlitCmdsUniquePtr commands = hgi_->CreateBlitCmds();
    if (!commands) {
      SetOperationalFallbackLocked(
          HdMerlinHgiVulkanFallbackReason::TargetUploadFailed);
      return false;
    }
    HgiTextureCpuToGpuOp upload;
    upload.cpuSourceBuffer = data;
    upload.bufferByteSize = byte_size;
    upload.gpuDestinationTexture = target;
    commands->CopyTextureCpuToGpu(upload);
    // Same-Hgi queue ordering makes this upload visible to the later host
    // composite without a queue/device idle wait.
    hgi_->SubmitCmds(commands.get(), HgiSubmitWaitTypeNoWait);
  } catch (const std::exception&) {
    SetOperationalFallbackLocked(
        HdMerlinHgiVulkanFallbackReason::TargetUploadFailed);
    return false;
  }

  ++telemetry_.cpu_upload_count;
  telemetry_.cpu_upload_bytes += byte_size;
  telemetry_.cpu_upload_encode_ns += ElapsedNanoseconds(encode_start);
  return true;
}

void HdMerlinHgiVulkanBridge::SetOperationalFallbackLocked(
    HdMerlinHgiVulkanFallbackReason reason) noexcept {
  status_.hgi_owned_targets = false;
  status_.gpu_copy = false;
  status_.selected_mode = HdMerlinHgiVulkanTransferMode::CpuReadback;
  status_.fallback_reason = reason;
}

PXR_NAMESPACE_CLOSE_SCOPE
