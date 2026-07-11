#include "adapter.hpp"

#include <pxr/pxr.h>

#include <cstdint>
#include <iostream>
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
  HdMerlinRenderBuffer color(SdfPath("/color"));
  if (!Check(color.Allocate(GfVec3i(2, 2, 1), HdFormatUNorm8Vec4, false),
             "color allocation failed") ||
      !Check(color.GetWidth() == 2 && color.GetHeight() == 2 &&
                 color.GetDepth() == 1,
             "color dimensions are incorrect") ||
      !Check(!color.IsConverged(), "new color buffer is converged")) {
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
  return 0;
}
