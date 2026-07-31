#include "hgi_vulkan_bridge.hpp"

#include <pxr/imaging/hgi/blitCmds.h>
#include <pxr/imaging/hgi/blitCmdsOps.h>
#include <pxr/imaging/hgi/hgi.h>
#include <pxr/imaging/hgi/tokens.h>
#include <pxr/base/trace/trace.h>

#if defined(MERLIN_HYDRA2_HAVE_HGI_VULKAN_NATIVE)
#include <pxr/imaging/hgiVulkan/blitCmds.h>
#include <pxr/imaging/hgiVulkan/capabilities.h>
#include <pxr/imaging/hgiVulkan/commandBuffer.h>
#include <pxr/imaging/hgiVulkan/commandQueue.h>
#include <pxr/imaging/hgiVulkan/device.h>
#include <pxr/imaging/hgiVulkan/diagnostic.h>
#include <pxr/imaging/hgiVulkan/hgi.h>
#include <pxr/imaging/hgiVulkan/instance.h>
#include <pxr/imaging/hgiVulkan/texture.h>

#include <merlin/vulkan/backend.hpp>
#endif

#include <chrono>
#include <exception>
#include <type_traits>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t ElapsedNanoseconds(Clock::time_point start) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start)
          .count());
}

#if defined(MERLIN_HYDRA2_HAVE_HGI_VULKAN_NATIVE)
template <class Handle>
std::uintptr_t EncodeHandle(Handle handle) noexcept {
  if constexpr (std::is_pointer_v<Handle>) {
    return reinterpret_cast<std::uintptr_t>(handle);
  } else {
    return static_cast<std::uintptr_t>(handle);
  }
}

template <class Handle>
Handle DecodeHandle(std::uintptr_t handle) noexcept {
  if constexpr (std::is_pointer_v<Handle>) {
    return reinterpret_cast<Handle>(handle);
  } else {
    return static_cast<Handle>(handle);
  }
}
#endif

