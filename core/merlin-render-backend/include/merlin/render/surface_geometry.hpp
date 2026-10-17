#pragma once

#include <merlin/render/backend.hpp>

namespace merlin::render {

// Direction of NDC +Y in a backend's framebuffer coordinates (origin top-left,
// +Y down). Vulkan maps NDC +Y down; Metal maps it up. Fragment derivatives
// are taken in framebuffer coordinates, so this fixes the sign that relates
// them to the camera's front-face winding.
enum class NdcYDirection { Down,
  Up };

// Effective rasterizer winding for one draw or instance: the camera's front
// face, reversed for a mirrored transform (InstanceRecord::mirrored) so that
// its front faces match the unmirrored source. Backends map this logical
// winding to native state.
[[nodiscard]] FrontFaceWinding ForwardDrawFrontFace(
    FrontFaceWinding camera, bool mirrored) noexcept;

// True for the debug views executed by the shared Forward surface context.
[[nodiscard]] bool IsForwardSurfaceDebugView(DebugView view) noexcept;

// Per-frame surface policy shared by every Forward entry:
//   x: +/-1 orientation of the derivative geometric normal; with the camera
//      winding and NDC direction it orients Ng toward the visible side,
//   y: debug view (0 none, 1 Ng, 2 Ns_base, 3 Ns, 4 front-facing),
//   zw: reserved zero.
// A debug view outside the surface context is rejected with InvalidRequest.
[[nodiscard]] Vec4 PackForwardSurfacePolicy(FrontFaceWinding camera,
    NdcYDirection ndc_y, DebugView view);

} // namespace merlin::render
