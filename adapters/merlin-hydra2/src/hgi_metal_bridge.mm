#include "hgi_metal_bridge.hpp"

#include <pxr/base/trace/trace.h>
#include <pxr/imaging/hgi/blitCmds.h>
#include <pxr/imaging/hgi/blitCmdsOps.h>
#include <pxr/imaging/hgi/hgi.h>
#include <pxr/imaging/hgi/tokens.h>

#if defined(MERLIN_HYDRA2_HAVE_HGI_METAL_NATIVE)
#include <pxr/imaging/hgiMetal/blitCmds.h>
#include <pxr/imaging/hgiMetal/hgi.h>
#include <pxr/imaging/hgiMetal/texture.h>
#include <Metal/Metal.h>
#include <merlin/metal/backend.hpp>
#endif

#include <chrono>
#include <exception>
#include <stdexcept>

PXR_NAMESPACE_OPEN_SCOPE

namespace {
using Clock = std::chrono::steady_clock;

std::uint64_t ElapsedNanoseconds(Clock::time_point start) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                            start)
          .count());
}

constexpr bool IsValidatedOpenUsd(
    [[maybe_unused]] std::uint32_t version) noexcept {
#ifdef MERLIN_OPENUSD_VALIDATED_PXR_VERSION
  return version == MERLIN_OPENUSD_VALIDATED_PXR_VERSION;
#else
  return true;
#endif
}
}  // namespace

std::string_view HdMerlinHgiMetalTransferModeName(
    HdMerlinHgiMetalTransferMode mode) noexcept {
  switch (mode) {
  case HdMerlinHgiMetalTransferMode::CpuReadback: return "cpu-readback";
  case HdMerlinHgiMetalTransferMode::GpuCopy: return "gpu-copy";
  case HdMerlinHgiMetalTransferMode::DirectSharedResource:
    return "direct-shared-resource";
  }
  return "unknown";
}

std::string_view HdMerlinHgiMetalDirectShareRejectionName(
    HdMerlinHgiMetalDirectShareRejection reason) noexcept {
  switch (reason) {
  case HdMerlinHgiMetalDirectShareRejection::None: return "none";
  case HdMerlinHgiMetalDirectShareRejection::NotEvaluated: return "not-evaluated";
  case HdMerlinHgiMetalDirectShareRejection::DeviceMismatch:
    return "device-mismatch";
  case HdMerlinHgiMetalDirectShareRejection::TextureStorageMismatch:
    return "texture-storage-mismatch";
  case HdMerlinHgiMetalDirectShareRejection::TextureUsageMismatch:
    return "texture-usage-mismatch";
  case HdMerlinHgiMetalDirectShareRejection::PixelFormatMismatch:
    return "pixel-format-mismatch";
  case HdMerlinHgiMetalDirectShareRejection::CommandQueueMismatch:
    return "command-queue-mismatch";
  case HdMerlinHgiMetalDirectShareRejection::CompletionRetentionUnavailable:
    return "completion-retention-unavailable";
  case HdMerlinHgiMetalDirectShareRejection::ResizeRetirementUnsafe:
    return "resize-retirement-unsafe";
  case HdMerlinHgiMetalDirectShareRejection::PublicTextureImportUnavailable:
    return "public-texture-import-unavailable";
  case HdMerlinHgiMetalDirectShareRejection::DirectPathUnavailable:
    return "direct-path-unavailable";
  }
  return "unknown";
}

