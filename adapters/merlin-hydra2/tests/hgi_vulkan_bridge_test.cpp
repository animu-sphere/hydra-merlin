#include "hgi_vulkan_bridge.hpp"

#include <pxr/pxr.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/driver.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/imaging/hgi/enums.h>
#include <pxr/imaging/hgi/hgi.h>
#include <pxr/imaging/hgi/tokens.h>
#include <pxr/imaging/hgi/types.h>

#include <iostream>
#include <string_view>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

int g_failures = 0;

// Every expectation is evaluated so one broken rejection does not hide the
// state of the others.
void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++g_failures;
  }
}

// Drives SetDrivers with a driver vector and reports the resulting rejection.
HdMerlinHgiVulkanFallbackReason RejectionFor(const HdDriverVector& drivers) {
  HdMerlinHgiVulkanBridge bridge(true);
  bridge.SetDrivers(drivers);
  return bridge.status().fallback_reason;
}

}  // namespace

int main() {
  const auto disabled =
      HdMerlinEvaluateHgiVulkanBridgeSupport(false, PXR_VERSION, true, true);
  Expect(!disabled.hgi_owned_targets &&
             disabled.fallback_reason ==
                 HdMerlinHgiVulkanFallbackReason::BridgeDisabled,
         "disabled bridge did not retain Tier 0");

  const auto missing =
      HdMerlinEvaluateHgiVulkanBridgeSupport(true, PXR_VERSION, false, false);
  Expect(!missing.hgi_owned_targets &&
             missing.fallback_reason ==
                 HdMerlinHgiVulkanFallbackReason::MissingRenderDriver,
         "missing Hgi driver was not rejected");

  const auto non_vulkan =
      HdMerlinEvaluateHgiVulkanBridgeSupport(true, PXR_VERSION, true, false);
  Expect(!non_vulkan.hgi_owned_targets &&
             non_vulkan.fallback_reason ==
                 HdMerlinHgiVulkanFallbackReason::NonVulkanRenderDriver,
         "non-Vulkan Hgi driver was not rejected");

  const auto vulkan =
      HdMerlinEvaluateHgiVulkanBridgeSupport(true, PXR_VERSION, true, true);
  Expect(vulkan.hgi_owned_targets && !vulkan.gpu_copy &&
             vulkan.selected_mode ==
                 HdMerlinHgiVulkanTransferMode::CpuReadback &&
             vulkan.fallback_reason ==
                 HdMerlinHgiVulkanFallbackReason::GpuCopyUnavailable,
         "HgiVulkan Tier 0 selection is incorrect");

  const auto unsupported =
      HdMerlinEvaluateHgiVulkanBridgeSupport(true, 9999, true, true);
  Expect(!unsupported.hgi_owned_targets &&
             unsupported.fallback_reason ==
                 HdMerlinHgiVulkanFallbackReason::UnsupportedOpenUsd,
         "unsupported OpenUSD version was not rejected");

  // Reported names are consumed by evidence tooling, so they are part of the
  // contract rather than debug text.
  Expect(HdMerlinHgiVulkanTransferModeName(
             HdMerlinHgiVulkanTransferMode::CpuReadback) ==
             std::string_view("cpu-readback"),
         "cpu-readback mode name is unstable");
  Expect(HdMerlinHgiVulkanTransferModeName(
             HdMerlinHgiVulkanTransferMode::GpuCopy) ==
             std::string_view("gpu-copy"),
         "gpu-copy mode name is unstable");
  Expect(HdMerlinHgiVulkanFallbackReasonName(
             HdMerlinHgiVulkanFallbackReason::GpuCopyUnavailable) ==
             std::string_view("gpu-copy-unavailable"),
         "gpu-copy-unavailable reason name is unstable");
  Expect(HdMerlinHgiVulkanFallbackReasonName(
             HdMerlinHgiVulkanFallbackReason::DriverSwapRejected) ==
             std::string_view("driver-swap-rejected"),
         "driver-swap-rejected reason name is unstable");

  // The format table drives both the RenderBuffer descriptor and the byte size
  // of the CPU buffer feeding it, so an unimplemented format must stay
  // unmapped rather than borrow a same-width texel.
  Expect(HdMerlinHgiFormatForRenderBuffer(HdFormatUNorm8Vec4) ==
             HgiFormatUNorm8Vec4,
         "color format mapping is incorrect");
  Expect(HdMerlinHgiFormatForRenderBuffer(HdFormatFloat32) == HgiFormatFloat32,
         "depth format mapping is incorrect");
  Expect(HdMerlinHgiFormatForRenderBuffer(HdFormatInt32) == HgiFormatInt32,
         "id format mapping is incorrect");
  Expect(HdMerlinHgiFormatForRenderBuffer(HdFormatUNorm8) == HgiFormatInvalid &&
             HdMerlinHgiFormatForRenderBuffer(HdFormatFloat16Vec4) ==
                 HgiFormatInvalid &&
             HdMerlinHgiFormatForRenderBuffer(HdFormatInvalid) ==
                 HgiFormatInvalid,
         "an unimplemented RenderBuffer format acquired an Hgi format");

  // Driver discovery must accept only a renderDriver entry that actually holds
  // an Hgi. Everything else is a missing driver, not a usable one.
  Expect(RejectionFor({}) ==
             HdMerlinHgiVulkanFallbackReason::MissingRenderDriver,
         "empty driver vector was not rejected");
  Expect(RejectionFor({nullptr}) ==
             HdMerlinHgiVulkanFallbackReason::MissingRenderDriver,
         "null driver entry was not rejected");

  HdDriver mistyped;
  mistyped.name = HgiTokens->renderDriver;
  mistyped.driver = VtValue(42);
  Expect(RejectionFor({&mistyped}) ==
             HdMerlinHgiVulkanFallbackReason::MissingRenderDriver,
         "renderDriver entry without an Hgi was not rejected");

  HdDriver misnamed;
  misnamed.name = TfToken("merlinNotARenderDriver");
  misnamed.driver = VtValue(static_cast<Hgi*>(nullptr));
  Expect(RejectionFor({&misnamed}) ==
             HdMerlinHgiVulkanFallbackReason::MissingRenderDriver,
         "non-renderDriver entry was not rejected");

  // Probing a bridge that never received a driver must not replace the
  // capability-level rejection with an operational one, and must not pretend
  // to have produced a target.
  HdMerlinHgiVulkanBridge bridge(true);
  bridge.SetDrivers({});
  HgiTextureDesc descriptor;
  descriptor.usage = HgiTextureUsageBitsShaderRead;
  descriptor.format = HgiFormatUNorm8Vec4;
  descriptor.dimensions = GfVec3i(1, 1, 1);
  HgiTextureHandle probe = bridge.CreateTarget(descriptor, false);
  Expect(!probe, "a driverless bridge produced a target");

  // Retiring nothing is a no-op rather than a counted retirement.
  bridge.DestroyTarget(nullptr);
  bridge.DestroyTarget(&probe);

  const auto runtime = bridge.status();
  Expect(runtime.openusd_version == PXR_VERSION &&
             runtime.fallback_reason ==
                 HdMerlinHgiVulkanFallbackReason::MissingRenderDriver,
         "target probe replaced the missing-driver rejection");

  const auto telemetry = bridge.telemetry();
  Expect(telemetry.target_creations == 0 && telemetry.target_retirements == 0 &&
             telemetry.target_orphans == 0 && telemetry.cpu_upload_count == 0 &&
             telemetry.cpu_upload_bytes == 0,
         "a driverless bridge reported target or upload activity");
  Expect(telemetry.gpu_copy_count == 0 && telemetry.coarse_wait_count == 0,
         "Tier 0 reported GPU-copy or coarse-wait activity");

  return g_failures == 0 ? 0 : 1;
}
