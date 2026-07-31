#include "adapter.hpp"

#include <pxr/pxr.h>

#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

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
  if (!Check(HdMerlinCanUseExclusiveGpuColorCopy(1, 1),
             "one GPU-capable color binding was rejected") ||
      !Check(!HdMerlinCanUseExclusiveGpuColorCopy(2, 1),
             "mixed GPU/CPU color bindings suppressed CPU readback") ||
      !Check(!HdMerlinCanUseExclusiveGpuColorCopy(2, 2),
             "multiple GPU color bindings selected only one destination") ||
      !Check(!HdMerlinCanUseExclusiveGpuColorCopy(1, 0),
             "CPU-only color binding selected GPU copy")) {
    return 1;
  }

  HdMerlinRenderBuffer color(SdfPath("/color"));
  if (!Check(color.Allocate(GfVec3i(2, 2, 1), HdFormatUNorm8Vec4, false),
             "color allocation failed") ||
      !Check(color.GetWidth() == 2 && color.GetHeight() == 2 &&
                 color.GetDepth() == 1,
             "color dimensions are incorrect") ||
      !Check(!color.IsConverged(), "new color buffer is converged")) {
    return 1;
  }
  if (!Check(color.GetResource(false).IsEmpty(),
             "CPU fallback unexpectedly exposed an Hgi resource")) {
    return 1;
  }

  const std::vector<std::uint8_t> pixels{
      1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  if (!Check(color.WriteColor(pixels, 2, 2), "color write failed")) {
    return 1;
  }
  color.SetConverged(true);
  if (!Check(color.IsConverged(), "color convergence was not recorded")) {
    return 1;
  }
  const auto* mapped = static_cast<const std::uint8_t*>(color.Map());
  if (!Check(mapped != nullptr && mapped[0] == 1 && mapped[15] == 16,
             "mapped color data is incorrect") ||
      !Check(color.IsMapped(), "color map state is incorrect") ||
      !Check(!color.Allocate(GfVec3i(4, 4, 1), HdFormatUNorm8Vec4, false),
             "mapped buffer was resized")) {
    return 1;
  }
  color.Unmap();
  if (!Check(!color.IsMapped(), "color unmap state is incorrect") ||
      !Check(color.Allocate(GfVec3i(4, 4, 1), HdFormatUNorm8Vec4, false),
             "color resize failed")) {
    return 1;
  }

  HdMerlinRenderBuffer depth(SdfPath("/depth"));
  if (!Check(depth.Allocate(GfVec3i(2, 1, 1), HdFormatFloat32, false),
             "depth allocation failed") ||
      !Check(depth.WriteDepth({0.25F, 1.0F}, 2, 1),
             "depth write failed")) {
    return 1;
  }
  const auto* mapped_depth = static_cast<const float*>(depth.Map());
  if (!Check(mapped_depth != nullptr && mapped_depth[0] == 0.25F &&
                 mapped_depth[1] == 1.0F,
             "mapped depth data is incorrect")) {
    return 1;
  }
  depth.Unmap();
  if (!Check(!depth.Allocate(GfVec3i(2, 1, 1), HdFormatFloat32, true),
             "multisampled depth allocation unexpectedly succeeded")) {
    return 1;
  }

  HdMerlinRenderBuffer ids(SdfPath("/primId"));
  if (!Check(ids.Allocate(GfVec3i(2, 1, 1), HdFormatInt32, false),
             "id allocation failed") ||
      !Check(ids.WriteId({7U, 11U}, 2, 1), "id write failed")) {
    return 1;
  }
  const auto* mapped_ids = static_cast<const std::uint32_t*>(ids.Map());
  if (!Check(mapped_ids != nullptr && mapped_ids[0] == 7U &&
                 mapped_ids[1] == 11U,
             "mapped id data is incorrect")) {
    return 1;
  }
  ids.Unmap();

  // A bridge that never receives an Hgi driver has to leave the buffer on the
  // CPU path without disturbing it: writes still land, no resource is
  // published, and no target is charged to telemetry.
  auto bridge = std::make_shared<HdMerlinHgiVulkanBridge>(true);
  bridge->SetDrivers({});
  HdMerlinRenderBuffer bridged(SdfPath("/bridgedColor"), bridge);
  if (!Check(bridged.Allocate(GfVec3i(2, 2, 1), HdFormatUNorm8Vec4, false),
             "bridged color allocation failed") ||
      !Check(bridged.WriteColor(pixels, 2, 2), "bridged color write failed") ||
      !Check(bridged.GetResource(false).IsEmpty(),
             "driverless bridge exposed an Hgi resource") ||
      !Check(bridge->telemetry().target_creations == 0,
             "driverless bridge created a target")) {
    return 1;
  }
  const auto* mapped_bridged =
      static_cast<const std::uint8_t*>(bridged.Map());
  if (!Check(mapped_bridged != nullptr && mapped_bridged[0] == 1 &&
                 mapped_bridged[15] == 16,
             "bridged color data did not reach the CPU buffer")) {
    return 1;
  }
  bridged.Unmap();
  return 0;
}
