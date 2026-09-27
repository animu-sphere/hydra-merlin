#include "memory_type.hpp"

#include <cassert>
#include <initializer_list>
#include <stdexcept>

int main() {
  using merlin::vulkan::detail::FindMemoryType;
  constexpr auto coherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  constexpr auto cached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
  VkPhysicalDeviceMemoryProperties memory{};
  memory.memoryTypeCount = 5;
  memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  memory.memoryTypes[1].propertyFlags = coherent;
  memory.memoryTypes[2].propertyFlags =
      coherent | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  memory.memoryTypes[3].propertyFlags = coherent | cached;
  memory.memoryTypes[4].propertyFlags =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | cached;

  // Both discrete and integrated devices can enumerate uncached types first.
  assert(FindMemoryType(memory, 0x1f, coherent, cached) == 3);
  // Uploads and images without a preference retain the original selection.
  assert(FindMemoryType(memory, 0x1f, coherent) == 1);
  assert(FindMemoryType(memory, 0x1f, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0);
  // Resource compatibility takes precedence over a cached memory preference.
  assert(FindMemoryType(memory, 0x07, coherent, cached) == 1);
  assert(FindMemoryType(memory, 0x04, coherent, cached) == 2);
  // A cached but noncoherent type cannot enter a read path with no invalidate.
  memory.memoryTypes[3].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
  assert(FindMemoryType(memory, 0x1f, coherent, cached) == 1);
  for (const auto bits : {0U, 0x10U, 0xffffffe0U}) {
    bool rejected{};
    try {
      (void)FindMemoryType(memory, bits, coherent, cached);
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    assert(rejected);
  }
  // The high bit is a valid memory type, independent of the driver's order.
  memory.memoryTypeCount = VK_MAX_MEMORY_TYPES;
  memory.memoryTypes[31].propertyFlags = coherent | cached;
  assert(FindMemoryType(memory, 0xffffffffU, coherent, cached) == 31);
}
