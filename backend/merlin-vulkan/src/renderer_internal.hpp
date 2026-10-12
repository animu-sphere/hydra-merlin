#pragma once

#include <merlin/vulkan/renderer.hpp>
#include <merlin/render/backend.hpp>
#include <merlin/render/forward_lighting.hpp>
#include "environment_lighting.hpp"
#include "gaussian_preparation.hpp"
#include "memory_type.hpp"

#include <merlin/vulkan/shader_abi.hpp>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cmath>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace merlin::vulkan {
namespace detail {

using CpuClock = std::chrono::steady_clock;

inline std::atomic<std::uint64_t> g_renderer_owner{1};

inline std::uint64_t ElapsedNanoseconds(CpuClock::time_point start) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(CpuClock::now() - start)
          .count());
}

template <typename T>
class IndexedTableView {
public:
  void Sync(const extraction::PersistentTable<T>& table) {
    if (source_.table_identity() == table.table_identity()) {
      return;
    }
    records_.clear();
    source_ = table;
    records_.reserve(source_.size());
    for (const auto& record : source_) {
      records_.push_back(&record);
    }
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return records_.size();
  }
  [[nodiscard]] bool empty() const noexcept {
    return records_.empty();
  }
  [[nodiscard]] const T& operator[](std::size_t index) const {
    return *records_[index];
  }

private:
  // Retaining the table root keeps every cached record pointer alive and also
  // prevents allocator address reuse from aliasing table_identity().
  extraction::PersistentTable<T> source_;
  std::vector<const T*> records_;
};

template <typename T>
class DenseTableView {
public:
  void Sync(const extraction::PersistentTable<T>& table) {
    if (source_.table_identity() == table.table_identity()) {
      return;
    }
    records_.clear();
    source_ = table;
    records_.reserve(source_.size());
    for (const auto& record : source_) {
      records_.push_back(record);
    }
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return records_.size();
  }
  [[nodiscard]] const T& operator[](std::size_t index) const {
    return records_[index];
  }

private:
  // Keep the root alive so an allocator cannot recycle its identity while the
  // dense view is current.
  extraction::PersistentTable<T> source_;
  std::vector<T> records_;
};

template <typename T, typename HandleOf>
inline const T* FindDeltaRecord(const extraction::PersistentTable<T>& table,
    const extraction::ResourceDelta& delta,
    std::size_t delta_index, HandleOf handle_of) {
  const auto handle = delta.upserts[delta_index];
  if (delta.upsert_indices.size() == delta.upserts.size()) {
    const auto index = delta.upsert_indices[delta_index];
    if (index < table.size() && handle_of(table[index]) == handle) {
      return &table[index];
    }
  }
  // Compatibility with snapshots created before dense upsert indices were
  // added. A missing or stale hint falls back to handle reconciliation.
  const auto found = std::find_if(
      table.begin(), table.end(),
      [&](const T& record) { return handle_of(record) == handle; });
  return found == table.end() ? nullptr : &*found;
}

constexpr std::uint32_t kMinimumVulkanApiVersion = VK_MAKE_API_VERSION(
    0, MERLIN_VULKAN_MIN_VERSION_MAJOR, MERLIN_VULKAN_MIN_VERSION_MINOR, 0);
// OpenUSD 26.05 and 26.08 create HgiVulkan devices against Vulkan 1.3.
// Borrowed mode uses the conventional descriptor path and only Vulkan 1.3
// core functionality, while Merlin-owned contexts retain the Vulkan 1.4
// product baseline.
constexpr std::uint32_t kMinimumBorrowedVulkanApiVersion = VK_API_VERSION_1_3;

constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kGaussianColorFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat kIdFormat = VK_FORMAT_R32_UINT;
// Frame timestamp pairs: 0/1 graphics submission, 2/3 Gaussian raster
// (the Gaussian subpasses and any compute tile raster), 4/5 Gaussian GPU
// sort, 6/7 Gaussian tile binning, and 8/9 Gaussian GPU preparation.
constexpr std::uint32_t kFrameTimestampQueryCount = 10;