// The validated OpenUSD line is chosen once, by MERLIN_OPENUSD_VALIDATED_
// VERSIONS in the top-level CMakeLists, and the adapter rejects a header
// mismatch at compile time. Deriving the check from that single configured
// value keeps a version bump from silently degrading the bridge to CPU
// fallback the way a duplicated literal list would; capability itself still
// comes from runtime driver detection, never from the version.
constexpr bool IsValidatedOpenUsd(
    [[maybe_unused]] std::uint32_t version) noexcept {
#ifdef MERLIN_OPENUSD_VALIDATED_PXR_VERSION
  return version == MERLIN_OPENUSD_VALIDATED_PXR_VERSION;
#else
  return true;
#endif
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
    case HdMerlinHgiVulkanFallbackReason::DriverSwapRejected:
      return "driver-swap-rejected";
    case HdMerlinHgiVulkanFallbackReason::InvalidTarget:
      return "invalid-target";
    case HdMerlinHgiVulkanFallbackReason::TargetCreationFailed:
      return "target-creation-failed";
    case HdMerlinHgiVulkanFallbackReason::TargetUploadFailed:
      return "target-upload-failed";
    case HdMerlinHgiVulkanFallbackReason::NativeContextUnavailable:
      return "native-context-unavailable";
    case HdMerlinHgiVulkanFallbackReason::SourceMismatch:
      return "source-mismatch";
    case HdMerlinHgiVulkanFallbackReason::GpuCopyFailed:
      return "gpu-copy-failed";
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

HgiFormat HdMerlinHgiFormatForRenderBuffer(HdFormat format) noexcept {
  switch (format) {
    case HdFormatUNorm8Vec4:
      return HgiFormatUNorm8Vec4;
    case HdFormatFloat32:
      return HgiFormatFloat32;
    case HdFormatInt32:
      return HgiFormatInt32;
    default:
      return HgiFormatInvalid;
  }
}

HdMerlinHgiVulkanBridge::HdMerlinHgiVulkanBridge(bool enabled)
    : enabled_(enabled),
      status_(HdMerlinEvaluateHgiVulkanBridgeSupport(
          enabled, PXR_VERSION, false, false)) {}

HdMerlinHgiVulkanBridge::~HdMerlinHgiVulkanBridge() = default;

void HdMerlinHgiVulkanBridge::SetDrivers(const HdDriverVector& drivers) {
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
    // Published targets can only be retired by the Hgi that created them.
    // Rather than leak them or hand them to another device, keep the creating
    // Hgi for retirement and stop publishing new targets.
    SetOperationalFallbackLocked(
        HdMerlinHgiVulkanFallbackReason::DriverSwapRejected);
    return;
  }
  hgi_ = discovered;
  const bool has_driver = hgi_ != nullptr;
  const bool is_vulkan =
      has_driver && hgi_->GetAPIName() == HgiTokens->Vulkan;
  status_ = HdMerlinEvaluateHgiVulkanBridgeSupport(
      enabled_, PXR_VERSION, has_driver, is_vulkan);
  if (!status_.hgi_owned_targets) {
    hgi_ = nullptr;
    return;
  }
#if defined(MERLIN_HYDRA2_HAVE_HGI_VULKAN_NATIVE)
  auto* hgi_vulkan = dynamic_cast<HgiVulkan*>(hgi_);
  HgiVulkanDevice* device =
      hgi_vulkan == nullptr ? nullptr : hgi_vulkan->GetPrimaryDevice();
  HgiVulkanInstance* instance =
      hgi_vulkan == nullptr ? nullptr : hgi_vulkan->GetVulkanInstance();
  HgiVulkanCommandQueue* queue =
      device == nullptr ? nullptr : device->GetCommandQueue();
  if (instance == nullptr || device == nullptr || queue == nullptr ||
      !instance->GetVulkanInstance() || !device->GetVulkanPhysicalDevice() ||
      !device->GetVulkanDevice() || !queue->GetVulkanGraphicsQueue()) {
    SetOperationalFallbackLocked(
        HdMerlinHgiVulkanFallbackReason::NativeContextUnavailable);
    return;
  }
  status_.gpu_copy = true;
  status_.selected_mode = HdMerlinHgiVulkanTransferMode::GpuCopy;
  status_.fallback_reason = HdMerlinHgiVulkanFallbackReason::None;
#endif
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

std::optional<HdMerlinBorrowedVulkanContext>
HdMerlinHgiVulkanBridge::BorrowedContext() const {
  std::scoped_lock lock(mutex_);
#if defined(MERLIN_HYDRA2_HAVE_HGI_VULKAN_NATIVE)
  if (hgi_ == nullptr || !status_.gpu_copy) {
    return std::nullopt;
  }
  auto* hgi_vulkan = dynamic_cast<HgiVulkan*>(hgi_);
  HgiVulkanDevice* device =
      hgi_vulkan == nullptr ? nullptr : hgi_vulkan->GetPrimaryDevice();
  HgiVulkanInstance* instance =
      hgi_vulkan == nullptr ? nullptr : hgi_vulkan->GetVulkanInstance();
  HgiVulkanCommandQueue* queue =
      device == nullptr ? nullptr : device->GetCommandQueue();
  if (instance == nullptr || device == nullptr || queue == nullptr) {
    return std::nullopt;
  }
  const HgiVulkanCapabilities* capabilities = hgi_vulkan->GetCapabilities();
  HdMerlinBorrowedVulkanContext result;
  result.instance = EncodeHandle(instance->GetVulkanInstance());
  result.physical_device = EncodeHandle(device->GetVulkanPhysicalDevice());
  result.device = EncodeHandle(device->GetVulkanDevice());
  result.graphics_queue = EncodeHandle(queue->GetVulkanGraphicsQueue());
  result.graphics_queue_family = device->GetGfxQueueFamilyIndex();
  result.graphics_queue_index = 0;
  result.timeline_semaphore_enabled =
      capabilities != nullptr &&
      capabilities->vkVulkan12Features.timelineSemaphore == VK_TRUE;
  result.validation_enabled = HgiVulkanIsValidationEnabled();
  result.debug_utils_enabled =
      instance->vkCreateDebugUtilsMessengerEXT != nullptr;
  return result;
#else
  return std::nullopt;
#endif
}

HgiTextureHandle HdMerlinHgiVulkanBridge::CreateTarget(
    const HgiTextureDesc& descriptor, bool recreation) {
  Hgi* hgi = nullptr;
  {
    std::scoped_lock lock(mutex_);
    if (hgi_ == nullptr || !status_.hgi_owned_targets) {
      // Preserve the capability-level reason selected by SetDrivers.
      return {};
    }
    hgi = hgi_;
  }
  // Only single-sampled 2D targets are published, so the descriptor has to be
  // exactly one texel deep: a HgiTextureType2D image with a deeper extent is
  // invalid in Vulkan rather than merely unused.
  if (descriptor.format == HgiFormatInvalid ||
      descriptor.type != HgiTextureType2D ||
      descriptor.dimensions[0] <= 0 || descriptor.dimensions[1] <= 0 ||
      descriptor.dimensions[2] != 1 || descriptor.layerCount != 1 ||
      descriptor.sampleCount != HgiSampleCount1) {
    std::scoped_lock lock(mutex_);
    SetOperationalFallbackLocked(
        HdMerlinHgiVulkanFallbackReason::InvalidTarget);
    return {};
  }

  try {
    HgiTextureHandle target = hgi->CreateTexture(descriptor);
    if (!target) {
      std::scoped_lock lock(mutex_);
      SetOperationalFallbackLocked(
          HdMerlinHgiVulkanFallbackReason::TargetCreationFailed);
      return {};
    }
    std::scoped_lock lock(mutex_);
    ++outstanding_targets_;
    ++telemetry_.target_generation;
    ++telemetry_.target_creations;
    telemetry_.target_recreations += recreation ? 1U : 0U;
    return target;
  } catch (const std::exception&) {
    std::scoped_lock lock(mutex_);
    SetOperationalFallbackLocked(
        HdMerlinHgiVulkanFallbackReason::TargetCreationFailed);
    return {};
  }
}

void HdMerlinHgiVulkanBridge::DestroyTarget(HgiTextureHandle* target) {
  if (target == nullptr || !*target) {
    return;
  }
  Hgi* hgi = nullptr;
  {
    std::scoped_lock lock(mutex_);
    hgi = hgi_;
  }
  if (hgi != nullptr) {
    hgi->DestroyTexture(target);
    std::scoped_lock lock(mutex_);
    ++telemetry_.target_retirements;
  } else {
    // SetDrivers refuses to drop an Hgi that still owns targets, so this is
    // unreachable; dropping the handle would leak the texture, so make that
    // visible rather than silent.
    *target = {};
    std::scoped_lock lock(mutex_);
    ++telemetry_.target_orphans;
  }
  std::scoped_lock lock(mutex_);
  if (outstanding_targets_ != 0) {
    --outstanding_targets_;
  }
}

bool HdMerlinHgiVulkanBridge::Upload(HgiTextureHandle target, const void* data,
                                     std::size_t byte_size) {
  TRACE_SCOPE("HdMerlinHgiVulkanBridge::Upload");
  Hgi* hgi = nullptr;
  {
    std::scoped_lock lock(mutex_);
    if (hgi_ == nullptr || !target || data == nullptr || byte_size == 0) {
      SetOperationalFallbackLocked(
          HdMerlinHgiVulkanFallbackReason::InvalidTarget);
      return false;
    }
    hgi = hgi_;
  }

  const auto encode_start = Clock::now();
  try {
    HgiBlitCmdsUniquePtr commands = hgi->CreateBlitCmds();
    if (!commands) {
      std::scoped_lock lock(mutex_);
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
    hgi->SubmitCmds(commands.get(), HgiSubmitWaitTypeNoWait);
  } catch (const std::exception&) {
    std::scoped_lock lock(mutex_);
    SetOperationalFallbackLocked(
        HdMerlinHgiVulkanFallbackReason::TargetUploadFailed);
    return false;
  }

  std::scoped_lock lock(mutex_);
  ++telemetry_.cpu_upload_count;
  telemetry_.cpu_upload_bytes += byte_size;
  telemetry_.cpu_upload_encode_ns += ElapsedNanoseconds(encode_start);
  return true;
}

bool HdMerlinHgiVulkanBridge::Copy(
    HgiTextureHandle target, merlin::vulkan::AovImageExport&& source,
    std::shared_ptr<merlin::render::Backend> backend) {
  TRACE_SCOPE("HdMerlinHgiVulkanBridge::Copy");
#if defined(MERLIN_HYDRA2_HAVE_HGI_VULKAN_NATIVE)
  auto* exporter = backend == nullptr
                       ? nullptr
                       : dynamic_cast<merlin::vulkan::AovImageExporter*>(
                             backend.get());
  auto lease = std::make_shared<merlin::vulkan::AovImageLease>(
      std::move(source.lease));
  const auto release_lease = [&]() noexcept {
    if (exporter == nullptr || !*lease) {
      return;
    }
    try {
      exporter->ReleaseAovImage(std::move(*lease));
    } catch (const std::exception&) {
    }
  };

  Hgi* hgi = nullptr;
  {
    std::scoped_lock lock(mutex_);
    if (hgi_ == nullptr || !status_.gpu_copy || !target ||
        exporter == nullptr || !*lease) {
      release_lease();
      SetOperationalFallbackLocked(
          HdMerlinHgiVulkanFallbackReason::GpuCopyUnavailable);
      return false;
    }
    hgi = hgi_;
  }

  const auto set_fallback = [&](HdMerlinHgiVulkanFallbackReason reason) {
    std::scoped_lock lock(mutex_);
    SetOperationalFallbackLocked(reason);
  };
  auto* hgi_vulkan = dynamic_cast<HgiVulkan*>(hgi);
  auto* destination = dynamic_cast<HgiVulkanTexture*>(target.Get());
  HgiVulkanDevice* device =
      hgi_vulkan == nullptr ? nullptr : hgi_vulkan->GetPrimaryDevice();
  const HgiTextureDesc* destination_descriptor =
      destination == nullptr ? nullptr : &destination->GetDescriptor();
  const bool valid_source =
      device != nullptr && destination != nullptr &&
      destination->GetDevice() == device &&
      source.product.aov == merlin::Aov::Color &&
      source.product.width == static_cast<std::uint32_t>(
                                  destination_descriptor->dimensions[0]) &&
      source.product.height == static_cast<std::uint32_t>(
                                   destination_descriptor->dimensions[1]) &&
      source.physical_device ==
          EncodeHandle(device->GetVulkanPhysicalDevice()) &&
      source.device == EncodeHandle(device->GetVulkanDevice()) &&
      source.image != 0 &&
      source.native_format ==
          static_cast<std::uint32_t>(VK_FORMAT_R8G8B8A8_UNORM) &&
      source.native_layout == static_cast<std::uint32_t>(
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) &&
      source.native_stage_mask ==
          static_cast<std::uint32_t>(VK_PIPELINE_STAGE_TRANSFER_BIT) &&
      source.native_access_mask ==
          static_cast<std::uint32_t>(VK_ACCESS_TRANSFER_READ_BIT) &&
      source.native_aspect_mask ==
          static_cast<std::uint32_t>(VK_IMAGE_ASPECT_COLOR_BIT) &&
      source.queue_family == device->GetGfxQueueFamilyIndex() &&
      source.renderer_completion != 0 &&
      source.renderer_completion == lease->completion_value() &&
      source.sample_count == 1 &&
      destination_descriptor->format == HgiFormatUNorm8Vec4 &&
      destination_descriptor->sampleCount == HgiSampleCount1;
  if (!valid_source) {
    release_lease();
    set_fallback(HdMerlinHgiVulkanFallbackReason::SourceMismatch);
    return false;
  }

  const auto encode_start = Clock::now();
  try {
    HgiBlitCmdsUniquePtr commands = hgi->CreateBlitCmds();
    auto* vulkan_commands = commands == nullptr
                                ? nullptr
                                : dynamic_cast<HgiVulkanBlitCmds*>(
                                      commands.get());
    if (vulkan_commands == nullptr) {
      release_lease();
      set_fallback(HdMerlinHgiVulkanFallbackReason::GpuCopyFailed);
      return false;
    }
    commands->PushDebugGroup("Merlin HgiVulkan GPU copy");
    HgiVulkanCommandBuffer* command_buffer =
        vulkan_commands->GetCommandBuffer();
    if (command_buffer == nullptr) {
      release_lease();
      set_fallback(HdMerlinHgiVulkanFallbackReason::GpuCopyFailed);
      return false;
    }

    const VkImageLayout destination_layout = destination->GetImageLayout();
    const VkAccessFlags destination_access =
        HgiVulkanTexture::GetDefaultAccessFlags(destination_descriptor->usage);
    destination->LayoutBarrier(
        command_buffer, destination_layout,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, destination_access,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImageCopy region{};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.layerCount = 1;
    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.dstSubresource.layerCount = 1;
    region.extent = {source.product.width, source.product.height, 1};
    vkCmdCopyImage(command_buffer->GetVulkanCommandBuffer(),
                   DecodeHandle<VkImage>(source.image),
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   destination->GetImage(),
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    destination->LayoutBarrier(
        command_buffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        destination_layout, VK_ACCESS_TRANSFER_WRITE_BIT,
        destination_access, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT);
    commands->PopDebugGroup();

    const auto weak_bridge = weak_from_this();
    command_buffer->AddCompletedHandler(
        [backend = std::move(backend), lease,
         weak_bridge]() mutable noexcept {
          if (auto* completed_exporter =
                  dynamic_cast<merlin::vulkan::AovImageExporter*>(
                      backend.get());
              completed_exporter != nullptr && *lease) {
            try {
              completed_exporter->ReleaseAovImage(std::move(*lease));
            } catch (const std::exception&) {
            }
          }
          if (const auto bridge = weak_bridge.lock()) {
            std::scoped_lock completion_lock(bridge->mutex_);
            ++bridge->telemetry_.gpu_copy_completion_count;
            if (bridge->telemetry_.gpu_copy_pending_count != 0) {
              --bridge->telemetry_.gpu_copy_pending_count;
            }
          }
        });
    hgi->SubmitCmds(commands.get(), HgiSubmitWaitTypeNoWait);
  } catch (const std::exception&) {
    release_lease();
    set_fallback(HdMerlinHgiVulkanFallbackReason::GpuCopyFailed);
    return false;
  }

  std::scoped_lock lock(mutex_);
  ++telemetry_.gpu_copy_count;
  ++telemetry_.gpu_copy_pending_count;
  telemetry_.gpu_copy_bytes +=
      static_cast<std::uint64_t>(source.product.width) *
      source.product.height * 4U;
  telemetry_.gpu_copy_encode_ns += ElapsedNanoseconds(encode_start);
  return true;
#else
  (void)target;
  (void)source;
  (void)backend;
  std::scoped_lock lock(mutex_);
  SetOperationalFallbackLocked(
      HdMerlinHgiVulkanFallbackReason::GpuCopyUnavailable);
  return false;
#endif
}

void HdMerlinHgiVulkanBridge::SetOperationalFallbackLocked(
    HdMerlinHgiVulkanFallbackReason reason) noexcept {
  status_.hgi_owned_targets = false;
  status_.gpu_copy = false;
  status_.selected_mode = HdMerlinHgiVulkanTransferMode::CpuReadback;
  status_.fallback_reason = reason;
}

PXR_NAMESPACE_CLOSE_SCOPE