std::string_view HdMerlinHgiMetalFallbackReasonName(
    HdMerlinHgiMetalFallbackReason reason) noexcept {
  switch (reason) {
  case HdMerlinHgiMetalFallbackReason::None: return "none";
  case HdMerlinHgiMetalFallbackReason::BridgeDisabled: return "bridge-disabled";
  case HdMerlinHgiMetalFallbackReason::MissingRenderDriver:
    return "missing-render-driver";
  case HdMerlinHgiMetalFallbackReason::NonMetalRenderDriver:
    return "non-metal-render-driver";
  case HdMerlinHgiMetalFallbackReason::UnsupportedOpenUsd:
    return "unsupported-openusd";
  case HdMerlinHgiMetalFallbackReason::GpuCopyUnavailable:
    return "gpu-copy-unavailable";
  case HdMerlinHgiMetalFallbackReason::DriverSwapRejected:
    return "driver-swap-rejected";
  case HdMerlinHgiMetalFallbackReason::InvalidTarget: return "invalid-target";
  case HdMerlinHgiMetalFallbackReason::TargetCreationFailed:
    return "target-creation-failed";
  case HdMerlinHgiMetalFallbackReason::TargetUploadFailed:
    return "target-upload-failed";
  case HdMerlinHgiMetalFallbackReason::NativeContextUnavailable:
    return "native-context-unavailable";
  case HdMerlinHgiMetalFallbackReason::SourceMismatch: return "source-mismatch";
  case HdMerlinHgiMetalFallbackReason::GpuCopyFailed: return "gpu-copy-failed";
  }
  return "unknown";
}

HdMerlinHgiMetalBridgeStatus HdMerlinEvaluateHgiMetalBridgeSupport(
    bool enabled, std::uint32_t openusd_version,
    bool render_driver_available, bool metal_render_driver) noexcept {
  HdMerlinHgiMetalBridgeStatus result;
  result.openusd_version = openusd_version;
  result.enabled = enabled;
  result.render_driver_available = render_driver_available;
  result.metal_render_driver = metal_render_driver;
  if (!enabled) {
    result.fallback_reason = HdMerlinHgiMetalFallbackReason::BridgeDisabled;
  } else if (!IsValidatedOpenUsd(openusd_version)) {
    result.fallback_reason = HdMerlinHgiMetalFallbackReason::UnsupportedOpenUsd;
  } else if (!render_driver_available) {
    result.fallback_reason = HdMerlinHgiMetalFallbackReason::MissingRenderDriver;
  } else if (!metal_render_driver) {
    result.fallback_reason = HdMerlinHgiMetalFallbackReason::NonMetalRenderDriver;
  } else {
    result.hgi_owned_targets = true;
    result.fallback_reason = HdMerlinHgiMetalFallbackReason::GpuCopyUnavailable;
  }
  return result;
}

HdMerlinHgiMetalDirectShareSupport HdMerlinEvaluateHgiMetalDirectShare(
    const HdMerlinHgiMetalDirectShareRequirements& r) noexcept {
  using Rejection = HdMerlinHgiMetalDirectShareRejection;
  if (!r.device_identity) return {false, Rejection::DeviceMismatch};
  if (!r.texture_storage_compatible) return {false, Rejection::TextureStorageMismatch};
  if (!r.texture_usage_compatible) return {false, Rejection::TextureUsageMismatch};
  if (!r.pixel_format_compatible) return {false, Rejection::PixelFormatMismatch};
  if (!r.command_queue_compatible) return {false, Rejection::CommandQueueMismatch};
  if (!r.completion_retention_available) return {false, Rejection::CompletionRetentionUnavailable};
  if (!r.resize_retirement_safe) return {false, Rejection::ResizeRetirementUnsafe};
  if (!r.public_texture_import_available) return {false, Rejection::PublicTextureImportUnavailable};
  if (!r.direct_path_available) return {false, Rejection::DirectPathUnavailable};
  return {true, Rejection::None};
}

HgiFormat HdMerlinHgiMetalFormatForRenderBuffer(HdFormat format) noexcept {
  switch (format) {
  case HdFormatUNorm8Vec4: return HgiFormatUNorm8Vec4;
  case HdFormatFloat32: return HgiFormatFloat32;
  case HdFormatInt32: return HgiFormatInt32;
  default: return HgiFormatInvalid;
  }
}

HdMerlinHgiMetalBridge::HdMerlinHgiMetalBridge(bool enabled)
    : enabled_(enabled),
      status_(HdMerlinEvaluateHgiMetalBridgeSupport(
          enabled, PXR_VERSION, false, false)) {}

HdMerlinHgiMetalBridge::~HdMerlinHgiMetalBridge() = default;