inline void Check(VkResult result, const char* operation) {
  if (result == VK_SUCCESS) {
    return;
  }
  RendererErrorCode code = RendererErrorCode::BackendFailure;
  if (result == VK_TIMEOUT) {
    code = RendererErrorCode::Timeout;
  } else if (result == VK_ERROR_DEVICE_LOST) {
    code = RendererErrorCode::DeviceLost;
  } else if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY ||
             result == VK_ERROR_OUT_OF_HOST_MEMORY) {
    code = RendererErrorCode::ResourceExhausted;
  } else if (result == VK_ERROR_FEATURE_NOT_PRESENT ||
             result == VK_ERROR_FORMAT_NOT_SUPPORTED ||
             result == VK_ERROR_EXTENSION_NOT_PRESENT ||
             result == VK_ERROR_LAYER_NOT_PRESENT ||
             result == VK_ERROR_INCOMPATIBLE_DRIVER) {
    code = RendererErrorCode::Unsupported;
  }
  throw RendererError(code, operation,
      "VkResult " +
          std::to_string(static_cast<std::int32_t>(result)),
      static_cast<std::int32_t>(result));
}

template <typename Handle>
inline std::uintptr_t EncodeHandle(Handle handle) noexcept {
  if constexpr (std::is_pointer_v<Handle>) {
    return reinterpret_cast<std::uintptr_t>(handle);
  } else {
    return static_cast<std::uintptr_t>(handle);
  }
}

template <typename Handle>
inline Handle DecodeHandle(std::uintptr_t handle) noexcept {
  if constexpr (std::is_pointer_v<Handle>) {
    return reinterpret_cast<Handle>(handle);
  } else {
    return static_cast<Handle>(handle);
  }
}

inline std::vector<std::uint32_t> ReadSpirv(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        "load SPIR-V shader",
        "could not open file: " + path.string());
  }
  const auto end = stream.tellg();
  if (end <= 0 || (end % static_cast<std::streamoff>(sizeof(std::uint32_t))) != 0) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        "load SPIR-V shader",
        "file size is invalid: " + path.string());
  }
  std::vector<std::uint32_t> code(static_cast<std::size_t>(end) /
                                  sizeof(std::uint32_t));
  stream.seekg(0);
  stream.read(reinterpret_cast<char*>(code.data()), end);
  if (!stream) {
    throw RendererError(RendererErrorCode::BackendFailure,
        "load SPIR-V shader",
        "could not read file: " + path.string());
  }
  constexpr std::uint32_t kSpirvMagic = 0x07230203U;
  if (code.size() < 5U || code[0] != kSpirvMagic || code[3] == 0U ||
      code[4] != 0U) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        "load SPIR-V shader",
        "file header is invalid: " + path.string());
  }
  return code;
}

inline bool HasLayer(const char* name) {
  std::uint32_t count{};
  Check(vkEnumerateInstanceLayerProperties(&count, nullptr),
      "enumerate instance layers");
  std::vector<VkLayerProperties> layers(count);
  Check(vkEnumerateInstanceLayerProperties(&count, layers.data()),
      "enumerate instance layers");
  return std::any_of(layers.begin(), layers.end(), [name](const auto& layer) {
    return std::strcmp(layer.layerName, name) == 0;
  });
}

inline bool HasInstanceExtension(const char* name) {
  std::uint32_t count{};
  Check(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr),
      "enumerate instance extensions");
  std::vector<VkExtensionProperties> extensions(count);
  Check(vkEnumerateInstanceExtensionProperties(nullptr, &count,
            extensions.data()),
      "enumerate instance extensions");
  return std::any_of(extensions.begin(), extensions.end(),
      [name](const auto& extension) {
        return std::strcmp(extension.extensionName, name) == 0;
      });
}

inline bool HasDeviceExtension(VkPhysicalDevice device, const char* name) {
  std::uint32_t count{};
  Check(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr),
      "enumerate device extensions");
  std::vector<VkExtensionProperties> extensions(count);
  Check(vkEnumerateDeviceExtensionProperties(device, nullptr, &count,
            extensions.data()),
      "enumerate device extensions");
  return std::any_of(extensions.begin(), extensions.end(),
      [name](const auto& extension) {
        return std::strcmp(extension.extensionName, name) == 0;
      });
}

using PushConstants = shader_abi::DrawConstants;
using MaterialUniforms = shader_abi::MaterialConstants;

