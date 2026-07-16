#include "presentation_glfw.hpp"

#include "window.hpp"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace merlin::viewport {
namespace {

template <typename Handle>
std::uintptr_t EncodeHandle(Handle handle) noexcept {
  if constexpr (std::is_pointer_v<Handle>) {
    return reinterpret_cast<std::uintptr_t>(handle);
  } else {
    return static_cast<std::uintptr_t>(handle);
  }
}

template <typename Handle>
Handle DecodeHandle(std::uintptr_t handle) noexcept {
  if constexpr (std::is_pointer_v<Handle>) {
    return reinterpret_cast<Handle>(handle);
  } else {
    return static_cast<Handle>(handle);
  }
}

std::int32_t CreateSurface(void* user_data, std::uintptr_t encoded_instance,
                           std::uintptr_t* encoded_surface) {
  if (user_data == nullptr || encoded_instance == 0 ||
      encoded_surface == nullptr) {
    return static_cast<std::int32_t>(VK_ERROR_INITIALIZATION_FAILED);
  }
  VkSurfaceKHR surface{};
  const auto result = glfwCreateWindowSurface(
      DecodeHandle<VkInstance>(encoded_instance),
      static_cast<GLFWwindow*>(user_data), nullptr, &surface);
  if (result == VK_SUCCESS) {
    *encoded_surface = EncodeHandle(surface);
  }
  return static_cast<std::int32_t>(result);
}

}  // namespace

vulkan::PresentationOptions MakeGlfwVulkanPresentation(Window& window,
                                                        bool vsync) {
  std::uint32_t count{};
  const auto* extensions = glfwGetRequiredInstanceExtensions(&count);
  if (extensions == nullptr || count == 0) {
    const char* detail{};
    (void)glfwGetError(&detail);
    throw std::runtime_error(
        std::string("GLFW reported no Vulkan instance extensions") +
        (detail == nullptr ? "" : ": " + std::string(detail)));
  }
  vulkan::PresentationOptions result;
  result.required_instance_extensions.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    result.required_instance_extensions.emplace_back(extensions[index]);
  }
  result.user_data = window.native_window();
  result.create_surface = CreateSurface;
  result.vsync = vsync;
  return result;
}

}  // namespace merlin::viewport
