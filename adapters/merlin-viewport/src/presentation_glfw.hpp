#pragma once

#include <merlin/vulkan/renderer.hpp>

namespace merlin::viewport {

class Window;

// GLFW is the native host boundary. The returned callback lets the Vulkan
// backend create and own its VkSurfaceKHR without exposing it to the viewport
// application or Core.
[[nodiscard]] vulkan::PresentationOptions MakeGlfwVulkanPresentation(
    Window& window, bool vsync);

}  // namespace merlin::viewport