struct GaussianGpuInstance {
  Vec2 center_pixels;
  Vec3 inverse_conic;
  Vec3 radiance;
  float opacity{};
  float radius_pixels{};
  float depth{};
  std::uint32_t resource_id{};
  std::uint32_t particle_id{};
};

struct GaussianPushConstants {
  Vec2 inverse_viewport_size;
};

static_assert(sizeof(GaussianGpuInstance) == 52U);
static_assert(offsetof(GaussianGpuInstance, inverse_conic) == 8U);
static_assert(offsetof(GaussianGpuInstance, radiance) == 20U);
static_assert(offsetof(GaussianGpuInstance, opacity) == 32U);
static_assert(offsetof(GaussianGpuInstance, resource_id) == 44U);

static_assert(shader_abi::kArtifactSchemaVersion ==
                  MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION,
    "shader artifact schema version drifted from the build system");
static_assert(shader_abi::kVersion == MERLIN_SHADER_ABI_VERSION,
    "shader ABI version drifted from the build system");

// The descriptor layouts and writes below are built from these declarations,
// so the shader ABI and the Vulkan resource setup cannot drift apart.
static_assert(shader_abi::kConventionalBaseColorTexture.set == 0);
static_assert(shader_abi::kConventionalBaseColorTexture.resource_class ==
              shader_abi::ResourceClass::CombinedImageSampler);
static_assert(shader_abi::kConventionalMaterialConstants.set == 0);
static_assert(shader_abi::kConventionalMaterialConstants.resource_class ==
              shader_abi::ResourceClass::UniformBuffer);
static_assert(shader_abi::kBindlessSamplers.set == 0);
static_assert(shader_abi::kBindlessSamplers.resource_class ==
              shader_abi::ResourceClass::Sampler);
static_assert(shader_abi::kBindlessTextures.set == 0);
static_assert(shader_abi::kBindlessTextures.resource_class ==
              shader_abi::ResourceClass::SampledImage);
static_assert(shader_abi::kBindlessMaterialConstants.set == 1);
static_assert(shader_abi::kBindlessMaterialConstants.resource_class ==
              shader_abi::ResourceClass::UniformBuffer);

// Vulkan requires the variable-count binding to be the highest in its set.
static_assert(shader_abi::kBindlessTextures.binding >
              shader_abi::kBindlessSamplers.binding);

constexpr std::uint32_t kMaskedAlphaFlag = 1U << 28U;
constexpr std::uint32_t kDoubleSidedFlag = 1U << 29U;
constexpr std::uint32_t kCounterClockwiseFrontFaceFlag = 1U << 30U;
constexpr std::uint32_t kGpuScenePipelineFlag = 1U << 31U;
constexpr std::uint32_t kSamplerIndexShift = 8U;
constexpr std::uint32_t kSamplerIndexMask = 0xffffU;

constexpr VkDeviceSize kArenaAlignment = 16;
constexpr VkDeviceSize kMinArenaBlockBytes = 256U * 1024U;
constexpr VkDeviceSize kMinStagingBytes = 256U * 1024U;
constexpr std::uint32_t kInvalidBlock = std::numeric_limits<std::uint32_t>::max();

inline constexpr VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment) {
  return (value + alignment - 1U) / alignment * alignment;
}

struct Buffer {
  VkBuffer handle{};
  VkDeviceMemory memory{};
  VkDeviceSize size{};
};

