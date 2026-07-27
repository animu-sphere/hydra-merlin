#include "hgi_vulkan_bridge.hpp"

#include <pxr/pxr.h>
#include <pxr/imaging/hgi/enums.h>
#include <pxr/imaging/hgi/types.h>

#include <iostream>
#include <string_view>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

bool Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

}  // namespace

int main() {
  const auto disabled = HdMerlinEvaluateHgiVulkanBridgeSupport(
      false, PXR_VERSION, true, true);
  if (!Check(!disabled.hgi_owned_targets &&
                 disabled.fallback_reason ==
                     HdMerlinHgiVulkanFallbackReason::BridgeDisabled,
             "disabled bridge did not retain Tier 0")) {
    return 1;
  }

  const auto missing = HdMerlinEvaluateHgiVulkanBridgeSupport(
      true, PXR_VERSION, false, false);
  if (!Check(!missing.hgi_owned_targets &&
                 missing.fallback_reason ==
                     HdMerlinHgiVulkanFallbackReason::MissingRenderDriver,
             "missing Hgi driver was not rejected")) {
    return 1;
  }

  const auto non_vulkan = HdMerlinEvaluateHgiVulkanBridgeSupport(
      true, PXR_VERSION, true, false);
  if (!Check(!non_vulkan.hgi_owned_targets &&
                 non_vulkan.fallback_reason ==
                     HdMerlinHgiVulkanFallbackReason::NonVulkanRenderDriver,
             "non-Vulkan Hgi driver was not rejected")) {
    return 1;
  }

  const auto vulkan = HdMerlinEvaluateHgiVulkanBridgeSupport(
      true, PXR_VERSION, true, true);
  if (!Check(vulkan.hgi_owned_targets && !vulkan.gpu_copy &&
                 vulkan.selected_mode ==
                     HdMerlinHgiVulkanTransferMode::CpuReadback &&
                 vulkan.fallback_reason ==
                     HdMerlinHgiVulkanFallbackReason::GpuCopyUnavailable,
             "HgiVulkan Tier 0 selection is incorrect") ||
      !Check(HdMerlinHgiVulkanTransferModeName(vulkan.selected_mode) ==
                 std::string_view("cpu-readback"),
             "transfer mode name is unstable") ||
      !Check(HdMerlinHgiVulkanFallbackReasonName(vulkan.fallback_reason) ==
                 std::string_view("gpu-copy-unavailable"),
             "fallback reason name is unstable")) {
    return 1;
  }

  const auto unsupported = HdMerlinEvaluateHgiVulkanBridgeSupport(
      true, 9999, true, true);
  if (!Check(!unsupported.hgi_owned_targets &&
                 unsupported.fallback_reason ==
                     HdMerlinHgiVulkanFallbackReason::UnsupportedOpenUsd,
             "unsupported OpenUSD version was not rejected")) {
    return 1;
  }

  HdMerlinHgiVulkanBridge bridge(true);
  bridge.SetDrivers({});
  HgiTextureDesc descriptor;
  descriptor.usage = HgiTextureUsageBitsShaderRead;
  descriptor.format = HgiFormatUNorm8Vec4;
  descriptor.dimensions = GfVec3i(1, 1, 1);
  (void)bridge.CreateTarget(descriptor, false);
  const auto runtime = bridge.status();
  if (!Check(runtime.openusd_version == PXR_VERSION &&
                 runtime.fallback_reason ==
                     HdMerlinHgiVulkanFallbackReason::MissingRenderDriver,
             "target probe replaced the missing-driver rejection")) {
    return 1;
  }
  return 0;
}
