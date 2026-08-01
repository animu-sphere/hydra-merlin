#include "hgi_metal_bridge.hpp"

#include <pxr/pxr.h>
#include <pxr/imaging/hgi/enums.h>

#include <array>
#include <iostream>
#include <string_view>
#include <utility>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {
int failures = 0;
void Expect(bool value, const char* message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}
}  // namespace

int main() {
  const auto disabled = HdMerlinEvaluateHgiMetalBridgeSupport(
      false, PXR_VERSION, true, true);
  Expect(!disabled.hgi_owned_targets &&
             disabled.fallback_reason ==
                 HdMerlinHgiMetalFallbackReason::BridgeDisabled,
         "disabled bridge did not retain Tier 0");
  const auto missing = HdMerlinEvaluateHgiMetalBridgeSupport(
      true, PXR_VERSION, false, false);
  Expect(missing.fallback_reason ==
             HdMerlinHgiMetalFallbackReason::MissingRenderDriver,
         "missing driver was not rejected");
  const auto metal = HdMerlinEvaluateHgiMetalBridgeSupport(
      true, PXR_VERSION, true, true);
  Expect(metal.hgi_owned_targets && !metal.gpu_copy &&
             metal.selected_mode == HdMerlinHgiMetalTransferMode::CpuReadback,
         "Metal bridge capability baseline is incorrect");
  Expect(HdMerlinHgiMetalTransferModeName(
             HdMerlinHgiMetalTransferMode::GpuCopy) ==
             std::string_view("gpu-copy"),
         "transfer mode name is unstable");

  HdMerlinHgiMetalDirectShareRequirements complete{
      .device_identity = true,
      .texture_storage_compatible = true,
      .texture_usage_compatible = true,
      .pixel_format_compatible = true,
      .command_queue_compatible = true,
      .completion_retention_available = true,
      .resize_retirement_safe = true,
      .public_texture_import_available = true,
      .direct_path_available = true,
  };
  Expect(HdMerlinEvaluateHgiMetalDirectShare(complete).supported,
         "complete direct-share requirements were rejected");
  const std::array gates{
      std::pair{&HdMerlinHgiMetalDirectShareRequirements::device_identity,
                HdMerlinHgiMetalDirectShareRejection::DeviceMismatch},
      std::pair{&HdMerlinHgiMetalDirectShareRequirements::texture_storage_compatible,
                HdMerlinHgiMetalDirectShareRejection::TextureStorageMismatch},
      std::pair{&HdMerlinHgiMetalDirectShareRequirements::texture_usage_compatible,
                HdMerlinHgiMetalDirectShareRejection::TextureUsageMismatch},
      std::pair{&HdMerlinHgiMetalDirectShareRequirements::pixel_format_compatible,
                HdMerlinHgiMetalDirectShareRejection::PixelFormatMismatch},
      std::pair{&HdMerlinHgiMetalDirectShareRequirements::command_queue_compatible,
                HdMerlinHgiMetalDirectShareRejection::CommandQueueMismatch},
      std::pair{&HdMerlinHgiMetalDirectShareRequirements::completion_retention_available,
                HdMerlinHgiMetalDirectShareRejection::CompletionRetentionUnavailable},
      std::pair{&HdMerlinHgiMetalDirectShareRequirements::resize_retirement_safe,
                HdMerlinHgiMetalDirectShareRejection::ResizeRetirementUnsafe},
      std::pair{&HdMerlinHgiMetalDirectShareRequirements::public_texture_import_available,
                HdMerlinHgiMetalDirectShareRejection::PublicTextureImportUnavailable},
      std::pair{&HdMerlinHgiMetalDirectShareRequirements::direct_path_available,
                HdMerlinHgiMetalDirectShareRejection::DirectPathUnavailable},
  };
  for (const auto& [member, reason] : gates) {
    auto rejected = complete;
    rejected.*member = false;
    const auto result = HdMerlinEvaluateHgiMetalDirectShare(rejected);
    Expect(!result.supported && result.rejection == reason,
           "a direct-share gate did not produce its rejection");
  }
  Expect(HdMerlinHgiMetalFormatForRenderBuffer(HdFormatUNorm8Vec4) ==
             HgiFormatUNorm8Vec4 &&
             HdMerlinHgiMetalFormatForRenderBuffer(HdFormatFloat32) ==
                 HgiFormatFloat32 &&
             HdMerlinHgiMetalFormatForRenderBuffer(HdFormatInt32) ==
                 HgiFormatInt32,
         "Metal RenderBuffer format table is incorrect");
  return failures == 0 ? 0 : 1;
}