void HdMerlinHgiMetalBridge::SetDrivers(const HdDriverVector& drivers) {
  std::scoped_lock lock(mutex_);
  Hgi* discovered = nullptr;
  for (const HdDriver* driver : drivers) {
    if (driver != nullptr && driver->name == HgiTokens->renderDriver &&
        driver->driver.IsHolding<Hgi*>()) {
      discovered = driver->driver.UncheckedGet<Hgi*>();
      break;
    }
  }
  if (discovered != hgi_ && outstanding_targets_ != 0) {
    SetOperationalFallbackLocked(
        HdMerlinHgiMetalFallbackReason::DriverSwapRejected);
    return;
  }
  hgi_ = discovered;
  const bool has_driver = hgi_ != nullptr;
  const bool is_metal = has_driver && hgi_->GetAPIName() == HgiTokens->Metal;
  status_ = HdMerlinEvaluateHgiMetalBridgeSupport(
      enabled_, PXR_VERSION, has_driver, is_metal);
  if (!status_.hgi_owned_targets) {
    hgi_ = nullptr;
    return;
  }
#if defined(MERLIN_HYDRA2_HAVE_HGI_METAL_NATIVE)
  auto* hgi_metal = dynamic_cast<HgiMetal*>(hgi_);
  if (hgi_metal == nullptr || hgi_metal->GetPrimaryDevice() == nil ||
      hgi_metal->GetQueue() == nil) {
    SetOperationalFallbackLocked(
        HdMerlinHgiMetalFallbackReason::NativeContextUnavailable);
    return;
  }
  status_.gpu_copy = true;
  const auto direct = HdMerlinEvaluateHgiMetalDirectShare({
      .device_identity = true,
      .texture_storage_compatible = true,
      .texture_usage_compatible = true,
      .pixel_format_compatible = true,
      .command_queue_compatible = true,
      .completion_retention_available = false,
      .resize_retirement_safe = false,
      .public_texture_import_available = false,
      .direct_path_available = false,
  });
  ++telemetry_.direct_share_evaluation_count;
  telemetry_.direct_share_rejection_count += direct.supported ? 0U : 1U;
  status_.direct_shared_resource = direct.supported;
  status_.direct_share_rejection = direct.rejection;
  status_.selected_mode = direct.supported
      ? HdMerlinHgiMetalTransferMode::DirectSharedResource
      : HdMerlinHgiMetalTransferMode::GpuCopy;
  status_.fallback_reason = HdMerlinHgiMetalFallbackReason::None;
#endif
}

HdMerlinHgiMetalBridgeStatus HdMerlinHgiMetalBridge::status() const {
  std::scoped_lock lock(mutex_);
  return status_;
}

HdMerlinHgiMetalBridgeTelemetry HdMerlinHgiMetalBridge::telemetry() const {
  std::scoped_lock lock(mutex_);
  return telemetry_;
}

HgiTextureHandle HdMerlinHgiMetalBridge::CreateTarget(
    const HgiTextureDesc& descriptor, bool recreation) {
  Hgi* hgi = nullptr;
  {
    std::scoped_lock lock(mutex_);
    if (hgi_ == nullptr || !status_.hgi_owned_targets) return {};
    hgi = hgi_;
  }
  if (descriptor.format == HgiFormatInvalid ||
      descriptor.type != HgiTextureType2D || descriptor.dimensions[0] <= 0 ||
      descriptor.dimensions[1] <= 0 || descriptor.dimensions[2] != 1 ||
      descriptor.layerCount != 1 || descriptor.sampleCount != HgiSampleCount1) {
    std::scoped_lock lock(mutex_);
    SetOperationalFallbackLocked(HdMerlinHgiMetalFallbackReason::InvalidTarget);
    return {};
  }
  try {
    auto target = hgi->CreateTexture(descriptor);
    if (!target) throw std::runtime_error("CreateTexture returned nil");
    std::scoped_lock lock(mutex_);
    ++outstanding_targets_;
    ++telemetry_.target_generation;
    ++telemetry_.target_creations;
    telemetry_.target_recreations += recreation ? 1U : 0U;
    return target;
  } catch (const std::exception&) {
    std::scoped_lock lock(mutex_);
    SetOperationalFallbackLocked(
        HdMerlinHgiMetalFallbackReason::TargetCreationFailed);
    return {};
  }
}