class DeviceMemoryBudget {
public:
  void Initialize(VkPhysicalDevice physical_device, VkDevice device,
      bool extension_available,
      std::uint64_t configured_limit_bytes) {
    physical_device_ = physical_device;
    device_ = device;
    extension_available_ = extension_available;
    configured_limit_bytes_ = configured_limit_bytes;
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &properties_);
    Refresh();
  }

  [[nodiscard]] VkDeviceMemory Allocate(VkDeviceSize bytes,
      std::uint32_t memory_type,
      const char* operation) {
    const auto heap = properties_.memoryTypes[memory_type].heapIndex;
    const bool device_local =
        (properties_.memoryHeaps[heap].flags &
            VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0U;
    if (device_local) {
      Refresh();
      const auto global_limit = configured_limit_bytes_ == 0
                                    ? telemetry_.heap_budget_bytes
                                    : std::min(configured_limit_bytes_,
                                          telemetry_.heap_budget_bytes);
      const auto heap_available =
          heap_budget_[heap] > heap_usage_[heap]
              ? heap_budget_[heap] - heap_usage_[heap]
              : 0;
      if (bytes > heap_available ||
          telemetry_.renderer_allocated_bytes > global_limit ||
          bytes > global_limit - telemetry_.renderer_allocated_bytes) {
        ++telemetry_.exhaustion_count;
        throw RendererError(
            RendererErrorCode::ResourceExhausted, operation,
            "device-local allocation of " + std::to_string(bytes) +
                " bytes exceeds available VRAM (renderer=" +
                std::to_string(telemetry_.renderer_allocated_bytes) +
                ", limit=" + std::to_string(global_limit) +
                ", heap-available=" + std::to_string(heap_available) + ")");
      }
    }

    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = bytes;
    allocation.memoryTypeIndex = memory_type;
    VkDeviceMemory memory{};
    Check(vkAllocateMemory(device_, &allocation, nullptr, &memory), operation);
    try {
      allocations_.emplace(memory, Allocation{bytes, heap, device_local});
    } catch (...) {
      vkFreeMemory(device_, memory, nullptr);
      throw;
    }
    ++telemetry_.allocation_count;
    if (device_local) {
      telemetry_.renderer_allocated_bytes += bytes;
      telemetry_.renderer_peak_allocated_bytes =
          std::max(telemetry_.renderer_peak_allocated_bytes,
              telemetry_.renderer_allocated_bytes);
    }
    return memory;
  }

  void Free(VkDeviceMemory memory) noexcept {
    if (memory == VK_NULL_HANDLE) {
      return;
    }
    const auto found = allocations_.find(memory);
    if (found != allocations_.end()) {
      if (found->second.device_local) {
        telemetry_.renderer_allocated_bytes -= found->second.bytes;
      }
      allocations_.erase(found);
      ++telemetry_.release_count;
    }
    vkFreeMemory(device_, memory, nullptr);
  }

  void Refresh() noexcept {
    if (physical_device_ == VK_NULL_HANDLE) {
      return;
    }
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 properties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    properties.pNext = extension_available_ ? &budget : nullptr;
    vkGetPhysicalDeviceMemoryProperties2(physical_device_, &properties);
    properties_ = properties.memoryProperties;
    telemetry_.extension_available = extension_available_;
    telemetry_.heap_capacity_bytes = 0;
    telemetry_.heap_budget_bytes = 0;
    telemetry_.heap_usage_bytes = 0;
    telemetry_.heap_available_bytes = 0;
    heap_budget_.fill(0);
    heap_usage_.fill(0);
    for (std::uint32_t heap = 0; heap < properties_.memoryHeapCount; ++heap) {
      if ((properties_.memoryHeaps[heap].flags &
              VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0U) {
        continue;
      }
      const auto capacity = properties_.memoryHeaps[heap].size;
      const auto heap_budget = extension_available_ ? budget.heapBudget[heap]
                                                    : capacity;
      const auto heap_usage = extension_available_ ? budget.heapUsage[heap]
                                                   : 0;
      heap_budget_[heap] = heap_budget;
      heap_usage_[heap] = heap_usage;
      telemetry_.heap_capacity_bytes += capacity;
      telemetry_.heap_budget_bytes += heap_budget;
      telemetry_.heap_usage_bytes += heap_usage;
      telemetry_.heap_available_bytes +=
          heap_budget > heap_usage ? heap_budget - heap_usage : 0;
    }
    telemetry_.configured_limit_bytes = configured_limit_bytes_;
    telemetry_.effective_limit_bytes =
        configured_limit_bytes_ == 0
            ? telemetry_.heap_budget_bytes
            : std::min(configured_limit_bytes_, telemetry_.heap_budget_bytes);
    ++telemetry_.query_count;
  }

  [[nodiscard]] const MemoryBudgetTelemetry& telemetry() const noexcept {
    return telemetry_;
  }

private:
  struct Allocation {
    VkDeviceSize bytes{};
    std::uint32_t heap{};
    bool device_local{};
  };

  VkPhysicalDevice physical_device_{};
  VkDevice device_{};
  bool extension_available_{};
  std::uint64_t configured_limit_bytes_{};
  VkPhysicalDeviceMemoryProperties properties_{};
  std::array<VkDeviceSize, VK_MAX_MEMORY_HEAPS> heap_budget_{};
  std::array<VkDeviceSize, VK_MAX_MEMORY_HEAPS> heap_usage_{};
  std::map<VkDeviceMemory, Allocation> allocations_;
  MemoryBudgetTelemetry telemetry_;
};

inline std::uint32_t FindMemoryTypeRaw(VkPhysicalDevice physical_device,
    std::uint32_t bits,
    VkMemoryPropertyFlags properties, VkMemoryPropertyFlags preferred = 0) {
  VkPhysicalDeviceMemoryProperties memory{};
  vkGetPhysicalDeviceMemoryProperties(physical_device, &memory);
  return detail::FindMemoryType(memory, bits, properties, preferred);
}

inline void DestroyBufferRaw(VkDevice device, Buffer& buffer,
    DeviceMemoryBudget* memory_budget = nullptr) noexcept {
  if (buffer.handle != VK_NULL_HANDLE) {
    vkDestroyBuffer(device, buffer.handle, nullptr);
  }
  if (buffer.memory != VK_NULL_HANDLE) {
    if (memory_budget != nullptr) {
      memory_budget->Free(buffer.memory);
    } else {
      vkFreeMemory(device, buffer.memory, nullptr);
    }
  }
  buffer = {};
}

inline Buffer CreateBufferRaw(VkDevice device, VkPhysicalDevice physical_device,
    VkDeviceSize size, VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties,
    DeviceMemoryBudget* memory_budget = nullptr,
    std::uint32_t first_queue_family = VK_QUEUE_FAMILY_IGNORED,
    std::uint32_t second_queue_family = VK_QUEUE_FAMILY_IGNORED,
    VkMemoryPropertyFlags preferred = 0) {
  Buffer result;
  result.size = size;
  VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = size;
  buffer_info.usage = usage;
  const std::array queue_families{first_queue_family, second_queue_family};
  if (first_queue_family != VK_QUEUE_FAMILY_IGNORED &&
      second_queue_family != VK_QUEUE_FAMILY_IGNORED &&
      first_queue_family != second_queue_family) {
    buffer_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
    buffer_info.queueFamilyIndexCount =
        static_cast<std::uint32_t>(queue_families.size());
    buffer_info.pQueueFamilyIndices = queue_families.data();
  } else {
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  }
  Check(vkCreateBuffer(device, &buffer_info, nullptr, &result.handle),
      "create buffer");
  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(device, result.handle, &requirements);
  try {
    const auto memory_type = FindMemoryTypeRaw(physical_device,
        requirements.memoryTypeBits, properties, preferred);
    if (memory_budget != nullptr) {
      result.memory = memory_budget->Allocate(requirements.size, memory_type,
          "allocate buffer memory");
    } else {
      VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      allocation.allocationSize = requirements.size;
      allocation.memoryTypeIndex = memory_type;
      Check(vkAllocateMemory(device, &allocation, nullptr, &result.memory),
          "allocate buffer memory");
    }
    Check(vkBindBufferMemory(device, result.handle, result.memory, 0),
        "bind buffer memory");
  } catch (...) {
    DestroyBufferRaw(device, result, memory_budget);
    throw;
  }
  return result;
}

// A suballocated span of one arena block. Geometry caches hold ranges instead
// of buffers so mesh edits and removals never destroy device allocations that
// other resources still occupy.
struct BufferRange {
  std::uint32_t block{kInvalidBlock};
  VkDeviceSize offset{};
  VkDeviceSize size{};

  [[nodiscard]] bool valid() const noexcept {
    return block != kInvalidBlock;
  }
};

// Grow-only pool of device-local blocks with first-fit free-list
// suballocation. Allocation and release order is deterministic, so identical
// edit sequences produce identical block/offset assignments.
class DeviceArena {
public:
  struct Allocation {
    BufferRange range;
    bool created_block{};
    VkDeviceSize block_bytes{};
  };

  void Initialize(VkDevice device, VkPhysicalDevice physical_device,
      VkBufferUsageFlags usage,
      DeviceMemoryBudget* memory_budget,
      std::uint32_t graphics_queue_family,
      std::uint32_t transfer_queue_family,
      VkDeviceSize alignment = kArenaAlignment) {
    device_ = device;
    physical_device_ = physical_device;
    usage_ = usage;
    memory_budget_ = memory_budget;
    graphics_queue_family_ = graphics_queue_family;
    transfer_queue_family_ = transfer_queue_family;
    alignment_ = std::max<VkDeviceSize>(alignment, kArenaAlignment);
  }

  void Destroy() noexcept {
    for (auto& block : blocks_) {
      DestroyBufferRaw(device_, block.buffer, memory_budget_);
    }
    blocks_.clear();
  }

  Allocation Allocate(VkDeviceSize bytes) {
    const auto aligned = AlignUp(bytes, alignment_);
    for (std::uint32_t index = 0; index < blocks_.size(); ++index) {
      auto& spans = blocks_[index].free_spans;
      for (auto span = spans.begin(); span != spans.end(); ++span) {
        if (span->size < aligned) {
          continue;
        }
        const BufferRange range{index, span->offset, aligned};
        span->offset += aligned;
        span->size -= aligned;
        if (span->size == 0) {
          spans.erase(span);
        }
        RecordAllocation(false, aligned);
        return {range, false, 0};
      }
    }
    const auto block_bytes = std::max(kMinArenaBlockBytes, aligned);
    Block block;
    block.buffer = CreateBufferRaw(device_, physical_device_, block_bytes,
        usage_ | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        memory_budget_, graphics_queue_family_,
        transfer_queue_family_);
    if (block_bytes > aligned) {
      block.free_spans.push_back({aligned, block_bytes - aligned});
    }
    blocks_.push_back(std::move(block));
    RecordAllocation(true, aligned);
    return {{static_cast<std::uint32_t>(blocks_.size() - 1U), 0, aligned},
        true, block_bytes};
  }

  void Release(const BufferRange& range) noexcept {
    if (!range.valid()) {
      return;
    }
    auto& spans = blocks_[range.block].free_spans;
    const auto next = std::find_if(
        spans.begin(), spans.end(),
        [&](const FreeSpan& span) { return span.offset > range.offset; });
    const auto inserted = spans.insert(next, {range.offset, range.size});
    const auto following = inserted + 1;
    if (following != spans.end() &&
        inserted->offset + inserted->size == following->offset) {
      inserted->size += following->size;
      spans.erase(following);
    }
    if (inserted != spans.begin()) {
      const auto previous = inserted - 1;
      if (previous->offset + previous->size == inserted->offset) {
        previous->size += inserted->size;
        spans.erase(inserted);
      }
    }
    ++telemetry_.release_count;
    telemetry_.resident_bytes -= range.size;
    if (telemetry_.active_ranges != 0) {
      --telemetry_.active_ranges;
    }
  }

  [[nodiscard]] VkBuffer buffer(std::uint32_t block) const noexcept {
    return blocks_[block].buffer.handle;
  }

  [[nodiscard]] VkDeviceSize aligned_size(VkDeviceSize bytes) const noexcept {
    return AlignUp(bytes, alignment_);
  }

  [[nodiscard]] std::uint32_t block_count() const noexcept {
    return static_cast<std::uint32_t>(blocks_.size());
  }

  [[nodiscard]] ArenaTelemetry telemetry() const noexcept {
    auto result = telemetry_;
    result.blocks = static_cast<std::uint32_t>(blocks_.size());
    for (const auto& block : blocks_) {
      result.capacity_bytes += block.buffer.size;
      result.free_spans +=
          static_cast<std::uint32_t>(block.free_spans.size());
      for (const auto& span : block.free_spans) {
        result.free_bytes += span.size;
        result.largest_free_span_bytes =
            std::max(result.largest_free_span_bytes, span.size);
      }
    }
    return result;
  }

private:
  struct FreeSpan {
    VkDeviceSize offset{};
    VkDeviceSize size{};
  };

  struct Block {
    Buffer buffer;
    std::vector<FreeSpan> free_spans; // sorted by offset, adjacent-merged
  };

  void RecordAllocation(bool grew, VkDeviceSize bytes) noexcept {
    ++telemetry_.allocation_count;
    ++telemetry_.active_ranges;
    telemetry_.resident_bytes += bytes;
    telemetry_.peak_active_ranges =
        std::max(telemetry_.peak_active_ranges, telemetry_.active_ranges);
    if (grew) {
      ++telemetry_.growth_count;
    }
    telemetry_.peak_resident_bytes =
        std::max(telemetry_.peak_resident_bytes,
            telemetry_.resident_bytes);
  }

  VkDevice device_{};
  VkPhysicalDevice physical_device_{};
  VkBufferUsageFlags usage_{};
  DeviceMemoryBudget* memory_budget_{};
  std::uint32_t graphics_queue_family_{VK_QUEUE_FAMILY_IGNORED};
  std::uint32_t transfer_queue_family_{VK_QUEUE_FAMILY_IGNORED};
  VkDeviceSize alignment_{kArenaAlignment};
  std::vector<Block> blocks_;
  ArenaTelemetry telemetry_;
};

// Persistently mapped host-visible ring that feeds device-local copies. Each
// frame reserves one contiguous region; regions are recycled once the frame
// that consumed them reports completion. When capacity is insufficient the
// ring reallocates and hands the old buffer to the caller for deferred
// destruction.
class StagingRing {
public:
  struct Reservation {
    std::byte* mapped{};
    VkBuffer buffer{};
    VkDeviceSize offset{};
  };

  void Initialize(VkDevice device, VkPhysicalDevice physical_device,
      DeviceMemoryBudget* memory_budget) {
    device_ = device;
    physical_device_ = physical_device;
    memory_budget_ = memory_budget;
    // RendererOptions caps frame contexts at eight, so this prevents metadata
    // allocation failure after a queue submission has already succeeded.
    regions_.reserve(8);
    retired_regions_.reserve(8);
  }

  void Destroy() noexcept {
    if (buffer_.memory != VK_NULL_HANDLE) {
      vkUnmapMemory(device_, buffer_.memory);
    }
    DestroyBufferRaw(device_, buffer_, memory_budget_);
    mapped_ = nullptr;
    regions_.clear();
    retired_regions_.clear();
    pending_ = {};
  }

  // Reserves one region for the current frame. `retired` receives the previous
  // buffer when the ring grew; the caller must defer its destruction until
  // in-flight frames complete.
  Reservation Reserve(VkDeviceSize bytes, Buffer& retired,
      VkDeviceSize& growth_bytes) {
    if (pending_.active) {
      throw std::logic_error("staging region is already reserved this frame");
    }
    const auto aligned = AlignUp(bytes, kArenaAlignment);
    const auto previous_head = head_;
    VkDeviceSize offset{};
    if (!TryPlace(aligned, offset)) {
      const auto capacity =
          std::max({kMinStagingBytes, aligned, buffer_.size * 2U});
      // Once all regions have completed, the old ring has no payload that must
      // survive allocation of its replacement. Releasing it first avoids
      // requiring transient headroom beyond a configured VRAM limit.
      if (regions_.empty() && buffer_.memory != VK_NULL_HANDLE) {
        vkUnmapMemory(device_, buffer_.memory);
        DestroyBufferRaw(device_, buffer_, memory_budget_);
        mapped_ = nullptr;
        head_ = 0;
      }

      // Preserve an in-flight ring until the replacement is fully allocated
      // and mapped. A budget rejection therefore leaves the current ring
      // usable instead of losing its raw Vulkan handles on the exceptional
      // path.
      retired_regions_.reserve(retired_regions_.size() + regions_.size());
      auto replacement = CreateBufferRaw(
          device_, physical_device_, capacity,
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
          memory_budget_);
      void* replacement_mapped{};
      try {
        Check(vkMapMemory(device_, replacement.memory, 0, replacement.size, 0,
                  &replacement_mapped),
            "map staging ring");
      } catch (...) {
        DestroyBufferRaw(device_, replacement, memory_budget_);
        throw;
      }
      retired = buffer_;
      if (retired.memory != VK_NULL_HANDLE) {
        vkUnmapMemory(device_, retired.memory);
      }
      buffer_ = replacement;
      replacement = {};
      mapped_ = static_cast<std::byte*>(replacement_mapped);
      retired_regions_.insert(retired_regions_.end(), regions_.begin(),
          regions_.end());
      regions_.clear();
      head_ = 0;
      offset = 0;
      growth_bytes = capacity;
      ++telemetry_.growth_count;
      telemetry_.peak_capacity_bytes =
          std::max(telemetry_.peak_capacity_bytes, capacity);
      if (retired.handle != VK_NULL_HANDLE) {
        ++telemetry_.retired_buffers;
        telemetry_.retired_bytes += retired.size;
      }
    } else if (offset < previous_head) {
      ++telemetry_.wrap_count;
    }
    pending_ = {true, offset, offset + aligned};
    head_ = offset + aligned;
    ++telemetry_.reservation_count;
    telemetry_.reserved_bytes += aligned;
    RefreshPeakInFlight();
    return {mapped_ + offset, buffer_.handle, offset};
  }

  void FinishFrame(std::uint64_t completion_value) {
    if (!pending_.active) {
      return;
    }
    regions_.push_back({pending_.begin, pending_.end, completion_value});
    pending_ = {};
    telemetry_.peak_active_regions = std::max(
        telemetry_.peak_active_regions, ActiveRegionCount());
  }

  void AbandonFrame() noexcept {
    if (!pending_.active) {
      return;
    }
    // No command buffer consumed this reservation, so the tail can be reused.
    head_ = pending_.begin;
    pending_ = {};
  }

  void Collect(std::uint64_t completed) {
    while (!retired_regions_.empty() &&
           retired_regions_.front().completion <= completed) {
      retired_regions_.erase(retired_regions_.begin());
    }
    while (!regions_.empty() && regions_.front().completion <= completed) {
      regions_.erase(regions_.begin());
    }
    if (regions_.empty() && !pending_.active) {
      head_ = 0;
    }
  }

  [[nodiscard]] UploadRingTelemetry telemetry() const noexcept {
    auto result = telemetry_;
    result.capacity_bytes = buffer_.size;
    result.active_regions = ActiveRegionCount();
    for (const auto& region : retired_regions_) {
      result.in_flight_bytes += region.end - region.begin;
    }
    for (const auto& region : regions_) {
      result.in_flight_bytes += region.end - region.begin;
    }
    if (pending_.active) {
      result.in_flight_bytes += pending_.end - pending_.begin;
    }
    return result;
  }

private:
  struct Region {
    VkDeviceSize begin{};
    VkDeviceSize end{};
    std::uint64_t completion{};
  };

  struct PendingRegion {
    bool active{};
    VkDeviceSize begin{};
    VkDeviceSize end{};
  };

  [[nodiscard]] std::uint32_t ActiveRegionCount() const noexcept {
    return static_cast<std::uint32_t>(retired_regions_.size() +
                                      regions_.size());
  }

  bool TryPlace(VkDeviceSize bytes, VkDeviceSize& offset) {
    if (buffer_.handle == VK_NULL_HANDLE || bytes > buffer_.size) {
      return false;
    }
    if (regions_.empty()) {
      offset = 0;
      return true;
    }
    const auto tail = regions_.front().begin;
    if (head_ >= tail) {
      if (buffer_.size - head_ >= bytes) {
        offset = head_;
        return true;
      }
      if (tail >= bytes) {
        offset = 0;
        return true;
      }
      return false;
    }
    if (tail - head_ >= bytes) {
      offset = head_;
      return true;
    }
    return false;
  }

  void RefreshPeakInFlight() noexcept {
    telemetry_.peak_in_flight_bytes =
        std::max(telemetry_.peak_in_flight_bytes,
            telemetry().in_flight_bytes);
  }

  VkDevice device_{};
  VkPhysicalDevice physical_device_{};
  DeviceMemoryBudget* memory_budget_{};
  Buffer buffer_;
  std::byte* mapped_{};
  VkDeviceSize head_{};
  std::vector<Region> regions_;
  std::vector<Region> retired_regions_;
  PendingRegion pending_;
  UploadRingTelemetry telemetry_;
};

} // namespace detail
} // namespace merlin::vulkan
