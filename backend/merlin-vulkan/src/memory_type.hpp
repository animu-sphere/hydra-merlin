#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <optional>
#include <stdexcept>

namespace merlin::vulkan::detail {

// Preferences never relax the resource's required properties or type mask.
// With no preferred match, retain the first compatible type's old behavior.
inline std::uint32_t FindMemoryType(
    const VkPhysicalDeviceMemoryProperties& memory, std::uint32_t bits,
    VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred = 0) {
  std::optional<std::uint32_t> fallback;
  for (std::uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
    const auto flags = memory.memoryTypes[index].propertyFlags;
    if ((bits & (1U << index)) == 0U || (flags & required) != required) {
      continue;
    }
    if ((flags & preferred) == preferred) {
      return index;
    }
    if (!fallback) {
      fallback = index;
    }
  }
  if (fallback) {
    return *fallback;
  }
  throw std::runtime_error("no compatible Vulkan memory type");
}

} // namespace merlin::vulkan::detail