void HdMerlinHgiMetalBridge::DestroyTarget(HgiTextureHandle* target) {
  if (target == nullptr || !*target) return;
  Hgi* hgi = nullptr;
  {
    std::scoped_lock lock(mutex_);
    hgi = hgi_;
  }
  if (hgi != nullptr) {
    hgi->DestroyTexture(target);
    std::scoped_lock lock(mutex_);
    ++telemetry_.target_retirements;
    if (outstanding_targets_ != 0) --outstanding_targets_;
  } else {
    *target = {};
    std::scoped_lock lock(mutex_);
    ++telemetry_.target_orphans;
  }
}

bool HdMerlinHgiMetalBridge::Upload(HgiTextureHandle target, const void* data,
                                    std::size_t byte_size) {
  TRACE_SCOPE("HdMerlinHgiMetalBridge::Upload");
  Hgi* hgi = nullptr;
  {
    std::scoped_lock lock(mutex_);
    if (hgi_ == nullptr || !target || data == nullptr || byte_size == 0) {
      SetOperationalFallbackLocked(
          HdMerlinHgiMetalFallbackReason::InvalidTarget);
      return false;
    }
    hgi = hgi_;
  }
  const auto start = Clock::now();
  try {
    auto commands = hgi->CreateBlitCmds();
    if (!commands) throw std::runtime_error("CreateBlitCmds returned nil");
    HgiTextureCpuToGpuOp upload;
    upload.cpuSourceBuffer = data;
    upload.bufferByteSize = byte_size;
    upload.gpuDestinationTexture = target;
    commands->CopyTextureCpuToGpu(upload);
    hgi->SubmitCmds(commands.get(), HgiSubmitWaitTypeNoWait);
  } catch (const std::exception&) {
    std::scoped_lock lock(mutex_);
    SetOperationalFallbackLocked(
        HdMerlinHgiMetalFallbackReason::TargetUploadFailed);
    return false;
  }
  std::scoped_lock lock(mutex_);
  ++telemetry_.cpu_upload_count;
  telemetry_.cpu_upload_bytes += byte_size;
  telemetry_.cpu_upload_encode_ns += ElapsedNanoseconds(start);
  return true;
}

