#include <merlin/vulkan/renderer.hpp>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <iostream>
#include <type_traits>
#include <vector>

namespace {

template <typename Handle>
std::uintptr_t EncodeHandle(Handle handle) noexcept {
  if constexpr (std::is_pointer_v<Handle>) {
    return reinterpret_cast<std::uintptr_t>(handle);
  } else {
    return static_cast<std::uintptr_t>(handle);
  }
}

struct VulkanContext {
  VkInstance instance{};
  VkPhysicalDevice physical_device{};
  VkDevice device{};
  VkQueue queue{};
  std::uint32_t queue_family{};

  ~VulkanContext() {
    if (device != VK_NULL_HANDLE) {
      (void)vkDeviceWaitIdle(device);
      vkDestroyDevice(device, nullptr);
    }
    if (instance != VK_NULL_HANDLE) {
      vkDestroyInstance(instance, nullptr);
    }
  }
};

bool CreateContext(VulkanContext& result) {
  VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  application.pApplicationName = "merlin-borrowed-context-test";
  application.apiVersion = VK_API_VERSION_1_4;
  VkInstanceCreateInfo instance_info{
      VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instance_info.pApplicationInfo = &application;
  if (vkCreateInstance(&instance_info, nullptr, &result.instance) !=
      VK_SUCCESS) {
    return false;
  }

  std::uint32_t device_count{};
  if (vkEnumeratePhysicalDevices(result.instance, &device_count, nullptr) !=
          VK_SUCCESS ||
      device_count == 0) {
    return false;
  }
  std::vector<VkPhysicalDevice> devices(device_count);
  if (vkEnumeratePhysicalDevices(result.instance, &device_count,
                                 devices.data()) != VK_SUCCESS) {
    return false;
  }
  for (const auto candidate : devices) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(candidate, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_4) {
      continue;
    }
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceFeatures2 features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &timeline;
    vkGetPhysicalDeviceFeatures2(candidate, &features);
    if (timeline.timelineSemaphore != VK_TRUE) {
      continue;
    }

    std::uint32_t queue_count{};
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count,
                                             queues.data());
    for (std::uint32_t index = 0; index < queue_count; ++index) {
      if ((queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0U) {
        continue;
      }
      const float priority = 1.0F;
      VkDeviceQueueCreateInfo queue_info{
          VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      queue_info.queueFamilyIndex = index;
      queue_info.queueCount = 1;
      queue_info.pQueuePriorities = &priority;
      VkPhysicalDeviceTimelineSemaphoreFeatures enabled_timeline{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
      enabled_timeline.timelineSemaphore = VK_TRUE;
      VkDeviceCreateInfo device_info{
          VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
      device_info.pNext = &enabled_timeline;
      device_info.queueCreateInfoCount = 1;
      device_info.pQueueCreateInfos = &queue_info;
      if (vkCreateDevice(candidate, &device_info, nullptr, &result.device) !=
          VK_SUCCESS) {
        continue;
      }
      result.physical_device = candidate;
      result.queue_family = index;
      vkGetDeviceQueue(result.device, index, 0, &result.queue);
      return result.queue != VK_NULL_HANDLE;
    }
  }
  return false;
}

merlin::vulkan::BorrowedVulkanContext Describe(
    const VulkanContext& context) {
  merlin::vulkan::BorrowedVulkanContext result;
  result.instance = EncodeHandle(context.instance);
  result.physical_device = EncodeHandle(context.physical_device);
  result.device = EncodeHandle(context.device);
  result.graphics_queue = EncodeHandle(context.queue);
  result.graphics_queue_family = context.queue_family;
  result.graphics_queue_index = 0;
  result.timeline_semaphore_enabled = true;
  return result;
}

bool ThrowsRendererError(const merlin::vulkan::RendererOptions& options) {
  try {
    merlin::vulkan::Renderer renderer(options);
  } catch (const merlin::vulkan::RendererError&) {
    return true;
  }
  return false;
}

}  // namespace

int main() {
  VulkanContext context;
  if (!CreateContext(context)) {
    std::cerr << "SKIP: Vulkan 1.4 graphics device with timeline semaphores "
                 "is unavailable\n";
    return 77;
  }

  merlin::vulkan::RendererOptions invalid;
  invalid.frames_in_flight = 2;
  invalid.descriptor_backend =
      merlin::vulkan::DescriptorBackendRequest::Conventional;
  invalid.borrowed_context = Describe(context);
  invalid.borrowed_context->graphics_queue = 0;
  if (!ThrowsRendererError(invalid)) {
    std::cerr << "null borrowed queue was accepted\n";
    return 1;
  }

  auto validation_mismatch = invalid;
  validation_mismatch.borrowed_context = Describe(context);
  validation_mismatch.enable_validation = true;
  if (!ThrowsRendererError(validation_mismatch)) {
    std::cerr << "unavailable borrowed validation was accepted\n";
    return 1;
  }

  auto bindless = invalid;
  bindless.borrowed_context = Describe(context);
  bindless.descriptor_backend =
      merlin::vulkan::DescriptorBackendRequest::Bindless;
  if (!ThrowsRendererError(bindless)) {
    std::cerr << "borrowed bindless mode was accepted without an enabled "
                 "feature contract\n";
    return 1;
  }

  {
    merlin::vulkan::RendererOptions options;
    options.frames_in_flight = 2;
    options.borrowed_context = Describe(context);
    merlin::vulkan::Renderer renderer(options);
    const auto& capabilities = renderer.capabilities();
    if (!capabilities.borrowed_vulkan_context ||
        !capabilities.timeline_semaphore ||
        capabilities.async_transfer_queue ||
        capabilities.queue_ownership_transfers ||
        capabilities.graphics_queue_family != context.queue_family ||
        capabilities.transfer_queue_family != context.queue_family ||
        capabilities.descriptor_indexing_selection.selected_backend !=
            merlin::vulkan::DescriptorBackend::Conventional) {
      std::cerr << "borrowed Vulkan capability selection is incorrect\n";
      return 1;
    }
  }

  // Renderer destruction must leave the application device usable. In
  // particular it must not destroy a device borrowed from Hgi or another host.
  VkSemaphoreCreateInfo semaphore_info{
      VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkSemaphore semaphore{};
  if (vkCreateSemaphore(context.device, &semaphore_info, nullptr,
                        &semaphore) != VK_SUCCESS) {
    std::cerr << "renderer destroyed or corrupted the borrowed device\n";
    return 1;
  }
  vkDestroySemaphore(context.device, semaphore, nullptr);
  return 0;
}