bool HdMerlinHgiMetalBridge::Copy(
    HgiTextureHandle target, merlin::metal::AovImageExport&& source,
    std::shared_ptr<merlin::render::Backend> backend) {
  TRACE_SCOPE("HdMerlinHgiMetalBridge::Copy");
#if defined(MERLIN_HYDRA2_HAVE_HGI_METAL_NATIVE)
  auto* exporter = backend == nullptr
      ? nullptr
      : dynamic_cast<merlin::metal::AovImageExporter*>(backend.get());
  auto lease = std::make_shared<merlin::metal::AovImageLease>(
      std::move(source.lease));
  const auto release_lease = [&]() noexcept {
    if (exporter == nullptr || !*lease) return;
    try { exporter->ReleaseAovImage(std::move(*lease)); } catch (...) {}
  };
  Hgi* hgi = nullptr;
  {
    std::scoped_lock lock(mutex_);
    if (hgi_ == nullptr || !status_.gpu_copy || !target ||
        exporter == nullptr || !*lease) {
      release_lease();
      SetOperationalFallbackLocked(
          HdMerlinHgiMetalFallbackReason::GpuCopyUnavailable);
      return false;
    }
    hgi = hgi_;
  }
  auto* hgi_metal = dynamic_cast<HgiMetal*>(hgi);
  auto* destination = dynamic_cast<HgiMetalTexture*>(target.Get());
  id<MTLTexture> src = source.texture == 0
      ? nil : (__bridge id<MTLTexture>)(reinterpret_cast<void*>(source.texture));
  id<MTLDevice> device = hgi_metal == nullptr ? nil : hgi_metal->GetPrimaryDevice();
  const auto* desc = destination == nullptr ? nullptr
                                             : &destination->GetDescriptor();
  const bool valid = hgi_metal != nullptr && destination != nullptr &&
      device != nil && hgi_metal->GetQueue() != nil && src != nil &&
      destination->GetTextureId() != nil &&
      destination->GetTextureId().device == device &&
      source.product.aov == merlin::Aov::Color &&
      source.product.width == static_cast<std::uint32_t>(desc->dimensions[0]) &&
      source.product.height == static_cast<std::uint32_t>(desc->dimensions[1]) &&
      source.device == reinterpret_cast<std::uintptr_t>((__bridge void*)device) &&
      source.command_queue != 0 &&
      source.native_format == static_cast<std::uint32_t>(MTLPixelFormatRGBA8Unorm) &&
      (source.native_usage & static_cast<std::uint32_t>(
          MTLTextureUsageRenderTarget)) != 0 &&
      source.native_storage_mode == static_cast<std::uint32_t>(
          src.storageMode) &&
      source.completion_event != 0 &&
      source.renderer_completion == lease->completion_value() &&
      desc->format == HgiFormatUNorm8Vec4 && desc->sampleCount == HgiSampleCount1;
  if (!valid) {
    release_lease();
    std::scoped_lock lock(mutex_);
    SetOperationalFallbackLocked(HdMerlinHgiMetalFallbackReason::SourceMismatch);
    return false;
  }
  const auto start = Clock::now();
  try {
    id<MTLCommandBuffer> command = hgi_metal->GetPrimaryCommandBuffer();
    if (command == nil) throw std::runtime_error("no HgiMetal command buffer");
    id<MTLSharedEvent> completion_event = (__bridge id<MTLSharedEvent>)(
        reinterpret_cast<void*>(source.completion_event));
    [command encodeWaitForEvent:completion_event
                           value:source.renderer_completion];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    if (blit == nil) throw std::runtime_error("no Metal blit encoder");
    const MTLSize size{source.product.width, source.product.height, 1};
    [blit copyFromTexture:src sourceSlice:0 sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:size
               toTexture:destination->GetTextureId() destinationSlice:0
          destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    [command addCompletedHandler:
        [backend = std::move(backend), lease,
         weak_bridge = weak_from_this()](id<MTLCommandBuffer>) {
      if (auto* done = dynamic_cast<merlin::metal::AovImageExporter*>(backend.get());
          done != nullptr && *lease) {
        try { done->ReleaseAovImage(std::move(*lease)); } catch (...) {}
      }
      if (auto bridge = weak_bridge.lock()) {
        std::scoped_lock completion_lock(bridge->mutex_);
        ++bridge->telemetry_.gpu_copy_completion_count;
        if (bridge->telemetry_.gpu_copy_pending_count != 0)
          --bridge->telemetry_.gpu_copy_pending_count;
      }
    }];
    hgi_metal->SetHasWork();
    hgi_metal->CommitPrimaryCommandBuffer(
        HgiMetal::CommitCommandBuffer_NoWait, true);
  } catch (...) {
    release_lease();
    std::scoped_lock lock(mutex_);
    SetOperationalFallbackLocked(HdMerlinHgiMetalFallbackReason::GpuCopyFailed);
    return false;
  }
  std::scoped_lock lock(mutex_);
  ++telemetry_.gpu_copy_count;
  ++telemetry_.gpu_copy_pending_count;
  telemetry_.gpu_copy_bytes += static_cast<std::uint64_t>(
      source.product.width) * source.product.height * 4U;
  telemetry_.gpu_copy_encode_ns += ElapsedNanoseconds(start);
  return true;
#else
  (void)target; (void)source; (void)backend;
  std::scoped_lock lock(mutex_);
  SetOperationalFallbackLocked(HdMerlinHgiMetalFallbackReason::GpuCopyUnavailable);
  return false;
#endif
}

void HdMerlinHgiMetalBridge::SetOperationalFallbackLocked(
    HdMerlinHgiMetalFallbackReason reason) noexcept {
  status_.hgi_owned_targets = false;
  status_.gpu_copy = false;
  status_.direct_shared_resource = false;
  status_.selected_mode = HdMerlinHgiMetalTransferMode::CpuReadback;
  status_.fallback_reason = reason;
}

PXR_NAMESPACE_CLOSE_SCOPE
