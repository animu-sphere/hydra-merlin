#include <merlin/vulkan/renderer.hpp>
#include "environment_lighting.hpp"
#include "gaussian_preparation.hpp"

#include <merlin/vulkan/shader_abi.hpp>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cmath>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace merlin::vulkan {

std::string_view RendererErrorCodeName(RendererErrorCode code) noexcept {
  switch (code) {
    case RendererErrorCode::InvalidRequest: return "invalid-request";
    case RendererErrorCode::InvalidToken: return "invalid-token";
    case RendererErrorCode::ResourceBusy: return "resource-busy";
    case RendererErrorCode::Timeout: return "timeout";
    case RendererErrorCode::DeviceLost: return "device-lost";
    case RendererErrorCode::ResourceExhausted: return "resource-exhausted";
    case RendererErrorCode::Unsupported: return "unsupported";
    case RendererErrorCode::BackendFailure: return "backend-failure";
  }
  return "unknown";
}

RendererError::RendererError(RendererErrorCode code, std::string operation,
                             std::string detail, std::int32_t native_code)
    : std::runtime_error(std::string(RendererErrorCodeName(code)) + ": " +
                         operation + ": " + detail),
      code_(code),
      operation_(std::move(operation)),
      detail_(std::move(detail)),
      native_code_(native_code) {}

namespace {

using CpuClock = std::chrono::steady_clock;

std::atomic<std::uint64_t> g_renderer_owner{1};

std::uint64_t ElapsedNanoseconds(CpuClock::time_point start) {
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

  [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
  [[nodiscard]] bool empty() const noexcept { return records_.empty(); }
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

  [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
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
const T* FindDeltaRecord(const extraction::PersistentTable<T>& table,
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
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat kIdFormat = VK_FORMAT_R32_UINT;

void Check(VkResult result, const char* operation) {
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

std::vector<std::uint32_t> ReadSpirv(const std::filesystem::path& path) {
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

bool HasLayer(const char* name) {
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

bool HasInstanceExtension(const char* name) {
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

bool HasDeviceExtension(VkPhysicalDevice device, const char* name) {
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

Mat4 Multiply(const Mat4& lhs, const Mat4& rhs) {
  Mat4 result;
  result.values.fill(0.0F);
  for (std::size_t column = 0; column < 4; ++column) {
    for (std::size_t row = 0; row < 4; ++row) {
      for (std::size_t inner = 0; inner < 4; ++inner) {
        result.values[column * 4 + row] +=
            lhs.values[inner * 4 + row] * rhs.values[column * 4 + inner];
      }
    }
  }
  return result;
}

std::array<Vec4, 3> NormalMatrix(const Mat4& transform) {
  const Vec3 column0{transform.values[0], transform.values[1],
                     transform.values[2]};
  const Vec3 column1{transform.values[4], transform.values[5],
                     transform.values[6]};
  const Vec3 column2{transform.values[8], transform.values[9],
                     transform.values[10]};
  const auto cross = [](const Vec3& lhs, const Vec3& rhs) {
    return Vec3{lhs.y * rhs.z - lhs.z * rhs.y,
                lhs.z * rhs.x - lhs.x * rhs.z,
                lhs.x * rhs.y - lhs.y * rhs.x};
  };
  const auto cofactor0 = cross(column1, column2);
  const auto cofactor1 = cross(column2, column0);
  const auto cofactor2 = cross(column0, column1);
  const auto determinant = column0.x * cofactor0.x +
                           column0.y * cofactor0.y +
                           column0.z * cofactor0.z;
  if (std::abs(determinant) <= 1.0e-20F) {
    return {Vec4{1.0F, 0.0F, 0.0F, 0.0F},
            Vec4{0.0F, 1.0F, 0.0F, 0.0F},
            Vec4{0.0F, 0.0F, 1.0F, 0.0F}};
  }
  const auto inverse_determinant = 1.0F / determinant;
  const auto column = [inverse_determinant](const Vec3& value) {
    return Vec4{value.x * inverse_determinant,
                value.y * inverse_determinant,
                value.z * inverse_determinant, 0.0F};
  };
  return {column(cofactor0), column(cofactor1), column(cofactor2)};
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

constexpr VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment) {
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

std::uint32_t FindMemoryTypeRaw(VkPhysicalDevice physical_device,
                                std::uint32_t bits,
                                VkMemoryPropertyFlags properties) {
  VkPhysicalDeviceMemoryProperties memory{};
  vkGetPhysicalDeviceMemoryProperties(physical_device, &memory);
  for (std::uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
    if ((bits & (1U << index)) != 0U &&
        (memory.memoryTypes[index].propertyFlags & properties) == properties) {
      return index;
    }
  }
  throw std::runtime_error("no compatible Vulkan memory type");
}

void DestroyBufferRaw(VkDevice device, Buffer& buffer,
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

Buffer CreateBufferRaw(VkDevice device, VkPhysicalDevice physical_device,
                       VkDeviceSize size, VkBufferUsageFlags usage,
                       VkMemoryPropertyFlags properties,
                       DeviceMemoryBudget* memory_budget = nullptr,
                       std::uint32_t first_queue_family = VK_QUEUE_FAMILY_IGNORED,
                       std::uint32_t second_queue_family = VK_QUEUE_FAMILY_IGNORED) {
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
  const auto memory_type =
      FindMemoryTypeRaw(physical_device, requirements.memoryTypeBits, properties);
  try {
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

  [[nodiscard]] bool valid() const noexcept { return block != kInvalidBlock; }
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
                  std::uint32_t transfer_queue_family) {
    device_ = device;
    physical_device_ = physical_device;
    usage_ = usage;
    memory_budget_ = memory_budget;
    graphics_queue_family_ = graphics_queue_family;
    transfer_queue_family_ = transfer_queue_family;
  }

  void Destroy() noexcept {
    for (auto& block : blocks_) {
      DestroyBufferRaw(device_, block.buffer, memory_budget_);
    }
    blocks_.clear();
  }

  Allocation Allocate(VkDeviceSize bytes) {
    const auto aligned = AlignUp(bytes, kArenaAlignment);
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
    std::vector<FreeSpan> free_spans;  // sorted by offset, adjacent-merged
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

}  // namespace

class Renderer::Impl {
 public:
  explicit Impl(RendererOptions options) {
    diagnostic_sink_ = options.diagnostic_sink;
    if (options.frames_in_flight < 2 || options.frames_in_flight > 8) {
      throw RendererError(RendererErrorCode::InvalidRequest,
                          "create renderer",
                          "frames_in_flight must be between 2 and 8");
    }
    if (options.descriptor_backend !=
            DescriptorBackendRequest::Conventional &&
        options.bindless_sampler_capacity > (1U << 16U)) {
      throw RendererError(RendererErrorCode::InvalidRequest,
                          "create renderer",
                          "bindless_sampler_capacity must not exceed 65536");
    }
    if (options.borrowed_context && options.presentation) {
      throw RendererError(
          RendererErrorCode::InvalidRequest, "create renderer",
          "a borrowed Vulkan context cannot own native presentation");
    }
    if (options.borrowed_context &&
        options.descriptor_backend == DescriptorBackendRequest::Bindless) {
      throw RendererError(
          RendererErrorCode::Unsupported, "create renderer",
          "a borrowed Vulkan context initially supports only conventional "
          "descriptors");
    }
    if (options.presentation) {
      if (options.presentation->create_surface == nullptr) {
        throw RendererError(RendererErrorCode::InvalidRequest,
                            "create presentation target",
                            "presentation surface callback is null");
      }
      presentation_vsync_ = options.presentation->vsync;
      presentation_overlay_user_data_ =
          options.presentation->overlay_user_data;
      presentation_overlay_ = options.presentation->render_overlay;
    }
    for (auto& artifact : options.generated_material_artifacts) {
      if (artifact.module_key.empty() || artifact.fragment.empty() ||
          artifact.fragment_entry_point.empty() ||
          artifact.parameter_buffer_size == 0U ||
          artifact.reflection.target.empty()) {
        throw RendererError(
            RendererErrorCode::InvalidRequest, "register material artifact",
            "generated material artifact metadata is incomplete");
      }
      if (!generated_material_artifacts_
               .emplace(artifact.module_key, std::move(artifact))
               .second) {
        throw RendererError(RendererErrorCode::InvalidRequest,
                            "register material artifact",
                            "generated material module key is duplicated");
      }
    }
    try {
      CreateDevice(options);
      capabilities_.generated_materials =
          !generated_material_artifacts_.empty() &&
          capabilities_.descriptor_indexing_selection.selected_backend ==
              DescriptorBackend::Conventional;
      memory_budget_.Initialize(physical_device_, device_,
                                capabilities_.memory_budget_extension,
                                options.vram_limit_bytes);
      const auto& initial_memory = memory_budget_.telemetry();
      capabilities_.device_local_heap_capacity_bytes =
          initial_memory.heap_capacity_bytes;
      capabilities_.device_local_heap_budget_bytes =
          initial_memory.heap_budget_bytes;
      capabilities_.device_local_heap_usage_bytes =
          initial_memory.heap_usage_bytes;
      capabilities_.configured_vram_limit_bytes = options.vram_limit_bytes;
      if (capabilities_.descriptor_indexing_selection.selected_backend ==
          DescriptorBackend::Bindless) {
        const auto& selection = capabilities_.descriptor_indexing_selection;
        bindless_texture_table_ =
            std::make_unique<BindlessTextureTable>(selection.texture_capacity);
        bindless_sampler_table_ =
            std::make_unique<BindlessSamplerTable>(selection.sampler_capacity);
        bindless_texture_views_.resize(selection.texture_capacity);
        bindless_samplers_.resize(selection.sampler_capacity);
        reserved_bindless_textures_.resize(kReservedBindlessTextureSlots);
        statistics_.bindless_resource_tables = true;
      }
      CreateFrameContexts(options.frames_in_flight);
      statistics_.frame_context_count = options.frames_in_flight;
      vertex_arena_.Initialize(
          device_, physical_device_, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
          &memory_budget_, queue_family_, transfer_queue_family_);
      index_arena_.Initialize(
          device_, physical_device_, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
          &memory_budget_, queue_family_, transfer_queue_family_);
      constexpr auto gaussian_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      gaussian_position_arena_.Initialize(device_, physical_device_,
          gaussian_usage, &memory_budget_, queue_family_, transfer_queue_family_);
      gaussian_covariance_arena_.Initialize(device_, physical_device_,
          gaussian_usage, &memory_budget_, queue_family_, transfer_queue_family_);
      gaussian_opacity_arena_.Initialize(device_, physical_device_,
          gaussian_usage, &memory_budget_, queue_family_, transfer_queue_family_);
      gaussian_radiance_arena_.Initialize(device_, physical_device_,
          gaussian_usage, &memory_budget_, queue_family_, transfer_queue_family_);
      staging_.Initialize(device_, physical_device_, &memory_budget_);
      gaussian_staging_.Initialize(device_, physical_device_, &memory_budget_);
      gpu_scene_staging_.Initialize(device_, physical_device_,
                                    &memory_budget_);
      gpu_driven_staging_.Initialize(device_, physical_device_,
                                     &memory_budget_);
      if (options.gpu_scene_capacities) {
        CreateGpuSceneBuffers(*options.gpu_scene_capacities);
      }
      CreateGaussianCornerBuffer();
    } catch (...) {
      Destroy(false);
      throw;
    }
  }

  ~Impl() { Destroy(true); }

  void Destroy(bool wait_for_submissions) noexcept {
    if (device_ != VK_NULL_HANDLE) {
      if (wait_for_submissions && owns_vulkan_context_) {
        (void)vkDeviceWaitIdle(device_);
      } else if (wait_for_submissions) {
        // Waiting only for Merlin submissions avoids idling unrelated Hgi or
        // host work on the application-owned device.
        for (auto& frame : frames_) {
          if (frame.outstanding) {
            try {
              WaitForFrame(frame, std::chrono::nanoseconds::max());
            } catch (const std::exception&) {
              // Destructors cannot report a device-loss-style failure. Keep
              // teardown best-effort while never escalating to device idle.
            }
          }
        }
      }
      ShutdownPresentationOverlay();
      for (auto& retired : deferred_) {
        DestroyBuffer(retired.buffer);
      }
      deferred_.clear();
      for (auto& retired : retired_textures_) {
        DestroyTexture(retired.texture);
      }
      retired_textures_.clear();
      for (const auto& retired : retired_samplers_) {
        vkDestroySampler(device_, retired.sampler, nullptr);
        if (retired.bindless_slot &&
            retired.bindless_slot.index < bindless_samplers_.size() &&
            bindless_samplers_[retired.bindless_slot.index] ==
                retired.sampler) {
          bindless_samplers_[retired.bindless_slot.index] = VK_NULL_HANDLE;
        }
      }
      retired_samplers_.clear();
      for (auto& [handle, texture] : texture_slots_) {
        (void)handle;
        DestroyTexture(texture);
      }
      texture_slots_.clear();
      if (bindless_sampler_table_) {
        for (const auto sampler : bindless_samplers_) {
          if (sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device_, sampler, nullptr);
          }
        }
      } else {
        for (const auto& [handle, sampler] : sampler_slots_) {
          (void)handle;
          vkDestroySampler(device_, sampler.sampler, nullptr);
        }
      }
      sampler_slots_.clear();
      bindless_samplers_.clear();
      bindless_texture_views_.clear();
      for (auto& texture : reserved_bindless_textures_) {
        DestroyTexture(texture);
      }
      reserved_bindless_textures_.clear();
      DestroyTexture(fallback_texture_);
      if (fallback_sampler_.sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device_, fallback_sampler_.sampler, nullptr);
      }
      for (auto& buffer : frame_upload_buffers_) {
        DestroyBuffer(buffer);
      }
      frame_upload_buffers_.clear();
      retired_ranges_.clear();
      staging_.Destroy();
      gaussian_staging_.Destroy();
      gpu_scene_staging_.Destroy();
      gpu_driven_staging_.Destroy();
      gaussian_position_arena_.Destroy();
      gaussian_covariance_arena_.Destroy();
      gaussian_opacity_arena_.Destroy();
      gaussian_radiance_arena_.Destroy();
      vertex_arena_.Destroy();
      index_arena_.Destroy();
      DestroyGpuSceneBuffers();
      DestroyBuffer(gaussian_corner_vertices_);
      for (auto& frame : frames_) {
        DestroyTarget(frame.target);
        DestroyBuffer(frame.material_uniforms);
        DestroyBuffer(frame.generated_parameter_uniforms);
        DestroyBuffer(frame.gaussian_instances);
        DestroyGpuDrivenFrameResources(frame.gpu_driven);
        if (frame.image_available != VK_NULL_HANDLE) {
          vkDestroySemaphore(device_, frame.image_available, nullptr);
        }
        if (frame.timestamp_pool != VK_NULL_HANDLE) {
          vkDestroyQueryPool(device_, frame.timestamp_pool, nullptr);
        }
        if (frame.descriptor_pool != VK_NULL_HANDLE) {
          vkDestroyDescriptorPool(device_, frame.descriptor_pool, nullptr);
        }
        if (frame.fence != VK_NULL_HANDLE) {
          vkDestroyFence(device_, frame.fence, nullptr);
        }
        if (frame.command_pool != VK_NULL_HANDLE) {
          vkDestroyCommandPool(device_, frame.command_pool, nullptr);
        }
        if (frame.transfer_command_pool != VK_NULL_HANDLE) {
          vkDestroyCommandPool(device_, frame.transfer_command_pool, nullptr);
        }
      }
      if (bindless_descriptor_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, bindless_descriptor_pool_, nullptr);
      }
      for (const auto& [path, module] : shader_modules_) {
        (void)path;
        vkDestroyShaderModule(device_, module, nullptr);
      }
      shader_modules_.clear();
      if (descriptor_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
      }
      for (const auto& [key, layout] : generated_descriptor_set_layouts_) {
        (void)key;
        vkDestroyDescriptorSetLayout(device_, layout, nullptr);
      }
      generated_descriptor_set_layouts_.clear();
      if (bindless_descriptor_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, bindless_descriptor_set_layout_,
                                     nullptr);
      }
      if (bindless_material_descriptor_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(
            device_, bindless_material_descriptor_set_layout_, nullptr);
      }
      for (const auto& [path, pipeline] : gpu_driven_pipelines_) {
        (void)path;
        vkDestroyPipeline(device_, pipeline, nullptr);
      }
      gpu_driven_pipelines_.clear();
      if (gpu_driven_pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, gpu_driven_pipeline_layout_, nullptr);
      }
      if (gpu_driven_descriptor_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_,
                                     gpu_driven_descriptor_set_layout_,
                                     nullptr);
      }
      if (timeline_semaphore_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, timeline_semaphore_, nullptr);
      }
      if (transfer_timeline_semaphore_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, transfer_timeline_semaphore_, nullptr);
      }
      DestroySwapchain();
      if (owns_vulkan_context_) {
        vkDestroyDevice(device_, nullptr);
      }
    }
    if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
      vkDestroySurfaceKHR(instance_, surface_, nullptr);
    }
    if (debug_messenger_ != VK_NULL_HANDLE) {
      const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
          vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
      if (destroy != nullptr) {
        destroy(instance_, debug_messenger_, nullptr);
      }
    }
    if (owns_vulkan_context_ && instance_ != VK_NULL_HANDLE) {
      vkDestroyInstance(instance_, nullptr);
    }
  }

  std::uint64_t Submit(const RenderRequest& request) {
    ValidateRequest(request);
    const auto backend_start = CpuClock::now();
    frame_counters_ = {};
    ResetAbandonedUploads();
    EnsureEnvironmentLighting(request.shaders.environment);

    auto rendered_aovs = RenderedAovs(request);
    auto cpu_readback_aovs = CpuReadbackAovs(request);
    const auto aov_mask = [](const auto& aovs) {
      std::uint64_t mask{};
      for (const auto aov : aovs) {
        mask |= std::uint64_t{1} << static_cast<std::uint32_t>(aov);
      }
      return mask;
    };
    frame_counters_.requested_aov_count = request.products.size();
    for (const auto& product : request.products) {
      frame_counters_.requested_aov_mask |=
          std::uint64_t{1} << static_cast<std::uint32_t>(product.aov);
    }
    frame_counters_.rendered_aov_count = rendered_aovs.size();
    frame_counters_.rendered_aov_mask = aov_mask(rendered_aovs);
    frame_counters_.cpu_readback_aov_count = cpu_readback_aovs.size();
    frame_counters_.cpu_readback_aov_mask = aov_mask(cpu_readback_aovs);
    if (bindless_texture_table_) {
      EnsureBindlessDescriptorSet();
      EnsureBindlessMaterialDescriptorSetLayout();
    } else {
      EnsureDescriptorSetLayout();
      EnsureGeneratedDescriptorSetLayouts();
    }
    auto& frame = AcquireFrame(request.width, request.height, request.shaders,
                               cpu_readback_aovs);
    frame.scene_revision = request.snapshot->revision;
    frame.rendered_aovs = std::move(rendered_aovs);
    frame.cpu_readback_aovs = std::move(cpu_readback_aovs);
    active_target_ = &frame.target;
    struct ResetActiveTarget {
      RenderTarget*& target;
      ~ResetActiveTarget() { target = nullptr; }
    } reset_active_target{active_target_};
    EnsureTarget(frame.target, request.width, request.height, request.shaders,
                  frame.cpu_readback_aovs);
    if (request.present) {
      PreparePresentation(frame, request.width, request.height);
    } else {
      frame.present_pending = false;
    }
    // The acquired image and its signaled acquire semaphore are only consumed
    // by a successful graphics submission; reclaim both on earlier failures.
    struct ReclaimPresentationOnError {
      Impl& impl;
      FrameContext& frame;
      bool armed;
      ~ReclaimPresentationOnError() {
        if (armed) {
          impl.ReclaimAcquiredImage(frame);
        }
      }
    } reclaim_presentation{*this, frame, frame.present_pending};

    const auto upload_start = CpuClock::now();
    std::uint64_t gaussian_preparation_ns{};
    std::uint64_t gaussian_attribute_upload_ns{};
    std::uint64_t gaussian_prepared_upload_ns{};
    geometry_records_.Sync(request.snapshot->geometries);
    texture_records_.Sync(request.snapshot->textures);
    sampler_records_.Sync(request.snapshot->samplers);
    material_records_.Sync(request.snapshot->materials);
    instance_records_.Sync(request.snapshot->instances);
    draw_records_.Sync(request.snapshot->draws);
    const bool gaussian_preparation_cache_hit =
        gaussian_preparation_cache_valid_ &&
        gaussian_preparation_source_.table_identity() ==
            request.snapshot->gaussians.table_identity() &&
        gaussian_preparation_view_.values == request.snapshot->view.values &&
        gaussian_preparation_projection_.values ==
            request.snapshot->projection.values &&
        gaussian_preparation_width_ == request.width &&
        gaussian_preparation_height_ == request.height;
    if (gaussian_preparation_cache_hit) {
      ++frame_counters_.gaussian_preparation_cache_hits;
    } else {
      const auto gaussian_preparation_start = CpuClock::now();
      prepared_gaussians_ = detail::PrepareGaussianFrame(
          *request.snapshot, {request.width, request.height});
      gaussian_preparation_ns = ElapsedNanoseconds(gaussian_preparation_start);
      gaussian_preparation_source_ = request.snapshot->gaussians;
      gaussian_preparation_view_ = request.snapshot->view;
      gaussian_preparation_projection_ = request.snapshot->projection;
      gaussian_preparation_width_ = request.width;
      gaussian_preparation_height_ = request.height;
      gaussian_preparation_cache_valid_ = true;
      ++gaussian_preparation_generation_;
      if (gaussian_preparation_generation_ == 0) {
        ++gaussian_preparation_generation_;
        for (auto& candidate_frame : frames_) {
          candidate_frame.gaussian_preparation_generation = 0;
        }
      }
      ++frame_counters_.gaussian_preparation_cache_misses;
    }
    frame_counters_.gaussian_candidate_count =
        prepared_gaussians_.counters.candidate_count;
    frame_counters_.gaussian_visible_count =
        prepared_gaussians_.counters.visible_count;
    frame_counters_.gaussian_hidden_count =
        prepared_gaussians_.counters.hidden_count;
    frame_counters_.gaussian_opacity_culled_count =
        prepared_gaussians_.counters.opacity_culled_count;
    frame_counters_.gaussian_frustum_culled_count =
        prepared_gaussians_.counters.frustum_culled_count;
    frame_counters_.gaussian_invalid_culled_count =
        prepared_gaussians_.counters.invalid_culled_count;
    frame_counters_.gaussian_sorted_count =
        prepared_gaussians_.counters.sorted_count;
    frame_counters_.gaussian_sorting_policy_fallback_count =
        prepared_gaussians_.counters.sorting_policy_fallback_count;
    SelectGeneratedMaterials();
    PreflightGeneratedMaterialPipelines(*request.snapshot);
    for (std::size_t i = 0; i < draw_records_.size(); ++i) {
      const auto& draw = draw_records_[i];
      ++frame_counters_.draw_count;
      ++frame_counters_.visible_primitive_count;
      frame_counters_.triangle_count +=
          geometry_records_[draw.geometry_index].indices->size() / 3U;
    }
    const auto resource_sync_mode =
        SelectResourceSyncMode(*request.snapshot);
    // Resource sync mutates residency before queue submission. Leave this set
    // on every exceptional exit so the next valid request replaces the
    // possibly mixed state instead of selecting the unchanged fast path.
    resource_residency_dirty_ = true;
    SyncGeometry(*request.snapshot, resource_sync_mode);
    SyncTextures(*request.snapshot, resource_sync_mode);
    SyncSamplers(*request.snapshot, resource_sync_mode);
    PrepareMaterialDescriptors(frame, *request.snapshot);
    PrepareBindlessDescriptors();
    const auto gaussian_attribute_upload_start = CpuClock::now();
    SyncGaussianAttributes(*request.snapshot, resource_sync_mode);
    gaussian_attribute_upload_ns =
        ElapsedNanoseconds(gaussian_attribute_upload_start);
    const auto gaussian_prepared_upload_start = CpuClock::now();
    PrepareGaussianInstances(frame);
    gaussian_prepared_upload_ns =
        ElapsedNanoseconds(gaussian_prepared_upload_start);
    StageGpuSceneUpdate(*request.snapshot, request.gpu_scene_update.get());
    PrepareGpuDrivenIndexed(frame, *request.snapshot, request);
    if (frame_counters_.upload_bytes != 0) {
      ++statistics_.scene_uploads;
    }
    const auto upload_ns = ElapsedNanoseconds(upload_start);

    const auto recording_start = CpuClock::now();
    Check(vkResetCommandPool(device_, frame.command_pool, 0),
          "reset frame command pool");
    const bool asynchronous_upload =
        capabilities_.async_transfer_queue &&
        (!pending_copies_.empty() || !pending_image_copies_.empty());
    if (asynchronous_upload) {
      Check(vkResetCommandPool(device_, frame.transfer_command_pool, 0),
            "reset transfer command pool");
      VkCommandBufferBeginInfo transfer_begin{
          VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      transfer_begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      Check(vkBeginCommandBuffer(frame.transfer_command_buffer,
                                 &transfer_begin),
            "begin transfer command buffer");
      RecordUploads(frame.transfer_command_buffer, true);
      Check(vkEndCommandBuffer(frame.transfer_command_buffer),
            "end transfer command buffer");
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(frame.command_buffer, &begin),
          "begin frame command buffer");
    if (frame.timestamp_pool != VK_NULL_HANDLE) {
      vkCmdResetQueryPool(frame.command_buffer, frame.timestamp_pool, 0, 4);
      vkCmdWriteTimestamp(frame.command_buffer,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          frame.timestamp_pool, 0);
    }
    if (asynchronous_upload) {
      RecordUploadAcquires(frame.command_buffer);
    } else {
      RecordUploads(frame.command_buffer, false);
    }
    RecordGpuDrivenDispatch(frame.command_buffer, frame, *request.snapshot,
                            request.gpu_driven_indexed);
    RecordFrame(frame.command_buffer, frame, *request.snapshot,
                 request.clear_color,
                 frame.cpu_readback_aovs);
    if (frame.timestamp_pool != VK_NULL_HANDLE) {
      vkCmdWriteTimestamp(frame.command_buffer,
                          VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                          frame.timestamp_pool, 1);
    }
    Check(vkEndCommandBuffer(frame.command_buffer), "end frame command buffer");
    const auto recording_ns = ElapsedNanoseconds(recording_start);

    frame.cpu_timings = {};
    frame.cpu_timings.upload_ns = upload_ns;
    frame.cpu_timings.gaussian_preparation_ns = gaussian_preparation_ns;
    frame.cpu_timings.gaussian_attribute_upload_ns =
        gaussian_attribute_upload_ns;
    frame.cpu_timings.gaussian_prepared_upload_ns =
        gaussian_prepared_upload_ns;
    frame.cpu_timings.command_recording_ns = recording_ns;
    // Keep every operation after successful queue submission non-allocating.
    deferred_.reserve(deferred_.size() + frame_upload_buffers_.size());
    const auto submission_start = CpuClock::now();
    const auto transfer_completion =
        asynchronous_upload ? SubmitTransfers(frame) : 0;
    std::uint64_t completion{};
    try {
      completion = SubmitFrame(frame, transfer_completion);
    } catch (...) {
      if (transfer_completion != 0) {
        VkSemaphoreWaitInfo wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wait_info.semaphoreCount = 1;
        wait_info.pSemaphores = &transfer_timeline_semaphore_;
        wait_info.pValues = &transfer_completion;
        (void)vkWaitSemaphores(device_, &wait_info,
                               std::numeric_limits<std::uint64_t>::max());
        if (frame_counters_.gpu_scene_copy_range_count != 0) {
          // The completed transfer may have replaced slots that the resident
          // CPU revision still names. Do not reuse that revision after the
          // graphics submission which would have committed it fails.
          InvalidateGpuSceneUpdate();
        }
        if (HasPendingGpuDrivenCandidateUpload(frame)) {
          // The transfer has already replaced the device-local candidate
          // list, but the failed graphics submission did not publish its
          // shadow. Force the next request to upload whichever sequence it
          // selects instead of comparing against stale CPU state.
          InvalidateGpuDrivenCandidates(frame);
        }
      }
      throw;
    }
    reclaim_presentation.armed = false;
    frame.cpu_timings.queue_submission_ns =
        ElapsedNanoseconds(submission_start);
    frame.counters = frame_counters_;
    frame.material_diagnostics = std::move(frame_material_diagnostics_);
    CommitTextureUploads();
    CommitResourceSnapshot(*request.snapshot);
    CommitGpuSceneUpdate();
    CommitGpuDrivenCandidates(frame);
    resource_residency_dirty_ = false;
    for (auto& buffer : frame_upload_buffers_) {
      deferred_.push_back({buffer, completion});
      buffer = {};
    }
    frame_upload_buffers_.clear();
    staging_.FinishFrame(completion);
    gaussian_staging_.FinishFrame(completion);
    gpu_scene_staging_.FinishFrame(completion);
    gpu_driven_staging_.FinishFrame(completion);
    frame.outstanding = true;
    ++statistics_.frames_submitted;
    if (frame.present_pending) {
      const auto presentation_start = CpuClock::now();
      try {
        PresentFrame(frame);
      } catch (...) {
        // Queue submission succeeded, so the staging reservations cannot be
        // abandoned or reused just because presentation failed. No completion
        // token can be returned on this path; wait and retire it here instead.
        WaitForFrame(frame, std::chrono::nanoseconds::max());
        latest_completed_value_ =
            std::max(latest_completed_value_, completion);
        staging_.Collect(completion);
        gaussian_staging_.Collect(completion);
        gpu_scene_staging_.Collect(completion);
        gpu_driven_staging_.Collect(completion);
        CollectDeferred(completion);
        frame.outstanding = false;
        frame.counters = {};
        frame.material_diagnostics.clear();
        throw;
      }
      frame.cpu_timings.presentation_ns =
          ElapsedNanoseconds(presentation_start);
    }
    frame.cpu_timings.backend_total_ns = ElapsedNanoseconds(backend_start);
    return completion;
  }

  bool IsComplete(std::uint64_t completion) const {
    const auto& frame = FindFrame(completion);
    if (timeline_semaphore_ != VK_NULL_HANDLE) {
      std::uint64_t value{};
      Check(vkGetSemaphoreCounterValue(device_, timeline_semaphore_, &value),
            "query frame completion");
      return value >= completion;
    }
    const auto status = vkGetFenceStatus(device_, frame.fence);
    if (status == VK_NOT_READY) {
      return false;
    }
    Check(status, "query frame fence");
    return true;
  }

  AovImageExport AcquireAovImage(std::uint64_t completion, Aov aov) {
    auto& frame = FindFrame(completion);
    if (!HasAov(frame.rendered_aovs, aov)) {
      throw RendererError(RendererErrorCode::InvalidRequest,
                          "acquire AOV image",
                          "AOV was not selected for this submission");
    }
    const auto mask = std::uint64_t{1} << static_cast<std::uint32_t>(aov);
    if ((frame.exported_aov_mask & mask) != 0) {
      throw RendererError(RendererErrorCode::ResourceBusy,
                          "acquire AOV image",
                          "AOV image already has an active export lease");
    }

    VkImage image{};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkImageAspectFlags aspect{};
    VkImageUsageFlags usage{};
    switch (aov) {
      case Aov::Color:
        image = frame.target.color;
        format = kColorFormat;
        aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT;
        break;
      case Aov::Depth:
        image = frame.target.depth;
        format = kDepthFormat;
        aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        break;
      case Aov::PrimId:
        image = frame.target.prim_id;
        format = kIdFormat;
        aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        break;
      case Aov::InstanceId:
        image = frame.target.instance_id;
        format = kIdFormat;
        aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        break;
      default:
        throw RendererError(RendererErrorCode::Unsupported,
                            "acquire AOV image",
                            "AOV has no Vulkan export image");
    }
    if (image == VK_NULL_HANDLE || format == VK_FORMAT_UNDEFINED) {
      throw RendererError(RendererErrorCode::BackendFailure,
                          "acquire AOV image",
                          "selected AOV image is unavailable");
    }

    frame.exported_aov_mask |= mask;
    ++frame.counters.aov_image_export_count;
    ++aov_image_export_count_;
    ++active_aov_image_leases_;

    AovImageExport result;
    result.product =
        MakeRenderProduct(frame.target.width, frame.target.height, aov);
    result.physical_device = EncodeHandle(physical_device_);
    result.device = EncodeHandle(device_);
    result.image = EncodeHandle(image);
    result.native_format = static_cast<std::uint32_t>(format);
    result.native_layout =
        static_cast<std::uint32_t>(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    result.native_stage_mask =
        static_cast<std::uint32_t>(VK_PIPELINE_STAGE_TRANSFER_BIT);
    result.native_access_mask =
        static_cast<std::uint32_t>(VK_ACCESS_TRANSFER_READ_BIT);
    result.native_aspect_mask = static_cast<std::uint32_t>(aspect);
    result.native_usage_mask = static_cast<std::uint32_t>(usage);
    result.native_tiling = static_cast<std::uint32_t>(VK_IMAGE_TILING_OPTIMAL);
    result.native_memory_property_mask =
        static_cast<std::uint32_t>(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    result.native_sharing_mode =
        static_cast<std::uint32_t>(VK_SHARING_MODE_EXCLUSIVE);
    result.queue_family = queue_family_;
    result.renderer_completion = completion;
    return result;
  }

  void ReleaseAovImage(std::uint64_t completion, Aov aov) {
    const auto found = std::find_if(
        frames_.begin(), frames_.end(), [&](const FrameContext& frame) {
          return frame.completion_value == completion;
        });
    if (found == frames_.end()) {
      throw RendererError(RendererErrorCode::InvalidToken,
                          "release AOV image",
                          "lease completion is unknown");
    }
    const auto mask =
        std::uint64_t{1} << static_cast<std::uint32_t>(aov);
    if ((found->exported_aov_mask & mask) == 0) {
      throw RendererError(RendererErrorCode::InvalidToken,
                          "release AOV image",
                          "lease is unknown or already released");
    }
    found->exported_aov_mask &= ~mask;
    if (active_aov_image_leases_ != 0) {
      --active_aov_image_leases_;
    }
  }

  RenderResult Resolve(std::uint64_t completion,
                       std::chrono::nanoseconds timeout) {
    auto& frame = FindFrame(completion);
    const auto resolve_start = CpuClock::now();
    const auto wait_start = CpuClock::now();
    WaitForFrame(frame, timeout);
    const auto wait_ns = ElapsedNanoseconds(wait_start);
    frame_counters_ = frame.counters;
    ResolveGpuDrivenCounters(frame);
    latest_completed_value_ = std::max(latest_completed_value_, completion);
    staging_.Collect(completion);
    gaussian_staging_.Collect(completion);
    gpu_scene_staging_.Collect(completion);
    gpu_driven_staging_.Collect(completion);
    CollectDeferred(completion);
    active_target_ = &frame.target;
    struct ResetActiveTarget {
      RenderTarget*& target;
      ~ResetActiveTarget() { target = nullptr; }
    } reset_active_target{active_target_};
    RenderResult result;
    const auto readback_start = CpuClock::now();
    if (HasAov(frame.cpu_readback_aovs, Aov::Color)) {
      result.color = ReadColor(frame.target.width, frame.target.height);
    }
    if (HasAov(frame.cpu_readback_aovs, Aov::Depth)) {
      result.depth = ReadDepth(frame.target.width, frame.target.height);
    }
    if (HasAov(frame.cpu_readback_aovs, Aov::PrimId)) {
      result.prim_id = ReadId(frame.target.width, frame.target.height,
                              Aov::PrimId, frame.target.prim_id_readback);
    }
    if (HasAov(frame.cpu_readback_aovs, Aov::InstanceId)) {
      result.instance_id = ReadId(frame.target.width, frame.target.height,
                                  Aov::InstanceId,
                                  frame.target.instance_id_readback);
    }
    const auto readback_ns = ElapsedNanoseconds(readback_start);
    result.rendered_aovs = frame.rendered_aovs;
    result.cpu_readback_aovs = frame.cpu_readback_aovs;
    result.scene_revision = frame.scene_revision;
    result.completion_value = completion;
    result.cpu_timings = frame.cpu_timings;
    result.cpu_timings.completion_wait_ns = wait_ns;
    result.cpu_timings.readback_ns = readback_ns;
    result.cpu_timings.gpu_execution_ns = ReadGpuExecutionNanoseconds(frame);
    result.cpu_timings.gaussian_raster_ns =
        ReadGaussianRasterNanoseconds(frame);
    result.cpu_timings.backend_total_ns += ElapsedNanoseconds(resolve_start);
    ++frame_counters_.wait_count;
    ++frame_counters_.resolve_count;
    result.counters = frame_counters_;
    result.material_diagnostics = frame.material_diagnostics;
    frame.outstanding = false;
    frame.counters = {};
    frame.material_diagnostics.clear();
    return result;
  }

  struct RetiredBuffer {
    Buffer buffer;
    std::uint64_t retire_value{};
  };

  struct TextureSlot {
    std::uint64_t revision{};
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
    bool pending_upload{};
    BindlessSlotHandle bindless_slot;
  };

  struct SamplerSlot {
    std::uint64_t revision{};
    VkSampler sampler{};
    BindlessSlotHandle bindless_slot;
  };

  struct RetiredTexture {
    TextureSlot texture;
    std::uint64_t retire_value{};
  };

  struct RetiredSampler {
    VkSampler sampler{};
    std::uint64_t retire_value{};
    BindlessSlotHandle bindless_slot;
  };

  // GPU residency for one mesh, keyed by the serialized RenderWorld handle
  // (slot index + generation) with per-sub-resource revisions. A revision
  // mismatch re-uploads only the stale sub-resource; a size-preserving edit
  // reuses the existing range in place.
  struct GeometrySlot {
    std::uint64_t vertex_revision{};
    std::uint64_t topology_revision{};
    BufferRange vertices;
    BufferRange indices;
    std::uint32_t index_count{};
  };

  struct GaussianAttributeSlot {
    std::uint64_t record_revision{};
    std::uint64_t positions_revision{};
    std::uint64_t covariance_revision{};
    std::uint64_t opacity_revision{};
    std::uint64_t radiance_revision{};
    std::uint64_t policy_revision{};
    std::uint64_t transform_revision{};
    std::uint64_t visibility_revision{};
    std::uint32_t generation{};
    std::uint32_t particle_count{};
    std::uint32_t coefficients_per_particle{};
    BufferRange positions;
    BufferRange covariances;
    BufferRange opacities;
    BufferRange radiance;
  };

  struct RetiredRange {
    DeviceArena* arena{};
    BufferRange range;
    std::uint64_t retire_value{};
  };

  struct PendingCopy {
    VkBuffer source{};
    VkDeviceSize source_offset{};
    VkBuffer destination{};
    VkDeviceSize destination_offset{};
    VkDeviceSize size{};
  };

  struct PendingImageCopy {
    VkBuffer source{};
    VkImage destination{};
    std::uint32_t width{};
    std::uint32_t height{};
  };

  struct GpuSceneBuffers {
    render::GpuScenePackingCapacities capacities;
    Buffer geometries;
    Buffer instances;
    Buffer materials;
    Buffer draws;
    std::uint64_t source_id{};
    std::uint64_t revision{};
    std::uint64_t pending_source_id{};
    std::uint64_t pending_revision{};
    std::shared_ptr<const std::vector<std::uint32_t>> draw_slot_indices;
    std::shared_ptr<const std::vector<std::uint32_t>>
        pending_draw_slot_indices;
    bool has_resident_update{};
    bool pending_update{};

    [[nodiscard]] bool enabled() const noexcept {
      return geometries.handle != VK_NULL_HANDLE;
    }
  };

  struct RenderTarget {
    std::uint32_t width{};
    std::uint32_t height{};
    VkImage color{};
    VkDeviceMemory color_memory{};
    VkImageView color_view{};
    VkImage depth{};
    VkDeviceMemory depth_memory{};
    VkImageView depth_view{};
    VkImage prim_id{};
    VkDeviceMemory prim_id_memory{};
    VkImageView prim_id_view{};
    VkImage instance_id{};
    VkDeviceMemory instance_id_memory{};
    VkImageView instance_id_view{};
    VkRenderPass render_pass{};
    VkFramebuffer framebuffer{};
    VkPipelineLayout pipeline_layout{};
    std::map<std::uint32_t, VkPipeline> pipelines;
    std::map<std::uint32_t, VkPipeline> gpu_driven_pipelines;
    std::map<std::string, VkPipelineLayout> generated_pipeline_layouts;
    std::map<std::pair<std::string, std::uint32_t>, VkPipeline>
        generated_pipelines;
    VkPipeline gaussian_pipeline{};
    VkPipeline gaussian_id_pipeline{};
    Buffer color_readback;
    Buffer depth_readback;
    Buffer prim_id_readback;
    Buffer instance_id_readback;
    ShaderPaths shaders;
    std::vector<Aov> cpu_readback_aovs;
  };

  struct GpuDrivenBatchResources {
    VkDescriptorSet descriptor_set{};
    VkDeviceSize candidate_offset{};
    VkDeviceSize command_offset{};
    VkDeviceSize counter_offset{};
    std::uint32_t candidate_count{};
    std::uint32_t vertex_block{kInvalidBlock};
    std::uint32_t index_block{kInvalidBlock};
    std::uint32_t pipeline_variant{};
    VkPipeline compute_pipeline{};
    // The device-local candidate list remains valid while this frame context
    // is idle. Reusing the context with an identical physical-slot sequence
    // therefore needs no staging reservation or transfer. Keep a separate
    // pending value so a failed queue submission cannot publish bytes that
    // never reached the device.
    std::vector<std::uint32_t> candidate_draw_slot_shadow;
    std::vector<std::uint32_t> pending_candidate_draw_slot_shadow;
    VkDeviceSize candidate_draw_slot_shadow_offset{};
    VkDeviceSize pending_candidate_draw_slot_shadow_offset{};
    bool candidate_upload_pending{};
  };

  struct GpuDrivenFrameResources {
    Buffer candidate_draw_slots;
    Buffer candidate_results;
    Buffer indirect_commands;
    Buffer dispatch_counters;
    Buffer counter_readback;
    VkDescriptorPool descriptor_pool{};
    std::vector<VkDescriptorSet> descriptor_sets;
    std::vector<GpuDrivenBatchResources> batches;
    VkDeviceSize candidate_capacity_bytes{};
    VkDeviceSize command_capacity_bytes{};
    VkDeviceSize counter_capacity_bytes{};
    std::uint32_t batch_capacity{};
    std::uint32_t candidate_count{};
    bool selected{};
  };

  struct FrameContext {
    VkCommandPool command_pool{};
    VkCommandBuffer command_buffer{};
    VkCommandPool transfer_command_pool{};
    VkCommandBuffer transfer_command_buffer{};
    VkFence fence{};
    VkQueryPool timestamp_pool{};
    VkSemaphore image_available{};
    std::uint32_t present_image_index{};
    bool present_pending{};
    std::uint64_t completion_value{};
    bool outstanding{};
    std::uint64_t exported_aov_mask{};
    RenderTarget target;
    std::uint64_t scene_revision{};
    std::vector<Aov> rendered_aovs;
    std::vector<Aov> cpu_readback_aovs;
    FrameCpuTimings cpu_timings;
    FrameCounters counters;
    VkDescriptorPool descriptor_pool{};
    std::uint32_t descriptor_capacity{};
    std::vector<VkDescriptorSet> material_descriptor_sets;
    VkDescriptorSet bindless_material_descriptor_set{};
    VkDeviceSize material_uniform_stride{};
    Buffer material_uniforms;
    std::vector<VkDeviceSize> generated_parameter_offsets;
    Buffer generated_parameter_uniforms;
    Buffer gaussian_instances;
    // Host-visible Gaussian buffers are frame-owned so they remain immutable
    // while a submission is in flight. Retain the packed contents for this
    // particular buffer and write only instance ranges whose derived values
    // changed when the frame context is reused.
    std::vector<GaussianGpuInstance> gaussian_instance_shadow;
    std::uint64_t gaussian_preparation_generation{};
    std::uint32_t gaussian_instance_count{};
    std::vector<MaterialDiagnostic> material_diagnostics;
    GpuDrivenFrameResources gpu_driven;
  };

  struct SwapchainState {
    VkSwapchainKHR handle{};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkColorSpaceKHR color_space{VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    VkPresentModeKHR present_mode{VK_PRESENT_MODE_FIFO_KHR};
    VkExtent2D extent{};
    std::uint32_t requested_width{};
    std::uint32_t requested_height{};
    std::vector<VkImage> images;
    std::vector<VkImageView> image_views;
    std::vector<VkFramebuffer> overlay_framebuffers;
    std::vector<VkSemaphore> render_finished;
    std::vector<bool> initialized;
    VkRenderPass overlay_render_pass{};
    bool dirty{};
  };

  enum class ResourceSyncMode {
    Full,
    ReplaceAll,
    Incremental,
    Unchanged,
  };

  [[nodiscard]] ResourceSyncMode SelectResourceSyncMode(
      const extraction::FrameSnapshot& snapshot) const noexcept {
    if (resource_residency_dirty_ ||
        snapshot.source_id != resident_snapshot_source_) {
      return ResourceSyncMode::ReplaceAll;
    }
    if (snapshot.source_id == 0) {
      return ResourceSyncMode::Full;
    }
    if (snapshot.revision == resident_snapshot_revision_) {
      return ResourceSyncMode::Unchanged;
    }
    if (snapshot.delta &&
        snapshot.delta->base_revision == resident_snapshot_revision_) {
      return ResourceSyncMode::Incremental;
    }
    return ResourceSyncMode::Full;
  }

  void CommitResourceSnapshot(
      const extraction::FrameSnapshot& snapshot) noexcept {
    resident_snapshot_source_ = snapshot.source_id;
    resident_snapshot_revision_ =
        snapshot.source_id == 0 ? 0 : snapshot.revision;
  }

  void AdoptBorrowedContext(const RendererOptions& options) {
    const auto& borrowed = *options.borrowed_context;
    // From this point onward every decoded native handle remains host-owned,
    // including exceptional construction paths.
    owns_vulkan_context_ = false;
    instance_ = DecodeHandle<VkInstance>(borrowed.instance);
    physical_device_ =
        DecodeHandle<VkPhysicalDevice>(borrowed.physical_device);
    device_ = DecodeHandle<VkDevice>(borrowed.device);
    queue_ = DecodeHandle<VkQueue>(borrowed.graphics_queue);
    queue_family_ = borrowed.graphics_queue_family;
    if (instance_ == VK_NULL_HANDLE ||
        physical_device_ == VK_NULL_HANDLE ||
        device_ == VK_NULL_HANDLE || queue_ == VK_NULL_HANDLE) {
      throw RendererError(RendererErrorCode::InvalidRequest,
                          "borrow Vulkan context",
                          "instance, physical device, device, and graphics "
                          "queue handles must all be non-null");
    }
    if (options.enable_validation &&
        (!borrowed.validation_enabled || !borrowed.debug_utils_enabled)) {
      throw RendererError(
          RendererErrorCode::Unsupported, "borrow Vulkan context",
          "renderer validation requires validation and VK_EXT_debug_utils on "
          "the borrowed instance");
    }

    std::uint32_t queue_count{};
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_count,
                                             nullptr);
    if (queue_family_ >= queue_count) {
      throw RendererError(RendererErrorCode::InvalidRequest,
                          "borrow Vulkan context",
                          "graphics queue family is out of range");
    }
    std::vector<VkQueueFamilyProperties> queues(queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_count,
                                             queues.data());
    const auto& queue_properties = queues[queue_family_];
    if ((queue_properties.queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0U ||
        borrowed.graphics_queue_index >= queue_properties.queueCount) {
      throw RendererError(RendererErrorCode::InvalidRequest,
                          "borrow Vulkan context",
                          "borrowed queue is not a valid graphics queue");
    }
    VkQueue declared_queue{};
    vkGetDeviceQueue(device_, queue_family_, borrowed.graphics_queue_index,
                     &declared_queue);
    if (declared_queue != queue_) {
      throw RendererError(RendererErrorCode::InvalidRequest,
                          "borrow Vulkan context",
                          "graphics queue does not match its declared family "
                          "and index");
    }

    std::uint32_t loader_api_version = VK_API_VERSION_1_0;
    const auto enumerate_instance_version =
        reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
    if (enumerate_instance_version != nullptr) {
      Check(enumerate_instance_version(&loader_api_version),
            "query Vulkan loader version");
    }
    if (loader_api_version < kMinimumBorrowedVulkanApiVersion) {
      throw RendererError(RendererErrorCode::Unsupported,
                          "borrow Vulkan context",
                          "Vulkan 1.3 loader is required");
    }

    VkPhysicalDeviceDescriptorIndexingProperties descriptor_properties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
    VkPhysicalDeviceDriverProperties driver_properties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    driver_properties.pNext = &descriptor_properties;
    VkPhysicalDeviceProperties2 properties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &driver_properties;
    vkGetPhysicalDeviceProperties2(physical_device_, &properties);
    if (properties.properties.apiVersion <
        kMinimumBorrowedVulkanApiVersion) {
      throw RendererError(RendererErrorCode::Unsupported,
                          "borrow Vulkan context",
                          "borrowed device does not provide Vulkan 1.3");
    }

    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceFeatures2 features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &timeline;
    vkGetPhysicalDeviceFeatures2(physical_device_, &features);
    VkPhysicalDeviceVulkan11Features vulkan11{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features vulkan12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 versioned_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    versioned_features.pNext = &vulkan11;
    vulkan11.pNext = &vulkan12;
    vkGetPhysicalDeviceFeatures2(physical_device_, &versioned_features);
    if (borrowed.timeline_semaphore_enabled &&
        timeline.timelineSemaphore != VK_TRUE) {
      throw RendererError(
          RendererErrorCode::Unsupported, "borrow Vulkan context",
          "application declared timeline semaphores on a device that does "
          "not support them");
    }
    if (borrowed.draw_indirect_first_instance_enabled &&
        features.features.drawIndirectFirstInstance != VK_TRUE) {
      throw RendererError(
          RendererErrorCode::Unsupported, "borrow Vulkan context",
          "application declared drawIndirectFirstInstance on a device that "
          "does not support it");
    }
    if (borrowed.draw_indirect_count_enabled &&
        vulkan12.drawIndirectCount != VK_TRUE) {
      throw RendererError(
          RendererErrorCode::Unsupported, "borrow Vulkan context",
          "application declared drawIndirectCount on a device that does not support it");
    }
    if (borrowed.shader_draw_parameters_enabled &&
        vulkan11.shaderDrawParameters != VK_TRUE) {
      throw RendererError(
          RendererErrorCode::Unsupported, "borrow Vulkan context",
          "application declared shaderDrawParameters on a device that does not support it");
    }

    VkFormatProperties depth_properties{};
    vkGetPhysicalDeviceFormatProperties(physical_device_, kDepthFormat,
                                        &depth_properties);
    const auto required_depth =
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if ((depth_properties.optimalTilingFeatures & required_depth) !=
        required_depth) {
      throw RendererError(RendererErrorCode::Unsupported,
                          "borrow Vulkan context",
                          "D32 depth attachment readback is unsupported");
    }

    capabilities_.loader_api_version = loader_api_version;
    capabilities_.header_version = VK_HEADER_VERSION_COMPLETE;
    capabilities_.sdk_version = MERLIN_VULKAN_SDK_VERSION;
    capabilities_.device_name = properties.properties.deviceName;
    capabilities_.driver_name = driver_properties.driverName;
    capabilities_.driver_info = driver_properties.driverInfo;
    capabilities_.api_version = properties.properties.apiVersion;
    capabilities_.driver_version = properties.properties.driverVersion;
    capabilities_.vendor_id = properties.properties.vendorID;
    capabilities_.device_id = properties.properties.deviceID;
    capabilities_.max_image_dimension_2d =
        properties.properties.limits.maxImageDimension2D;
    max_draw_indirect_count_ =
        properties.properties.limits.maxDrawIndirectCount;
    max_storage_buffer_range_ =
        properties.properties.limits.maxStorageBufferRange;
    max_compute_work_group_count_x_ =
        properties.properties.limits.maxComputeWorkGroupCount[0];
    storage_buffer_alignment_ = std::max<VkDeviceSize>(
        4U, properties.properties.limits.minStorageBufferOffsetAlignment);
    capabilities_.timeline_semaphore =
        borrowed.timeline_semaphore_enabled;
    capabilities_.draw_indirect_first_instance =
        borrowed.draw_indirect_first_instance_enabled;
    capabilities_.draw_indirect_count = borrowed.draw_indirect_count_enabled;
    capabilities_.shader_draw_parameters =
        borrowed.shader_draw_parameters_enabled;
    capabilities_.validation_enabled = options.enable_validation;
    capabilities_.graphics_queue = true;
    capabilities_.compute_queue =
        (queue_properties.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0U;
    capabilities_.transfer_queue =
        (queue_properties.queueFlags & VK_QUEUE_TRANSFER_BIT) != 0U;
    capabilities_.async_transfer_queue = false;
    capabilities_.queue_ownership_transfers = false;
    capabilities_.borrowed_vulkan_context = true;
    capabilities_.graphics_queue_family = queue_family_;
    capabilities_.transfer_queue_family = queue_family_;
    capabilities_.timestamp_queries =
        properties.properties.limits.timestampPeriod > 0.0F &&
        queue_properties.timestampValidBits != 0U;
    capabilities_.external_presentation = false;
    capabilities_.memory_budget_extension = false;
    selected_timestamp_valid_bits_ = queue_properties.timestampValidBits;
    timestamp_period_ns_ = properties.properties.limits.timestampPeriod;
    uniform_buffer_alignment_ = std::max<VkDeviceSize>(
        16U, properties.properties.limits.minUniformBufferOffsetAlignment);

    DescriptorIndexingConfiguration descriptor_configuration;
    descriptor_configuration.request =
        DescriptorBackendRequest::Conventional;
    descriptor_configuration.texture_capacity =
        options.bindless_texture_capacity;
    descriptor_configuration.sampler_capacity =
        options.bindless_sampler_capacity;
    descriptor_configuration.additional_sampler_allocation_count =
        static_cast<std::uint64_t>(
            descriptor_configuration.sampler_capacity) *
        options.frames_in_flight;
    descriptor_configuration.additional_per_stage_resource_count = 4;
    capabilities_.descriptor_indexing_selection = SelectDescriptorBackend(
        descriptor_configuration, {}, {});

    transfer_queue_family_ = queue_family_;
    transfer_queue_ = queue_;

    if (capabilities_.validation_enabled) {
      const auto create =
          reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
              vkGetInstanceProcAddr(instance_,
                                    "vkCreateDebugUtilsMessengerEXT"));
      if (create == nullptr) {
        throw RendererError(
            RendererErrorCode::Unsupported, "borrow Vulkan context",
            "VK_EXT_debug_utils entry point is unavailable");
      }
      VkDebugUtilsMessengerCreateInfoEXT debug_info{
          VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
      debug_info.messageSeverity =
          VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
      debug_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
      debug_info.pfnUserCallback = ValidationCallback;
      debug_info.pUserData = this;
      Check(create(instance_, &debug_info, nullptr, &debug_messenger_),
            "create borrowed validation debug messenger");
    }

    if (capabilities_.timeline_semaphore) {
      VkSemaphoreTypeCreateInfo type_info{
          VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
      type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
      type_info.initialValue = 0;
      VkSemaphoreCreateInfo semaphore_info{
          VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      semaphore_info.pNext = &type_info;
      Check(vkCreateSemaphore(device_, &semaphore_info, nullptr,
                              &timeline_semaphore_),
            "create frame timeline semaphore");
    }
  }

  void CreateDevice(const RendererOptions& options) {
    if (options.borrowed_context) {
      AdoptBorrowedContext(options);
      return;
    }

    constexpr const char* validation_layer = "VK_LAYER_KHRONOS_validation";
    const bool use_validation =
        options.enable_validation && HasLayer(validation_layer);
    const std::vector<const char*> layers = use_validation
        ? std::vector<const char*>{validation_layer}
        : std::vector<const char*>{};
    const bool use_debug_utils =
        use_validation && HasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    std::vector<const char*> extensions;
    if (use_debug_utils) {
      extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    if (options.presentation) {
      if (options.presentation->required_instance_extensions.empty()) {
        throw RendererError(RendererErrorCode::InvalidRequest,
                            "create presentation target",
                            "presentation adapter supplied no instance extensions");
      }
      for (const auto& extension :
           options.presentation->required_instance_extensions) {
        if (extension.empty() || !HasInstanceExtension(extension.c_str())) {
          throw RendererError(RendererErrorCode::Unsupported,
                              "create presentation target",
                              "required Vulkan instance extension is unavailable: " +
                                  extension);
        }
        if (std::find_if(extensions.begin(), extensions.end(),
                         [&](const char* existing) {
                           return extension == existing;
                         }) == extensions.end()) {
          extensions.push_back(extension.c_str());
        }
      }
    }

    VkDebugUtilsMessengerCreateInfoEXT debug_info{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug_info.pfnUserCallback = ValidationCallback;
    debug_info.pUserData = this;

    VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app_info.pApplicationName = "hdMerlin";
    app_info.applicationVersion = VK_MAKE_API_VERSION(0, 0, 2, 0);
    app_info.pEngineName = "Merlin";
    app_info.engineVersion = VK_MAKE_API_VERSION(0, 0, 2, 0);
    app_info.apiVersion = kMinimumVulkanApiVersion;

    std::uint32_t loader_api_version = VK_API_VERSION_1_0;
    const auto enumerate_instance_version =
        reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
    if (enumerate_instance_version != nullptr) {
      Check(enumerate_instance_version(&loader_api_version),
            "query Vulkan loader version");
    }
    if (loader_api_version < kMinimumVulkanApiVersion) {
      throw RendererError(RendererErrorCode::Unsupported, "create renderer",
                          std::string("Vulkan ") +
                              MERLIN_VULKAN_MIN_VERSION_STRING +
                              " loader is required");
    }
    capabilities_.loader_api_version = loader_api_version;
    capabilities_.header_version = VK_HEADER_VERSION_COMPLETE;
    capabilities_.sdk_version = MERLIN_VULKAN_SDK_VERSION;

    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pNext = use_debug_utils ? &debug_info : nullptr;
    instance_info.pApplicationInfo = &app_info;
    instance_info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    instance_info.ppEnabledLayerNames = layers.data();
    instance_info.enabledExtensionCount =
        static_cast<std::uint32_t>(extensions.size());
    instance_info.ppEnabledExtensionNames = extensions.data();
    Check(vkCreateInstance(&instance_info, nullptr, &instance_),
          "create Vulkan instance");
    if (options.presentation) {
      std::uintptr_t encoded_surface{};
      const auto result = static_cast<VkResult>(
          options.presentation->create_surface(
              options.presentation->user_data, EncodeHandle(instance_),
              &encoded_surface));
      Check(result, "create Vulkan presentation surface");
      surface_ = DecodeHandle<VkSurfaceKHR>(encoded_surface);
      if (surface_ == VK_NULL_HANDLE) {
        throw RendererError(RendererErrorCode::BackendFailure,
                            "create presentation target",
                            "presentation adapter returned a null surface");
      }
    }
    if (use_debug_utils) {
      const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
          vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
      if (create == nullptr) {
        throw std::runtime_error("VK_EXT_debug_utils entry point is unavailable");
      }
      Check(create(instance_, &debug_info, nullptr, &debug_messenger_),
            "create validation debug messenger");
    }

    std::uint32_t device_count{};
    Check(vkEnumeratePhysicalDevices(instance_, &device_count, nullptr),
          "enumerate physical devices");
    if (device_count == 0) {
      throw RendererError(RendererErrorCode::Unsupported, "create renderer",
                          "no Vulkan physical device is available");
    }
    std::vector<VkPhysicalDevice> devices(device_count);
    Check(vkEnumeratePhysicalDevices(instance_, &device_count, devices.data()),
          "enumerate physical devices");

    VkQueueFlags selected_queue_flags{};
    for (auto candidate : devices) {
      VkPhysicalDeviceProperties candidate_properties{};
      vkGetPhysicalDeviceProperties(candidate, &candidate_properties);
      if (candidate_properties.apiVersion < kMinimumVulkanApiVersion) {
        continue;
      }
      std::uint32_t queue_count{};
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count, nullptr);
      std::vector<VkQueueFamilyProperties> queues(queue_count);
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count, queues.data());
      std::uint32_t graphics_family = VK_QUEUE_FAMILY_IGNORED;
      std::uint32_t transfer_family = VK_QUEUE_FAMILY_IGNORED;
      for (std::uint32_t index = 0; index < queue_count; ++index) {
        const auto flags = queues[index].queueFlags;
        bool presentation_supported = true;
        if (surface_ != VK_NULL_HANDLE) {
          VkBool32 supported{};
          Check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, index,
                                                     surface_, &supported),
                "query Vulkan presentation support");
          presentation_supported = supported == VK_TRUE;
        }
        if (graphics_family == VK_QUEUE_FAMILY_IGNORED &&
            (flags & VK_QUEUE_GRAPHICS_BIT) != 0U &&
            presentation_supported) {
          graphics_family = index;
        }
        if (transfer_family == VK_QUEUE_FAMILY_IGNORED &&
            (flags & VK_QUEUE_TRANSFER_BIT) != 0U &&
            (flags & VK_QUEUE_GRAPHICS_BIT) == 0U) {
          transfer_family = index;
        }
      }
      if (graphics_family != VK_QUEUE_FAMILY_IGNORED &&
          (surface_ == VK_NULL_HANDLE ||
           HasDeviceExtension(candidate, VK_KHR_SWAPCHAIN_EXTENSION_NAME))) {
        physical_device_ = candidate;
        queue_family_ = graphics_family;
        transfer_queue_family_ =
            transfer_family == VK_QUEUE_FAMILY_IGNORED ? graphics_family
                                                        : transfer_family;
        selected_queue_flags = queues[graphics_family].queueFlags;
        selected_timestamp_valid_bits_ =
            queues[graphics_family].timestampValidBits;
      }
      if (physical_device_ != VK_NULL_HANDLE) {
        break;
      }
    }
    if (physical_device_ == VK_NULL_HANDLE) {
      throw RendererError(
          RendererErrorCode::Unsupported, "create renderer",
          std::string("no Vulkan ") + MERLIN_VULKAN_MIN_VERSION_STRING +
              " physical device with a graphics queue is available");
    }

    VkPhysicalDeviceDescriptorIndexingProperties descriptor_properties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
    VkPhysicalDeviceDriverProperties driver_properties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    driver_properties.pNext = &descriptor_properties;
    VkPhysicalDeviceProperties2 properties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &driver_properties;
    vkGetPhysicalDeviceProperties2(physical_device_, &properties);
    capabilities_.device_name = properties.properties.deviceName;
    capabilities_.driver_name = driver_properties.driverName;
    capabilities_.driver_info = driver_properties.driverInfo;
    capabilities_.api_version = properties.properties.apiVersion;
    capabilities_.driver_version = properties.properties.driverVersion;
    capabilities_.vendor_id = properties.properties.vendorID;
    capabilities_.device_id = properties.properties.deviceID;
    capabilities_.max_image_dimension_2d =
        properties.properties.limits.maxImageDimension2D;
    max_draw_indirect_count_ =
        properties.properties.limits.maxDrawIndirectCount;
    max_storage_buffer_range_ =
        properties.properties.limits.maxStorageBufferRange;
    max_compute_work_group_count_x_ =
        properties.properties.limits.maxComputeWorkGroupCount[0];
    storage_buffer_alignment_ = std::max<VkDeviceSize>(
        4U, properties.properties.limits.minStorageBufferOffsetAlignment);
    uniform_buffer_alignment_ = std::max<VkDeviceSize>(
        16U, properties.properties.limits.minUniformBufferOffsetAlignment);
    capabilities_.validation_enabled = use_validation;
    capabilities_.graphics_queue = true;
    capabilities_.compute_queue =
        (selected_queue_flags & VK_QUEUE_COMPUTE_BIT) != 0U;
    capabilities_.transfer_queue = transfer_queue_family_ != VK_QUEUE_FAMILY_IGNORED;
    capabilities_.graphics_queue_family = queue_family_;
    capabilities_.timestamp_queries =
        properties.properties.limits.timestampPeriod > 0.0F &&
        selected_timestamp_valid_bits_ != 0U;
    capabilities_.external_presentation = surface_ != VK_NULL_HANDLE;
    timestamp_period_ns_ = properties.properties.limits.timestampPeriod;

    VkFormatProperties depth_properties{};
    vkGetPhysicalDeviceFormatProperties(physical_device_, kDepthFormat,
                                        &depth_properties);
    const auto required_depth = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if ((depth_properties.optimalTilingFeatures & required_depth) != required_depth) {
      throw RendererError(RendererErrorCode::Unsupported, "create renderer",
                          "D32 depth attachment readback is unsupported");
    }

    VkPhysicalDeviceDescriptorIndexingFeatures descriptor_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    timeline.pNext = &descriptor_features;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &timeline;
    vkGetPhysicalDeviceFeatures2(physical_device_, &features);
    VkPhysicalDeviceVulkan11Features vulkan11{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features vulkan12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 versioned_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    versioned_features.pNext = &vulkan11;
    vulkan11.pNext = &vulkan12;
    vkGetPhysicalDeviceFeatures2(physical_device_, &versioned_features);
    capabilities_.timeline_semaphore = timeline.timelineSemaphore == VK_TRUE;
    capabilities_.draw_indirect_first_instance =
        features.features.drawIndirectFirstInstance == VK_TRUE;
    capabilities_.draw_indirect_count =
        vulkan12.drawIndirectCount == VK_TRUE;
    capabilities_.shader_draw_parameters =
        vulkan11.shaderDrawParameters == VK_TRUE;
    capabilities_.async_transfer_queue =
        options.enable_async_transfer && capabilities_.timeline_semaphore &&
        transfer_queue_family_ != queue_family_;
    if (!capabilities_.async_transfer_queue) {
      transfer_queue_family_ = queue_family_;
    }
    capabilities_.transfer_queue_family = transfer_queue_family_;
    capabilities_.queue_ownership_transfers =
        capabilities_.async_transfer_queue;
    capabilities_.memory_budget_extension = HasDeviceExtension(
        physical_device_, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    capabilities_.descriptor_indexing_features = {
        descriptor_features.shaderSampledImageArrayNonUniformIndexing ==
            VK_TRUE,
        descriptor_features.descriptorBindingSampledImageUpdateAfterBind ==
            VK_TRUE,
        descriptor_features.descriptorBindingPartiallyBound == VK_TRUE,
        descriptor_features.descriptorBindingVariableDescriptorCount ==
            VK_TRUE,
        descriptor_features.runtimeDescriptorArray == VK_TRUE,
    };
    capabilities_.descriptor_indexing_limits = {
        descriptor_properties.maxUpdateAfterBindDescriptorsInAllPools,
        descriptor_properties.maxPerStageDescriptorUpdateAfterBindSamplers,
        descriptor_properties.maxPerStageDescriptorUpdateAfterBindSampledImages,
        descriptor_properties.maxPerStageUpdateAfterBindResources,
        descriptor_properties.maxDescriptorSetUpdateAfterBindSamplers,
        descriptor_properties.maxDescriptorSetUpdateAfterBindSampledImages,
        properties.properties.limits.maxSamplerAllocationCount,
    };

    DescriptorIndexingConfiguration descriptor_configuration;
    descriptor_configuration.request = options.descriptor_backend;
    descriptor_configuration.texture_capacity =
        options.bindless_texture_capacity;
    descriptor_configuration.sampler_capacity =
        options.bindless_sampler_capacity;
    descriptor_configuration.additional_sampler_allocation_count =
        static_cast<std::uint64_t>(descriptor_configuration.sampler_capacity) *
        options.frames_in_flight;
    // The current Forward fragment stage exposes three color attachments and
    // one non-table uniform-buffer descriptor. Standalone sampler descriptors
    // are excluded from maxPerStageResources by Vulkan.
    descriptor_configuration.additional_per_stage_resource_count = 4;
    capabilities_.descriptor_indexing_selection = SelectDescriptorBackend(
        descriptor_configuration, capabilities_.descriptor_indexing_features,
        capabilities_.descriptor_indexing_limits);

    const float priority = 1.0F;
    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    VkDeviceQueueCreateInfo graphics_queue_info{
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    graphics_queue_info.queueFamilyIndex = queue_family_;
    graphics_queue_info.queueCount = 1;
    graphics_queue_info.pQueuePriorities = &priority;
    queue_infos.push_back(graphics_queue_info);
    if (capabilities_.async_transfer_queue) {
      auto transfer_queue_info = graphics_queue_info;
      transfer_queue_info.queueFamilyIndex = transfer_queue_family_;
      queue_infos.push_back(transfer_queue_info);
    }
    std::vector<const char*> device_extensions;
    if (capabilities_.memory_budget_extension) {
      device_extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    }
    if (surface_ != VK_NULL_HANDLE) {
      device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkPhysicalDeviceFeatures enabled_core_features{};
    enabled_core_features.drawIndirectFirstInstance =
        capabilities_.draw_indirect_first_instance ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceVulkan11Features enabled_vulkan11{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    enabled_vulkan11.shaderDrawParameters =
        capabilities_.shader_draw_parameters ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceVulkan12Features enabled_vulkan12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    enabled_vulkan12.timelineSemaphore =
        capabilities_.timeline_semaphore ? VK_TRUE : VK_FALSE;
    enabled_vulkan12.drawIndirectCount =
        capabilities_.draw_indirect_count ? VK_TRUE : VK_FALSE;
    if (capabilities_.descriptor_indexing_selection.selected_backend ==
        DescriptorBackend::Bindless) {
      enabled_vulkan12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
      enabled_vulkan12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
      enabled_vulkan12.descriptorBindingPartiallyBound = VK_TRUE;
      enabled_vulkan12.descriptorBindingVariableDescriptorCount = VK_TRUE;
      enabled_vulkan12.runtimeDescriptorArray = VK_TRUE;
    }
    enabled_vulkan11.pNext = &enabled_vulkan12;
    device_info.pNext = &enabled_vulkan11;
    device_info.pEnabledFeatures = &enabled_core_features;
    device_info.queueCreateInfoCount =
        static_cast<std::uint32_t>(queue_infos.size());
    device_info.pQueueCreateInfos = queue_infos.data();
    device_info.enabledExtensionCount =
        static_cast<std::uint32_t>(device_extensions.size());
    device_info.ppEnabledExtensionNames = device_extensions.data();
    Check(vkCreateDevice(physical_device_, &device_info, nullptr, &device_),
          "create Vulkan device");
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    vkGetDeviceQueue(device_, transfer_queue_family_, 0, &transfer_queue_);

    if (capabilities_.timeline_semaphore) {
      VkSemaphoreTypeCreateInfo type_info{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
      type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
      type_info.initialValue = 0;
      VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      semaphore_info.pNext = &type_info;
      Check(vkCreateSemaphore(device_, &semaphore_info, nullptr,
                              &timeline_semaphore_),
            "create frame timeline semaphore");
      if (capabilities_.async_transfer_queue) {
        Check(vkCreateSemaphore(device_, &semaphore_info, nullptr,
                                &transfer_timeline_semaphore_),
              "create transfer timeline semaphore");
      }
    }
  }

  void CreateFrameContexts(std::uint32_t count) {
    frames_.resize(count);
    for (auto& frame : frames_) {
      VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      pool_info.queueFamilyIndex = queue_family_;
      Check(vkCreateCommandPool(device_, &pool_info, nullptr,
                                &frame.command_pool),
            "create frame command pool");
      VkCommandBufferAllocateInfo command_info{
          VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      command_info.commandPool = frame.command_pool;
      command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      command_info.commandBufferCount = 1;
      Check(vkAllocateCommandBuffers(device_, &command_info,
                                     &frame.command_buffer),
            "allocate frame command buffer");
      if (surface_ != VK_NULL_HANDLE) {
        VkSemaphoreCreateInfo semaphore_info{
            VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        Check(vkCreateSemaphore(device_, &semaphore_info, nullptr,
                                &frame.image_available),
              "create presentation acquire semaphore");
      }
      if (capabilities_.async_transfer_queue) {
        pool_info.queueFamilyIndex = transfer_queue_family_;
        Check(vkCreateCommandPool(device_, &pool_info, nullptr,
                                  &frame.transfer_command_pool),
              "create transfer command pool");
        command_info.commandPool = frame.transfer_command_pool;
        Check(vkAllocateCommandBuffers(device_, &command_info,
                                       &frame.transfer_command_buffer),
              "allocate transfer command buffer");
      }
      if (timeline_semaphore_ == VK_NULL_HANDLE) {
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        Check(vkCreateFence(device_, &fence_info, nullptr, &frame.fence),
              "create frame fence");
      }
      if (capabilities_.timestamp_queries) {
        VkQueryPoolCreateInfo query_info{
            VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query_info.queryCount = 4;
        Check(vkCreateQueryPool(device_, &query_info, nullptr,
                                &frame.timestamp_pool),
              "create frame timestamp query pool");
      }
    }
  }

  Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags properties,
                      std::uint32_t first_queue_family =
                          VK_QUEUE_FAMILY_IGNORED,
                      std::uint32_t second_queue_family =
                          VK_QUEUE_FAMILY_IGNORED) {
    ++frame_counters_.allocation_count;
    ++frame_counters_.buffer_allocation_count;
    frame_counters_.buffer_allocation_bytes += size;
    return CreateBufferRaw(device_, physical_device_, size, usage, properties,
                           &memory_budget_, first_queue_family,
                           second_queue_family);
  }

  void DestroyBuffer(Buffer& buffer) noexcept {
    DestroyBufferRaw(device_, buffer, &memory_budget_);
  }

  template <typename Record>
  Buffer CreateGpuSceneTable(std::uint32_t capacity) {
    if (capacity == 0) {
      throw RendererError(RendererErrorCode::InvalidRequest,
                          "create GPU Scene buffers",
                          "every GPU Scene table capacity must be non-zero");
    }
    const auto bytes = static_cast<VkDeviceSize>(capacity) * sizeof(Record);
    if (bytes > max_storage_buffer_range_) {
      throw RendererError(
          RendererErrorCode::Unsupported, "create GPU Scene buffers",
          "GPU Scene table exceeds the device maxStorageBufferRange limit");
    }
    ++frame_counters_.allocation_count;
    ++frame_counters_.buffer_allocation_count;
    frame_counters_.buffer_allocation_bytes += bytes;
    return CreateBufferRaw(
        device_, physical_device_, bytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory_budget_, queue_family_,
        transfer_queue_family_);
  }

  void CreateGpuSceneBuffers(
      render::GpuScenePackingCapacities capacities) {
    gpu_scene_buffers_.capacities = capacities;
    gpu_scene_buffers_.geometries =
        CreateGpuSceneTable<render::GpuGeometry>(capacities.geometries);
    gpu_scene_buffers_.instances =
        CreateGpuSceneTable<render::GpuInstance>(capacities.instances);
    gpu_scene_buffers_.materials =
        CreateGpuSceneTable<render::GpuMaterial>(capacities.materials);
    gpu_scene_buffers_.draws =
        CreateGpuSceneTable<render::GpuDraw>(capacities.draws);
  }

  void DestroyGpuSceneBuffers() noexcept {
    DestroyBuffer(gpu_scene_buffers_.geometries);
    DestroyBuffer(gpu_scene_buffers_.instances);
    DestroyBuffer(gpu_scene_buffers_.materials);
    DestroyBuffer(gpu_scene_buffers_.draws);
    gpu_scene_buffers_.capacities = {};
    gpu_scene_buffers_.source_id = 0;
    gpu_scene_buffers_.revision = 0;
    gpu_scene_buffers_.draw_slot_indices.reset();
    gpu_scene_buffers_.pending_draw_slot_indices.reset();
    gpu_scene_buffers_.has_resident_update = false;
    gpu_scene_buffers_.pending_update = false;
  }

  void DestroyGpuDrivenFrameResources(
      GpuDrivenFrameResources& resources) noexcept {
    DestroyBuffer(resources.candidate_draw_slots);
    DestroyBuffer(resources.candidate_results);
    DestroyBuffer(resources.indirect_commands);
    DestroyBuffer(resources.dispatch_counters);
    DestroyBuffer(resources.counter_readback);
    if (resources.descriptor_pool != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(device_, resources.descriptor_pool, nullptr);
    }
    resources = {};
  }

  template <typename Record>
  void ValidateGpuSceneTableUpdate(
      const render::GpuScenePackedUpdate<Record>& packed,
      const std::vector<render::GpuSceneDirtyRange>& dirty_ranges,
      std::uint32_t capacity, std::string_view table_name) const {
    if (packed.ranges.size() != dirty_ranges.size()) {
      throw RendererError(RendererErrorCode::InvalidRequest,
                          "upload GPU Scene",
                          std::string(table_name) +
                              " packed ranges do not match the update plan");
    }
    std::uint64_t record_count{};
    std::uint64_t copy_bytes{};
    std::uint64_t previous_end{};
    for (std::size_t i = 0; i < packed.ranges.size(); ++i) {
      const auto& range = packed.ranges[i];
      const auto& dirty = dirty_ranges[i];
      if (range.records.empty() || range.first_slot != dirty.first_slot ||
          range.records.size() != dirty.slot_count) {
        throw RendererError(RendererErrorCode::InvalidRequest,
                            "upload GPU Scene",
                            std::string(table_name) +
                                " packed range does not match its dirty range");
      }
      const auto end = static_cast<std::uint64_t>(range.first_slot) +
                       range.records.size();
      if (range.first_slot < previous_end || end > capacity) {
        throw RendererError(RendererErrorCode::InvalidRequest,
                            "upload GPU Scene",
                            std::string(table_name) +
                                " packed range is unordered or exceeds capacity");
      }
      previous_end = end;
      record_count += range.records.size();
      copy_bytes += range.records.size() * sizeof(Record);
    }
    if (packed.record_count != record_count ||
        packed.copy_bytes != copy_bytes) {
      throw RendererError(RendererErrorCode::InvalidRequest,
                          "upload GPU Scene",
                          std::string(table_name) +
                              " packed telemetry does not match its payload");
    }
  }

  void StageGpuSceneUpdate(
      const extraction::FrameSnapshot& snapshot,
      const render::GpuScenePackedFrameUpdate* update) {
    if (update == nullptr) {
      return;
    }
    if (!gpu_scene_buffers_.enabled()) {
      throw RendererError(RendererErrorCode::InvalidRequest,
                          "upload GPU Scene",
                          "renderer has no GPU Scene table capacities");
    }
    const auto validate_plan = [&](const auto& plan,
                                   std::string_view table_name) {
      if (plan.source_id != snapshot.source_id ||
          plan.revision != snapshot.revision) {
        throw RendererError(RendererErrorCode::InvalidRequest,
                            "upload GPU Scene",
                            std::string(table_name) +
                                " update was not packed from the request snapshot");
      }
    };
    validate_plan(update->geometry_plan, "geometry");
    validate_plan(update->instance_plan, "instance");
    validate_plan(update->material_plan, "material");
    validate_plan(update->draw_plan, "draw");
    const auto& reference_plan = update->geometry_plan;
    const auto same_plan_boundary = [&](const auto& plan) {
      return plan.source_id == reference_plan.source_id &&
             plan.base_revision == reference_plan.base_revision &&
             plan.revision == reference_plan.revision;
    };
    if (!same_plan_boundary(update->instance_plan) ||
        !same_plan_boundary(update->material_plan) ||
        !same_plan_boundary(update->draw_plan)) {
      throw RendererError(RendererErrorCode::InvalidRequest,
                          "upload GPU Scene",
                          "table plans do not share one revision boundary");
    }
    if (update->geometry_plan.table !=
            render::GpuSceneResourceTable::Geometry ||
        update->instance_plan.table !=
            render::GpuSceneResourceTable::Instance ||
        update->material_plan.table !=
            render::GpuSceneResourceTable::Material) {
      throw RendererError(RendererErrorCode::InvalidRequest,
                          "upload GPU Scene",
                          "resource update plans name the wrong tables");
    }

    const auto capacities = gpu_scene_buffers_.capacities;
    ValidateGpuSceneTableUpdate(update->geometries,
                                update->geometry_plan.dirty_ranges,
                                capacities.geometries, "geometry");
    ValidateGpuSceneTableUpdate(update->instances,
                                update->instance_plan.dirty_ranges,
                                capacities.instances, "instance");
    ValidateGpuSceneTableUpdate(update->materials,
                                update->material_plan.dirty_ranges,
                                capacities.materials, "material");
    ValidateGpuSceneTableUpdate(update->draws,
                                update->draw_plan.dirty_ranges,
                                capacities.draws, "draw");
    for (const auto& range : update->materials.ranges) {
      for (const auto& material : range.records) {
        const bool has_texture = material.base_color_texture_index !=
                                 render::kInvalidGpuSceneTableIndex;
        const bool has_sampler = material.base_color_sampler_index !=
                                 render::kInvalidGpuSceneTableIndex;
        if (has_texture != has_sampler) {
          throw RendererError(
              RendererErrorCode::InvalidRequest, "upload GPU Scene",
              "material texture and sampler table references are incomplete");
        }
        if (has_texture && bindless_texture_table_ &&
            (material.base_color_texture_index >=
                 bindless_texture_views_.size() ||
             material.base_color_sampler_index >= bindless_samplers_.size())) {
          throw RendererError(
              RendererErrorCode::InvalidRequest, "upload GPU Scene",
              "material texture or sampler reference exceeds bindless capacity");
        }
      }
    }
    for (const auto& range : update->draws.ranges) {
      for (const auto& draw : range.records) {
        if (draw.geometry_index >= capacities.geometries ||
            draw.instance_index >= capacities.instances ||
            draw.material_index >= capacities.materials) {
          throw RendererError(
              RendererErrorCode::InvalidRequest, "upload GPU Scene",
              "draw record contains an out-of-capacity table reference");
        }
      }
    }
    if (!update->draw_slot_indices ||
        update->draw_slot_indices->size() != snapshot.draws.size()) {
      throw RendererError(RendererErrorCode::InvalidRequest,
                          "upload GPU Scene",
                          "draw slot map does not match the request snapshot");
    }
    if (gpu_scene_buffers_.draw_slot_indices != update->draw_slot_indices) {
      std::vector<bool> mapped_draw_slots(capacities.draws);
      for (const auto slot : *update->draw_slot_indices) {
        if (slot >= capacities.draws || mapped_draw_slots[slot]) {
          throw RendererError(
              RendererErrorCode::InvalidRequest, "upload GPU Scene",
              "draw slot map contains an invalid or duplicate slot");
        }
        mapped_draw_slots[slot] = true;
      }
    }
    const auto copy_bytes = update->geometries.copy_bytes +
                            update->instances.copy_bytes +
                            update->materials.copy_bytes +
                            update->draws.copy_bytes;
    if (update->copy_bytes != copy_bytes) {
      throw RendererError(RendererErrorCode::InvalidRequest,
                          "upload GPU Scene",
                          "frame copy bytes do not match the table payloads");
    }
    const bool complete_reconciliation =
        update->geometry_plan.full_reconciliation &&
        update->instance_plan.full_reconciliation &&
        update->material_plan.full_reconciliation &&
        update->draw_plan.full_reconciliation &&
        update->geometries.record_count == snapshot.geometries.size() &&
        update->instances.record_count == snapshot.instances.size() &&
        update->materials.record_count == snapshot.materials.size() &&
        update->draws.record_count == snapshot.draws.size();
    const bool same_source =
        gpu_scene_buffers_.has_resident_update &&
        gpu_scene_buffers_.source_id == reference_plan.source_id;
    const bool unchanged =
        same_source && gpu_scene_buffers_.revision == reference_plan.revision;
    const bool continuous = same_source &&
                            gpu_scene_buffers_.revision ==
                                reference_plan.base_revision;
    if (!unchanged && !continuous && !complete_reconciliation) {
      throw RendererError(
          RendererErrorCode::InvalidRequest, "upload GPU Scene",
          "update neither continues nor completely rebuilds the resident "
          "table revision");
    }
    if (unchanged && copy_bytes != 0 && !complete_reconciliation) {
      throw RendererError(RendererErrorCode::InvalidRequest,
                          "upload GPU Scene",
                          "unchanged resident revision contains copy payload");
    }
    gpu_scene_buffers_.pending_source_id = reference_plan.source_id;
    gpu_scene_buffers_.pending_revision = reference_plan.revision;
    gpu_scene_buffers_.pending_draw_slot_indices = update->draw_slot_indices;
    gpu_scene_buffers_.pending_update = true;
    if (copy_bytes == 0) {
      return;
    }

    Buffer retired_staging;
    VkDeviceSize growth_bytes{};
    const auto reservation = gpu_scene_staging_.Reserve(
        static_cast<VkDeviceSize>(copy_bytes), retired_staging, growth_bytes);
    frame_counters_.gpu_scene_upload_ring_reserved_bytes +=
        AlignUp(static_cast<VkDeviceSize>(copy_bytes), kArenaAlignment);
    if (growth_bytes != 0) {
      ++frame_counters_.allocation_count;
      ++frame_counters_.buffer_allocation_count;
      frame_counters_.buffer_allocation_bytes += copy_bytes;
      ++frame_counters_.gpu_scene_upload_ring_growth_count;
      frame_counters_.gpu_scene_upload_ring_growth_bytes += growth_bytes;
      Retire(retired_staging);
    }

    VkDeviceSize cursor{};
    const auto stage_table = [&]<typename Record>(
                                 const render::GpuScenePackedUpdate<Record>& packed,
                                 const Buffer& destination) {
      for (const auto& range : packed.ranges) {
        const auto bytes = static_cast<VkDeviceSize>(range.records.size()) *
                           sizeof(Record);
        std::memcpy(reservation.mapped + cursor, range.records.data(),
                    static_cast<std::size_t>(bytes));
        pending_copies_.push_back(
            {reservation.buffer, reservation.offset + cursor,
             destination.handle,
             static_cast<VkDeviceSize>(range.first_slot) * sizeof(Record),
             bytes});
        cursor += bytes;
        ++frame_counters_.gpu_scene_copy_range_count;
      }
    };
    stage_table(update->geometries, gpu_scene_buffers_.geometries);
    stage_table(update->instances, gpu_scene_buffers_.instances);
    stage_table(update->materials, gpu_scene_buffers_.materials);
    stage_table(update->draws, gpu_scene_buffers_.draws);
    frame_counters_.gpu_scene_upload_bytes += copy_bytes;
    frame_counters_.upload_bytes += copy_bytes;
  }

  void CommitGpuSceneUpdate() noexcept {
    if (!gpu_scene_buffers_.pending_update) {
      return;
    }
    gpu_scene_buffers_.source_id = gpu_scene_buffers_.pending_source_id;
    gpu_scene_buffers_.revision = gpu_scene_buffers_.pending_revision;
    gpu_scene_buffers_.draw_slot_indices =
        std::move(gpu_scene_buffers_.pending_draw_slot_indices);
    gpu_scene_buffers_.has_resident_update = true;
    gpu_scene_buffers_.pending_update = false;
  }

  void PrepareGpuDrivenIndexed(FrameContext& frame,
                               const extraction::FrameSnapshot& snapshot,
                               const RenderRequest& request) {
    auto& resources = frame.gpu_driven;
    resources.selected = false;
    resources.candidate_count = 0;
    for (auto& batch : resources.batches) {
      batch.candidate_count = 0;
      batch.candidate_upload_pending = false;
    }
    if (request.gpu_driven_indexed.mode == GpuDrivenIndexedMode::Disabled ||
        draw_records_.size() == 0) {
      return;
    }
    const auto unavailable = [&](std::string detail) {
      if (request.gpu_driven_indexed.mode == GpuDrivenIndexedMode::Require) {
        throw RendererError(RendererErrorCode::Unsupported,
                            "select GPU-driven indexed Forward",
                            std::move(detail));
      }
      ++frame_counters_.gpu_driven_fallback_count;
    };
    if (!capabilities_.draw_indirect_first_instance) {
      unavailable("drawIndirectFirstInstance was not enabled");
      return;
    }
    if (!capabilities_.draw_indirect_count) {
      unavailable("drawIndirectCount was not enabled");
      return;
    }
    if (!capabilities_.shader_draw_parameters) {
      unavailable("shaderDrawParameters was not enabled");
      return;
    }
    if (!capabilities_.compute_queue) {
      unavailable("the selected graphics queue does not support compute");
      return;
    }
    const auto draw_slots =
        gpu_scene_buffers_.pending_update
            ? gpu_scene_buffers_.pending_draw_slot_indices
            : (gpu_scene_buffers_.has_resident_update &&
                       gpu_scene_buffers_.source_id == snapshot.source_id &&
                       gpu_scene_buffers_.revision == snapshot.revision
                   ? gpu_scene_buffers_.draw_slot_indices
                   : nullptr);
    if (!bindless_texture_table_ || !draw_slots ||
        draw_slots->size() != draw_records_.size()) {
      unavailable("persistent bindless GPU Scene state is unavailable");
      return;
    }
    if (std::any_of(selected_material_artifacts_.begin(),
                    selected_material_artifacts_.end(),
                    [](const auto* artifact) { return artifact != nullptr; })) {
      unavailable("generated material pipelines require conventional submission");
      return;
    }

    constexpr auto pipeline_state_mask =
        kMaskedAlphaFlag | kDoubleSidedFlag |
        kCounterClockwiseFrontFaceFlag;

    struct BatchSelection {
      std::uint32_t vertex_block{};
      std::uint32_t index_block{};
      std::uint32_t pipeline_variant{};
      VkDeviceSize candidate_offset{};
      VkDeviceSize command_offset{};
      VkDeviceSize counter_offset{};
      std::vector<std::uint32_t> draw_slots;
    };
    std::vector<BatchSelection> selections;
    selections.reserve(draw_records_.size());
    for (std::size_t i = 0; i < draw_records_.size(); ++i) {
      const auto& draw = draw_records_[i];
      const auto& geometry = geometry_records_[draw.geometry_index];
      const auto& slot = geometry_slots_.at(geometry.mesh);
      const auto variant = MakeDrawPipelineVariant(draw, snapshot);
      const auto masked_variant = variant.variant_key & pipeline_state_mask;
      if (selections.empty() ||
          selections.back().vertex_block != slot.vertices.block ||
          selections.back().index_block != slot.indices.block ||
          (selections.back().pipeline_variant & pipeline_state_mask) !=
              masked_variant) {
        selections.push_back({slot.vertices.block, slot.indices.block,
                              variant.variant_key, 0, 0, 0, {}});
      }
      selections.back().draw_slots.push_back((*draw_slots)[i]);
    }
    const auto oversized_batch =
        std::any_of(selections.begin(), selections.end(),
                    [&](const auto& batch) {
                      return batch.draw_slots.size() >
                             max_draw_indirect_count_;
                    });
    if (oversized_batch) {
      unavailable("an arena/pipeline batch exceeds maxDrawIndirectCount");
      return;
    }
    const auto excessive_compute_batch =
        std::any_of(selections.begin(), selections.end(),
                    [&](const auto& batch) {
                      return shader_abi::GpuDrivenIndexedWorkgroupCount(
                                 static_cast<std::uint32_t>(
                                     batch.draw_slots.size())) >
                             max_compute_work_group_count_x_;
                    });
    if (excessive_compute_batch) {
      unavailable(
          "an arena/pipeline batch exceeds maxComputeWorkGroupCount[0]");
      return;
    }

    VkDeviceSize candidate_buffer_bytes{};
    VkDeviceSize command_buffer_bytes{};
    VkDeviceSize counter_buffer_bytes{};
    bool unsupported_batch_range{};
    for (auto& selection : selections) {
      const auto candidate_bytes =
          static_cast<VkDeviceSize>(selection.draw_slots.size()) *
          sizeof(std::uint32_t);
      const auto command_bytes =
          static_cast<VkDeviceSize>(selection.draw_slots.size()) *
          sizeof(render::GpuIndexedIndirectCommand);
      unsupported_batch_range =
          unsupported_batch_range ||
          candidate_bytes > max_storage_buffer_range_ ||
          command_bytes > max_storage_buffer_range_;
      candidate_buffer_bytes =
          AlignUp(candidate_buffer_bytes, storage_buffer_alignment_);
      command_buffer_bytes =
          AlignUp(command_buffer_bytes, storage_buffer_alignment_);
      counter_buffer_bytes =
          AlignUp(counter_buffer_bytes, storage_buffer_alignment_);
      selection.candidate_offset = candidate_buffer_bytes;
      selection.command_offset = command_buffer_bytes;
      selection.counter_offset = counter_buffer_bytes;
      candidate_buffer_bytes += candidate_bytes;
      command_buffer_bytes += command_bytes;
      counter_buffer_bytes +=
          sizeof(shader_abi::GpuDrivenIndexedDispatchCounters);
    }
    if (unsupported_batch_range) {
      unavailable("an arena/pipeline batch exceeds maxStorageBufferRange");
      return;
    }

    const auto compute_pipeline =
        EnsureGpuDrivenComputePipeline(request.shaders);
    EnsureGpuDrivenFrameResources(
        resources, candidate_buffer_bytes, command_buffer_bytes,
        counter_buffer_bytes,
        static_cast<std::uint32_t>(selections.size()));
    resources.batches.resize(selections.size());
    VkDeviceSize candidate_upload_bytes{};
    for (std::size_t i = 0; i < selections.size(); ++i) {
      auto& batch = resources.batches[i];
      const auto& selection = selections[i];
      if (batch.candidate_draw_slot_shadow != selection.draw_slots ||
          batch.candidate_draw_slot_shadow_offset !=
              selection.candidate_offset) {
        candidate_upload_bytes +=
            static_cast<VkDeviceSize>(selection.draw_slots.size()) *
            sizeof(std::uint32_t);
        batch.candidate_upload_pending = true;
      }
      batch.candidate_count =
          static_cast<std::uint32_t>(selection.draw_slots.size());
      batch.descriptor_set = resources.descriptor_sets[i];
      batch.candidate_offset = selection.candidate_offset;
      batch.command_offset = selection.command_offset;
      batch.counter_offset = selection.counter_offset;
      batch.vertex_block = selection.vertex_block;
      batch.index_block = selection.index_block;
      batch.pipeline_variant = selection.pipeline_variant;
      batch.compute_pipeline = compute_pipeline;
    }
    UpdateGpuDrivenBatchDescriptors(resources);
    if (candidate_upload_bytes != 0) {
      Buffer retired_staging;
      VkDeviceSize growth_bytes{};
      const auto reservation = gpu_driven_staging_.Reserve(
          candidate_upload_bytes, retired_staging, growth_bytes);
      if (growth_bytes != 0) {
        ++frame_counters_.allocation_count;
        ++frame_counters_.buffer_allocation_count;
        frame_counters_.buffer_allocation_bytes += growth_bytes;
        Retire(retired_staging);
      }
      VkDeviceSize source_offset{};
      for (std::size_t i = 0; i < selections.size(); ++i) {
        auto& batch = resources.batches[i];
        const auto& draw_slot_selection = selections[i].draw_slots;
        if (!batch.candidate_upload_pending) {
          continue;
        }
        const auto bytes =
            static_cast<VkDeviceSize>(draw_slot_selection.size()) *
            sizeof(std::uint32_t);
        std::memcpy(reservation.mapped + source_offset,
                    draw_slot_selection.data(),
                    static_cast<std::size_t>(bytes));
        pending_copies_.push_back(
            {reservation.buffer, reservation.offset + source_offset,
             resources.candidate_draw_slots.handle, batch.candidate_offset,
             bytes});
        batch.pending_candidate_draw_slot_shadow = draw_slot_selection;
        batch.pending_candidate_draw_slot_shadow_offset =
            batch.candidate_offset;
        source_offset += bytes;
      }
      frame_counters_.gpu_driven_candidate_upload_bytes =
          candidate_upload_bytes;
      frame_counters_.upload_bytes += candidate_upload_bytes;
    }
    frame_counters_.gpu_driven_candidate_draw_count = draw_slots->size();
    resources.candidate_count =
        static_cast<std::uint32_t>(draw_slots->size());
    resources.selected = true;
  }

  static void CommitGpuDrivenCandidates(FrameContext& frame) noexcept {
    auto& resources = frame.gpu_driven;
    for (auto& batch : resources.batches) {
      if (!batch.candidate_upload_pending) {
        continue;
      }
      batch.candidate_draw_slot_shadow =
          std::move(batch.pending_candidate_draw_slot_shadow);
      batch.candidate_draw_slot_shadow_offset =
          batch.pending_candidate_draw_slot_shadow_offset;
      batch.candidate_upload_pending = false;
    }
  }

  static void InvalidateGpuDrivenCandidates(FrameContext& frame) noexcept {
    auto& resources = frame.gpu_driven;
    for (auto& batch : resources.batches) {
      batch.candidate_draw_slot_shadow.clear();
      batch.pending_candidate_draw_slot_shadow.clear();
      batch.candidate_draw_slot_shadow_offset = 0;
      batch.pending_candidate_draw_slot_shadow_offset = 0;
      batch.candidate_upload_pending = false;
    }
  }

  static bool HasPendingGpuDrivenCandidateUpload(
      const FrameContext& frame) noexcept {
    return std::any_of(
        frame.gpu_driven.batches.begin(), frame.gpu_driven.batches.end(),
        [](const auto& batch) { return batch.candidate_upload_pending; });
  }

  void InvalidateGpuSceneUpdate() noexcept {
    gpu_scene_buffers_.source_id = 0;
    gpu_scene_buffers_.revision = 0;
    gpu_scene_buffers_.pending_source_id = 0;
    gpu_scene_buffers_.pending_revision = 0;
    gpu_scene_buffers_.draw_slot_indices.reset();
    gpu_scene_buffers_.pending_draw_slot_indices.reset();
    gpu_scene_buffers_.has_resident_update = false;
    gpu_scene_buffers_.pending_update = false;
  }

  void CreateGaussianCornerBuffer() {
    constexpr std::array<Vec2, 6> corners{{
        {-1.0F, -1.0F},
        {1.0F, -1.0F},
        {1.0F, 1.0F},
        {-1.0F, -1.0F},
        {1.0F, 1.0F},
        {-1.0F, 1.0F},
    }};
    gaussian_corner_vertices_ = CreateBuffer(
        sizeof(corners), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* mapped{};
    Check(vkMapMemory(device_, gaussian_corner_vertices_.memory, 0,
                      sizeof(corners), 0, &mapped),
          "map Gaussian corner vertices");
    std::memcpy(mapped, corners.data(), sizeof(corners));
    vkUnmapMemory(device_, gaussian_corner_vertices_.memory);
  }

  void DestroyTexture(TextureSlot& texture) noexcept {
    if (texture.view != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, texture.view, nullptr);
    }
    if (texture.image != VK_NULL_HANDLE) {
      vkDestroyImage(device_, texture.image, nullptr);
    }
    if (texture.memory != VK_NULL_HANDLE) {
      memory_budget_.Free(texture.memory);
    }
    texture = {};
  }

  void ResetAbandonedUploads() {
    pending_copies_.clear();
    pending_image_copies_.clear();
    pending_graphics_acquire_images_.clear();
    staging_.AbandonFrame();
    gaussian_staging_.AbandonFrame();
    gpu_scene_staging_.AbandonFrame();
    gpu_driven_staging_.AbandonFrame();
    gpu_scene_buffers_.pending_draw_slot_indices.reset();
    gpu_scene_buffers_.pending_update = false;
    for (const auto handle : pending_texture_handles_) {
      const auto texture = texture_slots_.find(handle);
      if (texture != texture_slots_.end() &&
          texture->second.pending_upload) {
        if (texture->second.bindless_slot) {
          RetireTexture(texture->second);
        } else {
          DestroyTexture(texture->second);
        }
      }
    }
    pending_texture_handles_.clear();
    for (auto& texture : reserved_bindless_textures_) {
      if (texture.pending_upload) {
        DestroyTexture(texture);
        force_reserved_bindless_texture_writes_ = true;
      }
    }
    if (fallback_texture_.pending_upload) {
      DestroyTexture(fallback_texture_);
    }
    for (auto& buffer : frame_upload_buffers_) {
      DestroyBuffer(buffer);
    }
    frame_upload_buffers_.clear();
    if (resource_residency_dirty_) {
      // A failed submission produces no completion token, so Resolve cannot
      // collect resources retired while preparing that submission. Reclaim
      // only generations already known to be complete before retrying.
      // Bindless dirty slots remain queued until PrepareBindlessDescriptors has
      // recreated all required fallback images.
      CollectDeferred(latest_completed_value_, false);
    }
  }

  TextureSlot CreateTextureResource(
      std::uint32_t width, std::uint32_t height, std::uint64_t revision,
      const std::vector<std::uint8_t>& pixels, bool count_upload = true) {
    TextureSlot slot;
    slot.revision = revision;
    Buffer staging;
    try {
      slot.image = CreateImage(
          width, height, kColorFormat,
          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
          slot.memory);
      slot.view =
          CreateImageView(slot.image, kColorFormat, VK_IMAGE_ASPECT_COLOR_BIT);
      slot.pending_upload = true;
      staging = CreateBuffer(
          static_cast<VkDeviceSize>(pixels.size()),
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      void* mapped{};
      Check(vkMapMemory(device_, staging.memory, 0, staging.size, 0, &mapped),
            "map texture staging buffer");
      std::memcpy(mapped, pixels.data(), pixels.size());
      vkUnmapMemory(device_, staging.memory);
      pending_image_copies_.push_back(
          {staging.handle, slot.image, width, height});
      frame_upload_buffers_.push_back(staging);
      staging = {};
      if (count_upload) {
        frame_counters_.upload_bytes += pixels.size();
        frame_counters_.texture_upload_bytes += pixels.size();
      }
    } catch (...) {
      DestroyBuffer(staging);
      DestroyTexture(slot);
      throw;
    }
    return slot;
  }

  void CommitTextureUploads() noexcept {
    for (const auto handle : pending_texture_handles_) {
      const auto texture = texture_slots_.find(handle);
      if (texture != texture_slots_.end()) {
        texture->second.pending_upload = false;
      }
    }
    pending_texture_handles_.clear();
    for (auto& texture : reserved_bindless_textures_) {
      texture.pending_upload = false;
    }
    fallback_texture_.pending_upload = false;
  }

  VkSampler CreateSamplerResource(FilterMode min_filter, FilterMode mag_filter,
                                  AddressMode address_u,
                                  AddressMode address_v) {
    const auto filter = [](FilterMode value) {
      return value == FilterMode::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    };
    const auto address = [](AddressMode value) {
      switch (value) {
        case AddressMode::Repeat:
          return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case AddressMode::MirroredRepeat:
          return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case AddressMode::ClampToEdge:
          return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      }
      return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    };
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.minFilter = filter(min_filter);
    info.magFilter = filter(mag_filter);
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = address(address_u);
    info.addressModeV = address(address_v);
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.minLod = 0.0F;
    info.maxLod = 0.0F;
    VkSampler sampler{};
    Check(vkCreateSampler(device_, &info, nullptr, &sampler), "create sampler");
    return sampler;
  }

  void CollectCompletedBindlessSlots(std::uint64_t completed) {
    if (!bindless_texture_table_) {
      return;
    }
    for (const auto slot : bindless_texture_table_->Collect(completed)) {
      bindless_texture_views_[slot.index] = VK_NULL_HANDLE;
    }
    for (const auto slot : bindless_sampler_table_->Collect(completed)) {
      bindless_samplers_[slot.index] = VK_NULL_HANDLE;
    }
  }

  void RetireTexture(TextureSlot& texture) {
    if (texture.image != VK_NULL_HANDLE) {
      if (texture.bindless_slot) {
        bindless_texture_table_->Retire(texture.bindless_slot,
                                        timeline_value_);
      }
      retired_textures_.push_back({texture, timeline_value_});
      texture = {};
      CollectCompletedBindlessSlots(latest_completed_value_);
    }
  }

  void RetireSampler(SamplerSlot& sampler) {
    if (sampler.sampler != VK_NULL_HANDLE) {
      bool bindless_slot_retired{};
      if (sampler.bindless_slot) {
        const auto slot = sampler.bindless_slot;
        bindless_sampler_table_->Release(slot, timeline_value_);
        if (!bindless_sampler_table_->IsActive(slot)) {
          retired_samplers_.push_back(
              {sampler.sampler, timeline_value_, slot});
          bindless_slot_retired = true;
        }
      } else {
        retired_samplers_.push_back(
            {sampler.sampler, timeline_value_, {}});
      }
      sampler = {};
      if (bindless_slot_retired) {
        CollectCompletedBindlessSlots(latest_completed_value_);
      }
    }
  }

  void SyncTextures(const extraction::FrameSnapshot& snapshot,
                    ResourceSyncMode mode) {
    if (mode == ResourceSyncMode::Unchanged) {
      frame_counters_.texture_cache_hits += snapshot.textures.size();
      return;
    }

    std::vector<const extraction::TextureRecord*> records;
    if (mode == ResourceSyncMode::ReplaceAll) {
      for (auto& [handle, slot] : texture_slots_) {
        (void)handle;
        RetireTexture(slot);
        ++frame_counters_.texture_reconcile_count;
      }
      texture_slots_.clear();
      records.reserve(snapshot.textures.size());
      for (const auto& texture : snapshot.textures) {
        records.push_back(&texture);
      }
    } else if (mode == ResourceSyncMode::Full) {
      std::set<std::uint64_t> snapshot_handles;
      for (const auto& texture : snapshot.textures) {
        snapshot_handles.insert(texture.texture);
      }
      for (auto slot = texture_slots_.begin(); slot != texture_slots_.end();) {
        if (!snapshot_handles.contains(slot->first)) {
          RetireTexture(slot->second);
          slot = texture_slots_.erase(slot);
          ++frame_counters_.texture_reconcile_count;
        } else {
          ++slot;
        }
      }
      records.reserve(snapshot.textures.size());
      for (const auto& texture : snapshot.textures) {
        records.push_back(&texture);
      }
    } else {
      const auto& delta = snapshot.delta->textures;
      for (const auto handle : delta.removals) {
        ++frame_counters_.texture_reconcile_count;
        const auto slot = texture_slots_.find(handle);
        if (slot != texture_slots_.end()) {
          RetireTexture(slot->second);
          texture_slots_.erase(slot);
        }
      }
      records.reserve(delta.upserts.size());
      for (std::size_t i = 0; i < delta.upserts.size(); ++i) {
        const auto* record = FindDeltaRecord(
            snapshot.textures, delta, i,
            [](const extraction::TextureRecord& texture) {
              return texture.texture;
            });
        if (!record) {
          throw RendererError(RendererErrorCode::InvalidRequest,
                              "synchronize textures",
                              "snapshot texture delta has no matching record");
        }
        records.push_back(record);
      }
      frame_counters_.texture_cache_hits +=
          snapshot.textures.size() - records.size();
    }

    frame_counters_.texture_reconcile_count += records.size();
    for (const auto* texture_record : records) {
      const auto& texture = *texture_record;
      if (!texture.pixels) {
        throw RendererError(RendererErrorCode::InvalidRequest,
                            "synchronize textures",
                            "texture payload is null");
      }
      const auto [entry, inserted] =
          texture_slots_.try_emplace(texture.texture);
      if (!inserted && entry->second.revision == texture.revision) {
        ++frame_counters_.texture_cache_hits;
        continue;
      }
      ++frame_counters_.texture_cache_misses;
      RetireTexture(entry->second);
      auto replacement = CreateTextureResource(
          texture.width, texture.height, texture.revision, *texture.pixels);
      try {
        if (bindless_texture_table_) {
          replacement.bindless_slot = bindless_texture_table_->Allocate();
          bindless_texture_views_[replacement.bindless_slot.index] =
              replacement.view;
        }
      } catch (...) {
        DestroyTexture(replacement);
        throw;
      }
      entry->second = replacement;
      pending_texture_handles_.push_back(texture.texture);
    }
  }

  void SyncSamplers(const extraction::FrameSnapshot& snapshot,
                    ResourceSyncMode mode) {
    if (mode == ResourceSyncMode::Unchanged) {
      frame_counters_.sampler_cache_hits += snapshot.samplers.size();
      return;
    }

    std::vector<const extraction::SamplerRecord*> records;
    if (mode == ResourceSyncMode::ReplaceAll) {
      for (auto& [handle, slot] : sampler_slots_) {
        (void)handle;
        RetireSampler(slot);
        ++frame_counters_.sampler_reconcile_count;
      }
      sampler_slots_.clear();
      records.reserve(snapshot.samplers.size());
      for (const auto& sampler : snapshot.samplers) {
        records.push_back(&sampler);
      }
    } else if (mode == ResourceSyncMode::Full) {
      std::set<std::uint64_t> snapshot_handles;
      for (const auto& sampler : snapshot.samplers) {
        snapshot_handles.insert(sampler.sampler);
      }
      for (auto slot = sampler_slots_.begin(); slot != sampler_slots_.end();) {
        if (!snapshot_handles.contains(slot->first)) {
          RetireSampler(slot->second);
          slot = sampler_slots_.erase(slot);
          ++frame_counters_.sampler_reconcile_count;
        } else {
          ++slot;
        }
      }
      records.reserve(snapshot.samplers.size());
      for (const auto& sampler : snapshot.samplers) {
        records.push_back(&sampler);
      }
    } else {
      const auto& delta = snapshot.delta->samplers;
      for (const auto handle : delta.removals) {
        ++frame_counters_.sampler_reconcile_count;
        const auto slot = sampler_slots_.find(handle);
        if (slot != sampler_slots_.end()) {
          RetireSampler(slot->second);
          sampler_slots_.erase(slot);
        }
      }
      records.reserve(delta.upserts.size());
      for (std::size_t i = 0; i < delta.upserts.size(); ++i) {
        const auto* record = FindDeltaRecord(
            snapshot.samplers, delta, i,
            [](const extraction::SamplerRecord& sampler) {
              return sampler.sampler;
            });
        if (!record) {
          throw RendererError(RendererErrorCode::InvalidRequest,
                              "synchronize samplers",
                              "snapshot sampler delta has no matching record");
        }
        records.push_back(record);
      }
      frame_counters_.sampler_cache_hits +=
          snapshot.samplers.size() - records.size();
    }

    frame_counters_.sampler_reconcile_count += records.size();
    for (const auto* sampler_record : records) {
      const auto& sampler = *sampler_record;
      const auto [entry, inserted] = sampler_slots_.try_emplace(sampler.sampler);
      if (!inserted && entry->second.revision == sampler.revision) {
        ++frame_counters_.sampler_cache_hits;
        continue;
      }
      ++frame_counters_.sampler_cache_misses;
      RetireSampler(entry->second);
      SamplerSlot replacement;
      replacement.revision = sampler.revision;
      if (bindless_sampler_table_) {
        BindlessSamplerDescriptor descriptor;
        descriptor.min_filter = sampler.min_filter;
        descriptor.mag_filter = sampler.mag_filter;
        descriptor.address_u = sampler.address_u;
        descriptor.address_v = sampler.address_v;
        const auto slot = bindless_sampler_table_->Acquire(descriptor);
        replacement.bindless_slot = slot;
        if (bindless_samplers_[slot.index] == VK_NULL_HANDLE) {
          try {
            bindless_samplers_[slot.index] = CreateSamplerResource(
                sampler.min_filter, sampler.mag_filter, sampler.address_u,
                sampler.address_v);
          } catch (...) {
            bindless_sampler_table_->Release(slot, latest_completed_value_);
            (void)bindless_sampler_table_->Collect(latest_completed_value_);
            throw;
          }
        }
        replacement.sampler = bindless_samplers_[slot.index];
      } else {
        replacement.sampler = CreateSamplerResource(
            sampler.min_filter, sampler.mag_filter, sampler.address_u,
            sampler.address_v);
      }
      entry->second = replacement;
    }
  }

  void EnsureFallbackTextureAndSampler() {
    if (bindless_texture_table_) {
      EnsureReservedBindlessTextures();
    } else if (fallback_texture_.image == VK_NULL_HANDLE) {
      const std::vector<std::uint8_t> white{255U, 255U, 255U, 255U};
      fallback_texture_ = CreateTextureResource(1, 1, 1, white, false);
    }
    if (fallback_sampler_.sampler == VK_NULL_HANDLE) {
      fallback_sampler_.revision = 1;
      fallback_sampler_.sampler = CreateSamplerResource(
          FilterMode::Linear, FilterMode::Linear, AddressMode::Repeat,
          AddressMode::Repeat);
    }
  }

  void EnsureReservedBindlessTextures() {
    if (!bindless_texture_table_) {
      return;
    }
    static const std::array<std::array<std::uint8_t, 4>,
                            kReservedBindlessTextureSlots>
        pixels{{
            {255U, 255U, 255U, 255U},
            {0U, 0U, 0U, 255U},
            {128U, 128U, 255U, 255U},
            {255U, 0U, 255U, 255U},
        }};
    for (std::uint32_t index = 0; index < pixels.size(); ++index) {
      auto& texture = reserved_bindless_textures_[index];
      if (texture.image == VK_NULL_HANDLE) {
        texture = CreateTextureResource(
            1, 1, 1,
            std::vector<std::uint8_t>(pixels[index].begin(),
                                      pixels[index].end()),
            false);
        bindless_texture_views_[index] = texture.view;
        force_reserved_bindless_texture_writes_ = true;
      }
    }
  }

  [[nodiscard]] VkImageView FallbackTextureView() const noexcept {
    if (bindless_texture_table_) {
      return reserved_bindless_textures_[static_cast<std::uint32_t>(
                                             BindlessFallbackTexture::White)]
          .view;
    }
    return fallback_texture_.view;
  }

  void EnsureBindlessDescriptorSet() {
    if (!bindless_texture_table_ ||
        bindless_descriptor_set_ != VK_NULL_HANDLE) {
      return;
    }
    const auto& selection = capabilities_.descriptor_indexing_selection;
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding = shader_abi::kBindlessSamplers.binding;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[0].descriptorCount = selection.sampler_capacity;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = shader_abi::kBindlessTextures.binding;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[1].descriptorCount = selection.texture_capacity;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    constexpr auto common_flags =
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    const std::array<VkDescriptorBindingFlags, 2> binding_flags{
        common_flags,
        common_flags | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flags_info.bindingCount = static_cast<std::uint32_t>(binding_flags.size());
    flags_info.pBindingFlags = binding_flags.data();
    VkDescriptorSetLayoutCreateInfo layout_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.pNext = &flags_info;
    layout_info.flags =
        VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layout_info.pBindings = bindings.data();
    Check(vkCreateDescriptorSetLayout(device_, &layout_info, nullptr,
                                      &bindless_descriptor_set_layout_),
          "create bindless resource descriptor layout");
    ++frame_counters_.descriptor_layout_cache_misses;

    const std::array<VkDescriptorPoolSize, 2> sizes{{
        {VK_DESCRIPTOR_TYPE_SAMPLER, selection.sampler_capacity},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, selection.texture_capacity},
    }};
    VkDescriptorPoolCreateInfo pool_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    pool_info.pPoolSizes = sizes.data();
    Check(vkCreateDescriptorPool(device_, &pool_info, nullptr,
                                 &bindless_descriptor_pool_),
          "create bindless resource descriptor pool");
    ++frame_counters_.descriptor_pool_creation_count;

    VkDescriptorSetVariableDescriptorCountAllocateInfo variable_count{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
    variable_count.descriptorSetCount = 1;
    variable_count.pDescriptorCounts = &selection.texture_capacity;
    VkDescriptorSetAllocateInfo allocate{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.pNext = &variable_count;
    allocate.descriptorPool = bindless_descriptor_pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &bindless_descriptor_set_layout_;
    Check(vkAllocateDescriptorSets(device_, &allocate,
                                   &bindless_descriptor_set_),
          "allocate bindless resource descriptor set");
    ++frame_counters_.descriptor_allocation_count;
  }

  void EnsureBindlessMaterialDescriptorSetLayout() {
    if (bindless_material_descriptor_set_layout_ != VK_NULL_HANDLE) {
      ++frame_counters_.descriptor_layout_cache_hits;
      return;
    }
    std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
    bindings[0].binding = shader_abi::kBindlessMaterialConstants.binding;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    const std::array table_bindings{
        shader_abi::kGpuSceneGeometries.binding,
        shader_abi::kGpuSceneInstances.binding,
        shader_abi::kGpuSceneMaterials.binding,
        shader_abi::kGpuSceneDraws.binding,
    };
    for (std::size_t i = 0; i < table_bindings.size(); ++i) {
      auto& binding = bindings[i + 1];
      binding.binding = table_bindings[i];
      binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      binding.descriptorCount = 1;
      binding.stageFlags =
          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
          VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    Check(vkCreateDescriptorSetLayout(
              device_, &info, nullptr,
              &bindless_material_descriptor_set_layout_),
          "create bindless material descriptor layout");
    ++frame_counters_.descriptor_layout_cache_misses;
  }

  void EnsureGpuDrivenDescriptorAndPipelineLayouts() {
    if (gpu_driven_descriptor_set_layout_ != VK_NULL_HANDLE) {
      return;
    }
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    const std::array binding_numbers{
        shader_abi::kGpuDrivenCandidateDrawSlots.binding,
        shader_abi::kGpuDrivenCandidateResults.binding,
        shader_abi::kGpuDrivenIndirectCommands.binding,
        shader_abi::kGpuDrivenDispatchCounters.binding,
    };
    for (std::size_t i = 0; i < bindings.size(); ++i) {
      bindings[i].binding = binding_numbers[i];
      bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layout_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layout_info.pBindings = bindings.data();
    Check(vkCreateDescriptorSetLayout(device_, &layout_info, nullptr,
                                      &gpu_driven_descriptor_set_layout_),
          "create GPU-driven descriptor layout");
    ++frame_counters_.descriptor_layout_cache_misses;

    const std::array set_layouts{
        bindless_descriptor_set_layout_,
        bindless_material_descriptor_set_layout_,
        gpu_driven_descriptor_set_layout_,
    };
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.size = sizeof(shader_abi::GpuDrivenIndexedConstants);
    VkPipelineLayoutCreateInfo pipeline_layout_info{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout_info.setLayoutCount =
        static_cast<std::uint32_t>(set_layouts.size());
    pipeline_layout_info.pSetLayouts = set_layouts.data();
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    Check(vkCreatePipelineLayout(device_, &pipeline_layout_info, nullptr,
                                 &gpu_driven_pipeline_layout_),
          "create GPU-driven pipeline layout");
  }

  VkPipeline EnsureGpuDrivenComputePipeline(const ShaderPaths& shaders) {
    EnsureGpuDrivenDescriptorAndPipelineLayouts();
    const auto path = shaders.gpu_driven_compute.empty()
                          ? shaders.bindless_vertex.parent_path() /
                                "gpu-driven-indexed.comp.spv"
                          : shaders.gpu_driven_compute;
    const auto found = gpu_driven_pipelines_.find(path);
    if (found != gpu_driven_pipelines_.end()) {
      ++frame_counters_.pipeline_cache_hits;
      return found->second;
    }
    VkPipelineShaderStageCreateInfo stage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = GetShaderModule(path);
    stage.pName = "main";
    VkComputePipelineCreateInfo info{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = stage;
    info.layout = gpu_driven_pipeline_layout_;
    VkPipeline pipeline{};
    Check(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr,
                                   &pipeline),
          "create GPU-driven compute pipeline");
    gpu_driven_pipelines_.emplace(path, pipeline);
    ++frame_counters_.pipeline_creation_count;
    ++frame_counters_.pipeline_cache_misses;
    return pipeline;
  }

  void EnsureGpuDrivenFrameResources(GpuDrivenFrameResources& resources,
                                     VkDeviceSize candidate_bytes,
                                     VkDeviceSize command_bytes,
                                     VkDeviceSize counter_bytes,
                                     std::uint32_t batch_count) {
    if (resources.candidate_capacity_bytes >= candidate_bytes &&
        resources.command_capacity_bytes >= command_bytes &&
        resources.counter_capacity_bytes >= counter_bytes &&
        resources.batch_capacity >= batch_count &&
        resources.descriptor_pool != VK_NULL_HANDLE) {
      return;
    }
    DestroyGpuDrivenFrameResources(resources);
    resources.candidate_capacity_bytes =
        std::max<VkDeviceSize>(candidate_bytes, 1U);
    resources.command_capacity_bytes =
        std::max<VkDeviceSize>(command_bytes, 1U);
    resources.counter_capacity_bytes =
        std::max<VkDeviceSize>(counter_bytes, 1U);
    resources.batch_capacity = std::max(batch_count, 1U);
    resources.candidate_draw_slots = CreateBuffer(
        resources.candidate_capacity_bytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, queue_family_,
        transfer_queue_family_);
    resources.candidate_results = CreateBuffer(
        resources.candidate_capacity_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    resources.indirect_commands = CreateBuffer(
        resources.command_capacity_bytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    resources.dispatch_counters = CreateBuffer(
        resources.counter_capacity_bytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    resources.counter_readback = CreateBuffer(
        resources.counter_capacity_bytes,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    const VkDescriptorPoolSize pool_size{
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, resources.batch_capacity * 4U};
    VkDescriptorPoolCreateInfo pool_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = resources.batch_capacity;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    Check(vkCreateDescriptorPool(device_, &pool_info, nullptr,
                                 &resources.descriptor_pool),
          "create GPU-driven descriptor pool");
    ++frame_counters_.descriptor_pool_creation_count;
    VkDescriptorSetAllocateInfo allocate{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = resources.descriptor_pool;
    allocate.descriptorSetCount = resources.batch_capacity;
    const std::vector layouts(resources.batch_capacity,
                              gpu_driven_descriptor_set_layout_);
    allocate.pSetLayouts = layouts.data();
    resources.descriptor_sets.resize(resources.batch_capacity);
    Check(vkAllocateDescriptorSets(device_, &allocate,
                                   resources.descriptor_sets.data()),
          "allocate GPU-driven descriptor sets");
    frame_counters_.descriptor_allocation_count += resources.batch_capacity;
  }

  void UpdateGpuDrivenBatchDescriptors(
      const GpuDrivenFrameResources& resources) {
    const std::array descriptor_buffers{
        &resources.candidate_draw_slots, &resources.candidate_results,
        &resources.indirect_commands, &resources.dispatch_counters,
    };
    const std::array descriptor_bindings{
        shader_abi::kGpuDrivenCandidateDrawSlots.binding,
        shader_abi::kGpuDrivenCandidateResults.binding,
        shader_abi::kGpuDrivenIndirectCommands.binding,
        shader_abi::kGpuDrivenDispatchCounters.binding,
    };
    std::vector<VkDescriptorBufferInfo> buffer_infos(
        resources.batches.size() * descriptor_buffers.size());
    std::vector<VkWriteDescriptorSet> writes(buffer_infos.size());
    for (std::size_t batch_index = 0;
         batch_index < resources.batches.size(); ++batch_index) {
      const auto& batch = resources.batches[batch_index];
      const auto slot_bytes = static_cast<VkDeviceSize>(batch.candidate_count) *
                              sizeof(std::uint32_t);
      const auto command_bytes =
          static_cast<VkDeviceSize>(batch.candidate_count) *
          sizeof(render::GpuIndexedIndirectCommand);
      const std::array offsets{batch.candidate_offset, batch.candidate_offset,
                               batch.command_offset, batch.counter_offset};
      const std::array ranges{
          slot_bytes, slot_bytes, command_bytes,
          VkDeviceSize{sizeof(shader_abi::GpuDrivenIndexedDispatchCounters)}};
      for (std::size_t binding_index = 0;
           binding_index < descriptor_buffers.size(); ++binding_index) {
        const auto index =
            batch_index * descriptor_buffers.size() + binding_index;
        buffer_infos[index] = {descriptor_buffers[binding_index]->handle,
                               offsets[binding_index], ranges[binding_index]};
        writes[index] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[index].dstSet = batch.descriptor_set;
        writes[index].dstBinding = descriptor_bindings[binding_index];
        writes[index].descriptorCount = 1;
        writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[index].pBufferInfo = &buffer_infos[index];
      }
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
    frame_counters_.descriptor_update_count += writes.size();
  }

  void WriteBindlessDescriptors(
      const std::vector<std::uint32_t>& texture_indices,
      const std::vector<std::uint32_t>& sampler_indices) {
    std::vector<VkDescriptorImageInfo> texture_infos(texture_indices.size());
    std::vector<VkDescriptorImageInfo> sampler_infos(sampler_indices.size());
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(texture_indices.size() + sampler_indices.size());
    const auto error_view =
        reserved_bindless_textures_[static_cast<std::uint32_t>(
                                        BindlessFallbackTexture::Error)]
            .view;
    for (std::size_t i = 0; i < texture_indices.size(); ++i) {
      const auto index = texture_indices[i];
      const auto view = bindless_texture_views_[index] == VK_NULL_HANDLE
          ? error_view
          : bindless_texture_views_[index];
      texture_infos[i] =
          {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      write.dstSet = bindless_descriptor_set_;
      write.dstBinding = shader_abi::kBindlessTextures.binding;
      write.dstArrayElement = index;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      write.pImageInfo = &texture_infos[i];
      writes.push_back(write);
    }
    for (std::size_t i = 0; i < sampler_indices.size(); ++i) {
      const auto index = sampler_indices[i];
      const auto sampler = bindless_samplers_[index] == VK_NULL_HANDLE
          ? fallback_sampler_.sampler
          : bindless_samplers_[index];
      sampler_infos[i] = {sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
      VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      write.dstSet = bindless_descriptor_set_;
      write.dstBinding = shader_abi::kBindlessSamplers.binding;
      write.dstArrayElement = index;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
      write.pImageInfo = &sampler_infos[i];
      writes.push_back(write);
    }
    if (!writes.empty()) {
      vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                             writes.data(), 0, nullptr);
      frame_counters_.descriptor_update_count += writes.size();
      frame_counters_.bindless_sampled_image_descriptor_update_count +=
          texture_indices.size();
      frame_counters_.bindless_sampler_descriptor_update_count +=
          sampler_indices.size();
    }
  }

  void PrepareBindlessDescriptors() {
    if (!bindless_texture_table_) {
      return;
    }
    EnsureFallbackTextureAndSampler();
    EnsureBindlessDescriptorSet();

    auto texture_indices = bindless_texture_table_->ConsumeDirtySlots();
    if (force_reserved_bindless_texture_writes_) {
      for (std::uint32_t index = 0;
           index < kReservedBindlessTextureSlots; ++index) {
        texture_indices.push_back(index);
      }
      std::sort(texture_indices.begin(), texture_indices.end());
      texture_indices.erase(
          std::unique(texture_indices.begin(), texture_indices.end()),
          texture_indices.end());
    }
    const auto sampler_indices =
        bindless_sampler_table_->ConsumeDirtySlots();
    WriteBindlessDescriptors(texture_indices, sampler_indices);
    force_reserved_bindless_texture_writes_ = false;
  }

  void EnsureDescriptorSetLayout() {
    if (descriptor_set_layout_ != VK_NULL_HANDLE) {
      ++frame_counters_.descriptor_layout_cache_hits;
      return;
    }
    ++frame_counters_.descriptor_layout_cache_misses;
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding = shader_abi::kConventionalBaseColorTexture.binding;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = shader_abi::kConventionalMaterialConstants.binding;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
                             VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    Check(vkCreateDescriptorSetLayout(device_, &info, nullptr,
                                      &descriptor_set_layout_),
          "create material descriptor layout");
  }

  void EnsureGeneratedDescriptorSetLayouts() {
    for (const auto& [module_key, artifact] : generated_material_artifacts_) {
      if (generated_descriptor_set_layouts_.contains(module_key)) {
        ++frame_counters_.descriptor_layout_cache_hits;
        continue;
      }
      std::vector<VkDescriptorSetLayoutBinding> bindings;
      bindings.push_back(
          {artifact.parameter_binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
           VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
      for (const auto& resource : artifact.resource_bindings) {
        if (resource.type == MaterialValueType::Texture2D ||
            resource.type == MaterialValueType::CombinedTextureSampler) {
          bindings.push_back(
              {resource.texture_binding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
               resource.array_size, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
        }
        if (resource.type == MaterialValueType::Sampler ||
            resource.type == MaterialValueType::CombinedTextureSampler) {
          bindings.push_back(
              {resource.sampler_binding, VK_DESCRIPTOR_TYPE_SAMPLER,
               resource.array_size, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
        }
      }
      bindings.push_back(
          {artifact.material_constants_binding,
           VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
           nullptr});
      std::sort(bindings.begin(), bindings.end(),
                [](const auto& left, const auto& right) {
                  return left.binding < right.binding;
                });
      const auto duplicate = std::adjacent_find(
          bindings.begin(), bindings.end(), [](const auto& left,
                                                const auto& right) {
            return left.binding == right.binding;
          });
      if (duplicate != bindings.end() ||
          std::any_of(bindings.begin(), bindings.end(), [](const auto& binding) {
            return binding.descriptorCount == 0U;
          })) {
        continue;
      }
      VkDescriptorSetLayoutCreateInfo info{
          VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      info.bindingCount = static_cast<std::uint32_t>(bindings.size());
      info.pBindings = bindings.data();
      VkDescriptorSetLayout layout{};
      Check(vkCreateDescriptorSetLayout(device_, &info, nullptr, &layout),
            "create generated material descriptor layout");
      generated_descriptor_set_layouts_.emplace(module_key, layout);
      ++frame_counters_.descriptor_layout_cache_misses;
    }
  }

  struct DirectionalLighting {
    Vec4 direction_intensity{0.0F, 0.0F, 1.0F, 1.0F};
    Vec3 color{1.0F, 1.0F, 1.0F};
  };

  static DirectionalLighting ExtractDirectionalLighting(
      const extraction::FrameSnapshot& snapshot) {
    DirectionalLighting result;
    const auto directional =
        std::find_if(snapshot.lights.begin(), snapshot.lights.end(),
                     [](const extraction::LightRecord& light) {
                       return light.type == LightType::Directional;
                     });
    if (directional == snapshot.lights.end()) {
      return result;
    }
    // Directional lights emit along local -Z. Lambert shading needs the
    // opposite vector, from the surface toward the source, so transform +Z.
    auto x = directional->transform.values[8];
    auto y = directional->transform.values[9];
    auto z = directional->transform.values[10];
    const auto length = std::sqrt(x * x + y * y + z * z);
    if (length > 0.0F) {
      x /= length;
      y /= length;
      z /= length;
    } else {
      x = 0.0F;
      y = 0.0F;
      z = 1.0F;
    }
    result.direction_intensity = {x, y, z, directional->intensity};
    result.color = directional->color;
    return result;
  }

  void EnsureEnvironmentLighting(const std::filesystem::path& path) {
    if (path == environment_path_) {
      return;
    }
    auto replacement = detail::LoadDiffuseEnvironment(path);
    environment_lighting_ = std::move(replacement);
    environment_path_ = path;
  }

  MaterialUniforms MakeMaterialUniforms(
      const extraction::MaterialRecord& material,
      const DirectionalLighting& lighting) const {
    MaterialUniforms result{};
    result.base_color = material.parameters.base_color;
    result.light_direction_intensity = lighting.direction_intensity;
    result.light_color_alpha_cutoff = {
        lighting.color.x, lighting.color.y, lighting.color.z,
        material.parameters.alpha_cutoff};
    result.diffuse_environment = environment_lighting_.coefficients;
    return result;
  }

  void RejectGeneratedMaterial(std::size_t index,
                               MaterialDiagnosticCategory category,
                               std::string message) {
    const auto& module = *material_records_[index].module;
    selected_material_artifacts_[index] = nullptr;
    MaterialDiagnostic diagnostic;
    diagnostic.category = category;
    diagnostic.severity = DiagnosticSeverity::Error;
    diagnostic.fallback = MaterialFallback::BasicMaterial;
    diagnostic.message = std::move(message);
    diagnostic.context.material_identity = module.key;
    diagnostic.context.backend_target = "spirv";
    frame_counters_.material_fallbacks.Record(diagnostic.fallback);
    frame_material_diagnostics_.push_back(std::move(diagnostic));
  }

  static std::optional<std::size_t> PackedMaterialValueSize(
      MaterialValueType type) noexcept {
    switch (type) {
      case MaterialValueType::Float: return sizeof(float);
      case MaterialValueType::Float2: return sizeof(Vec2);
      case MaterialValueType::Float3: return sizeof(Vec3);
      case MaterialValueType::Float4: return sizeof(Vec4);
      case MaterialValueType::Integer: return sizeof(std::int32_t);
      case MaterialValueType::Boolean: return sizeof(std::uint32_t);
      default: return std::nullopt;
    }
  }

  static bool GeneratedBindingFits(
      const GeneratedMaterialParameterBinding& binding,
      std::uint32_t parameter_buffer_size) noexcept {
    const auto value_size = PackedMaterialValueSize(binding.type);
    if (!value_size || binding.array_size == 0U) {
      return false;
    }
    const auto buffer_size = static_cast<std::size_t>(parameter_buffer_size);
    const auto offset = static_cast<std::size_t>(binding.offset);
    if (offset > buffer_size || *value_size > buffer_size - offset) {
      return false;
    }
    if (binding.array_size == 1U) {
      return true;
    }
    const auto stride =
        binding.array_stride == 0U
            ? *value_size
            : static_cast<std::size_t>(binding.array_stride);
    if (stride < *value_size) {
      return false;
    }
    const auto remaining = buffer_size - offset - *value_size;
    return static_cast<std::size_t>(binding.array_size - 1U) <=
           remaining / stride;
  }

  void SelectGeneratedMaterials() {
    selected_material_artifacts_.assign(material_records_.size(), nullptr);
    frame_material_diagnostics_.clear();
    std::vector<bool> material_is_drawn(material_records_.size(), false);
    for (std::size_t draw_index = 0; draw_index < draw_records_.size();
         ++draw_index) {
      const auto material_index = draw_records_[draw_index].material_index;
      if (material_index < material_is_drawn.size()) {
        material_is_drawn[material_index] = true;
      }
    }
    for (std::size_t index = 0; index < material_records_.size(); ++index) {
      const auto& material = material_records_[index];
      if (!material.module || !material_is_drawn[index]) {
        continue;
      }
      const auto& module = *material.module;
      if (bindless_texture_table_) {
        RejectGeneratedMaterial(
            index, MaterialDiagnosticCategory::TargetFailure,
            "generated materials currently require conventional Vulkan "
            "descriptors");
        continue;
      }
      const auto found = generated_material_artifacts_.find(module.key);
      if (found == generated_material_artifacts_.end()) {
        RejectGeneratedMaterial(
            index, MaterialDiagnosticCategory::CacheIncompatible,
            "no Vulkan artifact is registered for the material module");
        continue;
      }
      const auto& artifact = found->second;
      bool concrete_resource_layout_matches =
          artifact.resource_bindings.size() == module.resources.entries.size() &&
          artifact.material_constants_binding ==
              shader_abi::kConventionalMaterialConstants.binding;
      for (const auto& resource : module.resources.entries) {
        const auto binding = std::find_if(
            artifact.resource_bindings.begin(),
            artifact.resource_bindings.end(), [&](const auto& candidate) {
              return candidate.name == resource.name &&
                     candidate.type == resource.type &&
                     candidate.array_size == resource.array_size;
            });
        concrete_resource_layout_matches =
            concrete_resource_layout_matches &&
            binding != artifact.resource_bindings.end();
      }
      if (!concrete_resource_layout_matches ||
          !generated_descriptor_set_layouts_.contains(module.key)) {
        RejectGeneratedMaterial(
            index, MaterialDiagnosticCategory::ReflectionMismatch,
            "the Vulkan artifact's concrete resource layout disagrees "
            "with the material module");
        continue;
      }
      auto diagnostics = VerifyMaterialAbi(module);
      auto reflected =
          VerifyMaterialTargetReflection(module, artifact.reflection);
      diagnostics.insert(diagnostics.end(),
                         std::make_move_iterator(reflected.begin()),
                         std::make_move_iterator(reflected.end()));
      bool concrete_layout_matches =
          artifact.parameter_bindings.size() ==
          module.parameters.entries.size();
      for (const auto& parameter : module.parameters.entries) {
        const auto binding = std::find_if(
            artifact.parameter_bindings.begin(),
            artifact.parameter_bindings.end(), [&](const auto& candidate) {
              return candidate.name == parameter.name &&
                     candidate.type == parameter.type &&
                     candidate.array_size == parameter.array_size &&
                     GeneratedBindingFits(
                         candidate, artifact.parameter_buffer_size);
            });
        concrete_layout_matches =
            concrete_layout_matches &&
            binding != artifact.parameter_bindings.end();
      }
      if (!concrete_layout_matches) {
        RejectGeneratedMaterial(
            index, MaterialDiagnosticCategory::ReflectionMismatch,
            "the Vulkan artifact's concrete parameter layout disagrees "
            "with the material module");
        continue;
      }
      bool resources_available = true;
      const auto has_texture = [&](std::uint64_t handle) {
        for (std::size_t i = 0; i < texture_records_.size(); ++i) {
          if (texture_records_[i].texture == handle) {
            return true;
          }
        }
        return false;
      };
      const auto has_sampler = [&](std::uint64_t handle) {
        for (std::size_t i = 0; i < sampler_records_.size(); ++i) {
          if (sampler_records_[i].sampler == handle) {
            return true;
          }
        }
        return false;
      };
      for (const auto& entry : material.generated_resources.entries) {
        for (const auto& value : entry.values) {
          if (value.texture.valid()) {
            resources_available =
                resources_available && has_texture(value.texture.value());
          }
          if (value.sampler.valid()) {
            resources_available =
                resources_available && has_sampler(value.sampler.value());
          }
        }
      }
      if (!resources_available) {
        RejectGeneratedMaterial(
            index, MaterialDiagnosticCategory::MissingTexture,
            "a generated material texture or sampler is unavailable");
        continue;
      }
      if (!diagnostics.empty()) {
        for (auto& diagnostic : diagnostics) {
          frame_counters_.material_fallbacks.Record(diagnostic.fallback);
          frame_material_diagnostics_.push_back(std::move(diagnostic));
        }
        continue;
      }
      try {
        (void)GetShaderModule(artifact.fragment);
      } catch (const RendererError& error) {
        if (error.code() == RendererErrorCode::DeviceLost ||
            error.code() == RendererErrorCode::ResourceExhausted) {
          throw;
        }
        RejectGeneratedMaterial(
            index, MaterialDiagnosticCategory::CacheCorrupt,
            "the registered Vulkan material artifact could not be loaded as "
            "SPIR-V");
        continue;
      }
      selected_material_artifacts_[index] = &artifact;
    }
  }

  static std::size_t MaterialValueSize(MaterialValueType type) {
    if (const auto size = PackedMaterialValueSize(type)) {
      return *size;
    }
    throw RendererError(RendererErrorCode::Unsupported,
                        "pack generated material parameters",
                        "unsupported generated material parameter type");
  }

  static void CopyMaterialValue(std::byte* destination,
                                const MaterialValue& value,
                                MaterialValueType type) {
    const auto copy = [&](const auto* typed) {
      if (typed == nullptr) {
        throw RendererError(RendererErrorCode::InvalidRequest,
                            "pack generated material parameters",
                            "generated material parameter value has the "
                            "wrong declared type");
      }
      std::memcpy(destination, typed, sizeof(*typed));
    };
    switch (type) {
      case MaterialValueType::Float:
        copy(std::get_if<float>(&value));
        break;
      case MaterialValueType::Float2:
        copy(std::get_if<Vec2>(&value));
        break;
      case MaterialValueType::Float3:
        copy(std::get_if<Vec3>(&value));
        break;
      case MaterialValueType::Float4:
        copy(std::get_if<Vec4>(&value));
        break;
      case MaterialValueType::Integer:
        copy(std::get_if<std::int32_t>(&value));
        break;
      case MaterialValueType::Boolean: {
        const auto* boolean = std::get_if<bool>(&value);
        if (boolean == nullptr) {
          throw RendererError(RendererErrorCode::InvalidRequest,
                              "pack generated material parameters",
                              "generated material boolean has the wrong "
                              "declared type");
        }
        const std::uint32_t encoded = *boolean ? 1U : 0U;
        std::memcpy(destination, &encoded, sizeof(encoded));
        break;
      }
      default:
        (void)MaterialValueSize(type);
    }
  }

  void PackGeneratedMaterialParameters(
      std::byte* destination, const extraction::MaterialRecord& material,
      const GeneratedMaterialArtifact& artifact) const {
    std::memset(destination, 0, artifact.parameter_buffer_size);
    for (const auto& binding : artifact.parameter_bindings) {
      const auto state = std::find_if(
          material.generated_parameters.entries.begin(),
          material.generated_parameters.entries.end(),
          [&](const auto& entry) { return entry.name == binding.name; });
      if (state == material.generated_parameters.entries.end() ||
          state->type != binding.type ||
          state->values.size() != binding.array_size) {
        throw RendererError(RendererErrorCode::InvalidRequest,
                            "pack generated material parameters",
                            "generated parameter state does not match the "
                            "registered Vulkan artifact");
      }
      const auto value_size = MaterialValueSize(binding.type);
      const auto stride =
          binding.array_stride == 0U ? value_size : binding.array_stride;
      const auto end = static_cast<std::size_t>(binding.offset) +
                       stride * (binding.array_size - 1U) + value_size;
      if (end > artifact.parameter_buffer_size) {
        throw RendererError(RendererErrorCode::InvalidRequest,
                            "pack generated material parameters",
                            "generated parameter binding exceeds its buffer");
      }
      for (std::size_t value_index = 0;
           value_index < state->values.size(); ++value_index) {
        CopyMaterialValue(destination + binding.offset + stride * value_index,
                          state->values[value_index], binding.type);
      }
    }
  }

  void PrepareBindlessMaterialDescriptors(
      FrameContext& frame, const extraction::FrameSnapshot& snapshot) {
    frame.material_descriptor_sets.clear();
    if (material_records_.empty()) {
      return;
    }
    const auto material_count =
        static_cast<std::uint32_t>(material_records_.size());
    const auto uniform_stride =
        AlignUp(sizeof(MaterialUniforms), uniform_buffer_alignment_);
    const auto uniform_bytes = uniform_stride * material_count;
    bool descriptor_dirty{};
    if (frame.material_uniforms.handle == VK_NULL_HANDLE ||
        frame.material_uniforms.size < uniform_bytes) {
      DestroyBuffer(frame.material_uniforms);
      frame.material_uniforms = CreateBuffer(
          uniform_bytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      descriptor_dirty = true;
    }
    if (frame.descriptor_pool == VK_NULL_HANDLE) {
      std::array<VkDescriptorPoolSize, 2> sizes{{
          {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1},
          {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4U},
      }};
      VkDescriptorPoolCreateInfo pool_info{
          VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
      pool_info.maxSets = 1;
      // The shared bindless layout contains the optional GPU Scene bindings.
      // Vulkan descriptor pools must cover every binding in an allocated set
      // even when the conventional bindless shader does not statically use it.
      pool_info.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
      pool_info.pPoolSizes = sizes.data();
      Check(vkCreateDescriptorPool(device_, &pool_info, nullptr,
                                   &frame.descriptor_pool),
            "create bindless material descriptor pool");
      ++frame_counters_.descriptor_pool_creation_count;
      VkDescriptorSetAllocateInfo allocate{
          VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      allocate.descriptorPool = frame.descriptor_pool;
      allocate.descriptorSetCount = 1;
      allocate.pSetLayouts = &bindless_material_descriptor_set_layout_;
      Check(vkAllocateDescriptorSets(
                device_, &allocate,
                &frame.bindless_material_descriptor_set),
            "allocate bindless material descriptor set");
      ++frame_counters_.descriptor_allocation_count;
      descriptor_dirty = true;
    }
    if (descriptor_dirty) {
      std::array<VkDescriptorBufferInfo, 5> buffer_infos{};
      buffer_infos[0] = {
          frame.material_uniforms.handle, 0, sizeof(MaterialUniforms)};
      std::array<VkWriteDescriptorSet, 5> writes{};
      writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[0].dstSet = frame.bindless_material_descriptor_set;
      writes[0].dstBinding =
          shader_abi::kBindlessMaterialConstants.binding;
      writes[0].descriptorCount = 1;
      writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      writes[0].pBufferInfo = &buffer_infos[0];
      std::uint32_t write_count = 1;
      if (gpu_scene_buffers_.enabled()) {
        const std::array<const Buffer*, 4> table_buffers{
            &gpu_scene_buffers_.geometries,
            &gpu_scene_buffers_.instances,
            &gpu_scene_buffers_.materials,
            &gpu_scene_buffers_.draws,
        };
        const std::array table_binding_indices{
            shader_abi::kGpuSceneGeometries.binding,
            shader_abi::kGpuSceneInstances.binding,
            shader_abi::kGpuSceneMaterials.binding,
            shader_abi::kGpuSceneDraws.binding,
        };
        for (std::size_t i = 0; i < table_buffers.size(); ++i) {
          buffer_infos[i + 1] = {
              table_buffers[i]->handle, 0, table_buffers[i]->size};
          auto& write = writes[i + 1];
          write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          write.dstSet = frame.bindless_material_descriptor_set;
          write.dstBinding = table_binding_indices[i];
          write.descriptorCount = 1;
          write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
          write.pBufferInfo = &buffer_infos[i + 1];
        }
        write_count = static_cast<std::uint32_t>(writes.size());
      }
      vkUpdateDescriptorSets(device_, write_count, writes.data(), 0, nullptr);
      frame_counters_.descriptor_update_count += write_count;
    }
    frame.material_uniform_stride = uniform_stride;

    void* mapped{};
    Check(vkMapMemory(device_, frame.material_uniforms.memory, 0,
                      uniform_bytes, 0, &mapped),
          "map bindless material uniform buffer");
    const auto lighting = ExtractDirectionalLighting(snapshot);
    for (std::uint32_t i = 0; i < material_count; ++i) {
      const auto& material = material_records_[i];
      const auto uniforms = MakeMaterialUniforms(material, lighting);
      std::memcpy(static_cast<std::byte*>(mapped) + uniform_stride * i,
                  &uniforms, sizeof(uniforms));
    }
    vkUnmapMemory(device_, frame.material_uniforms.memory);
  }

  void PrepareGaussianInstances(FrameContext& frame) {
    if (frame.gaussian_preparation_generation ==
        gaussian_preparation_generation_) {
      return;
    }
    if (prepared_gaussians_.gaussians.size() >
        std::numeric_limits<std::uint32_t>::max()) {
      throw RendererError(RendererErrorCode::Unsupported,
                          "upload prepared Gaussian stream",
                          "visible Gaussian count exceeds uint32 draw limit");
    }
    if (prepared_gaussians_.gaussians.empty()) {
      frame.gaussian_instance_count = 0;
      frame.gaussian_instance_shadow.clear();
      frame.gaussian_preparation_generation = gaussian_preparation_generation_;
      return;
    }
    std::vector<GaussianGpuInstance> packed;
    packed.reserve(prepared_gaussians_.gaussians.size());
    for (const auto& source : prepared_gaussians_.gaussians) {
      packed.push_back({
          source.center_pixels,
          source.inverse_conic,
          source.radiance,
          source.opacity,
          source.radius_pixels,
          source.depth,
          static_cast<std::uint32_t>(source.resource),
          source.particle,
      });
    }
    const auto bytes = static_cast<VkDeviceSize>(
        packed.size() * sizeof(GaussianGpuInstance));
    bool replaced = false;
    if (frame.gaussian_instances.handle == VK_NULL_HANDLE ||
        frame.gaussian_instances.size < bytes) {
      auto replacement = CreateBuffer(
          bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      DestroyBuffer(frame.gaussian_instances);
      frame.gaussian_instances = replacement;
      replaced = true;
    }
    struct InstanceRange {
      std::size_t first{};
      std::size_t count{};
    };
    std::vector<InstanceRange> changed_ranges;
    if (replaced) {
      changed_ranges.push_back({0, packed.size()});
    } else {
      const auto shared_count =
          std::min(frame.gaussian_instance_shadow.size(), packed.size());
      std::size_t index{};
      while (index < shared_count) {
        if (std::memcmp(&frame.gaussian_instance_shadow[index], &packed[index],
                        sizeof(GaussianGpuInstance)) == 0) {
          ++index;
          continue;
        }
        const auto first = index++;
        while (index < shared_count &&
               std::memcmp(&frame.gaussian_instance_shadow[index],
                           &packed[index], sizeof(GaussianGpuInstance)) != 0) {
          ++index;
        }
        changed_ranges.push_back({first, index - first});
      }
      if (packed.size() > shared_count) {
        changed_ranges.push_back(
            {shared_count, packed.size() - shared_count});
      }
    }
    std::uint64_t uploaded_bytes{};
    for (const auto& range : changed_ranges) {
      uploaded_bytes += range.count * sizeof(GaussianGpuInstance);
    }
    if (uploaded_bytes != 0) {
      void* mapped{};
      Check(vkMapMemory(device_, frame.gaussian_instances.memory, 0, bytes, 0,
                        &mapped),
            "map prepared Gaussian stream");
      auto* destination = static_cast<GaussianGpuInstance*>(mapped);
      for (const auto& range : changed_ranges) {
        std::memcpy(destination + range.first, packed.data() + range.first,
                    range.count * sizeof(GaussianGpuInstance));
      }
      vkUnmapMemory(device_, frame.gaussian_instances.memory);
    }
    frame.gaussian_instance_shadow = std::move(packed);
    frame.gaussian_instance_count =
        static_cast<std::uint32_t>(prepared_gaussians_.gaussians.size());
    frame.gaussian_preparation_generation = gaussian_preparation_generation_;
    frame_counters_.gaussian_upload_bytes += uploaded_bytes;
    frame_counters_.upload_bytes += uploaded_bytes;
  }

  void ValidateMaterialTextureBindings() const {
    for (std::size_t i = 0; i < material_records_.size(); ++i) {
      const auto& material = material_records_[i];
      if (material.base_color_texture &&
          (material.base_color_texture->texture_index >=
               texture_records_.size() ||
           material.base_color_texture->sampler_index >=
               sampler_records_.size())) {
        throw RendererError(RendererErrorCode::InvalidRequest,
                            "prepare material descriptors",
                            "material texture binding index is invalid");
      }
    }
  }

  void PrepareMaterialDescriptors(
      FrameContext& frame, const extraction::FrameSnapshot& snapshot) {
    ValidateMaterialTextureBindings();
    if (bindless_texture_table_) {
      PrepareBindlessMaterialDescriptors(frame, snapshot);
      return;
    }
    frame.material_descriptor_sets.clear();
    if (material_records_.empty()) {
      return;
    }
    EnsureFallbackTextureAndSampler();
    const auto material_count =
        static_cast<std::uint32_t>(material_records_.size());
    if (frame.descriptor_pool == VK_NULL_HANDLE ||
        frame.descriptor_capacity < material_count) {
      if (frame.descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, frame.descriptor_pool, nullptr);
        frame.descriptor_pool = VK_NULL_HANDLE;
        frame.descriptor_capacity = 0;
      }
      std::uint32_t maximum_sampled_images{};
      std::uint32_t maximum_samplers{};
      for (const auto& [key, artifact] : generated_material_artifacts_) {
        (void)key;
        std::uint32_t artifact_images{};
        std::uint32_t artifact_samplers{};
        for (const auto& resource : artifact.resource_bindings) {
          if (resource.type == MaterialValueType::Texture2D ||
              resource.type == MaterialValueType::CombinedTextureSampler) {
            artifact_images += resource.array_size;
          }
          if (resource.type == MaterialValueType::Sampler ||
              resource.type == MaterialValueType::CombinedTextureSampler) {
            artifact_samplers += resource.array_size;
          }
        }
        maximum_sampled_images =
            std::max(maximum_sampled_images, artifact_images);
        maximum_samplers = std::max(maximum_samplers, artifact_samplers);
      }
      std::vector<VkDescriptorPoolSize> sizes{
          {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, material_count},
          {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, material_count * 2U},
      };
      if (maximum_sampled_images != 0U) {
        sizes.push_back({VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                         maximum_sampled_images * material_count});
      }
      if (maximum_samplers != 0U) {
        sizes.push_back({VK_DESCRIPTOR_TYPE_SAMPLER,
                         maximum_samplers * material_count});
      }
      VkDescriptorPoolCreateInfo info{
          VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
      info.maxSets = material_count;
      info.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
      info.pPoolSizes = sizes.data();
      Check(vkCreateDescriptorPool(device_, &info, nullptr,
                                   &frame.descriptor_pool),
            "create material descriptor pool");
      ++frame_counters_.descriptor_pool_creation_count;
      frame.descriptor_capacity = material_count;
    } else {
      Check(vkResetDescriptorPool(device_, frame.descriptor_pool, 0),
            "reset material descriptor pool");
    }

    const auto uniform_stride =
        AlignUp(sizeof(MaterialUniforms), uniform_buffer_alignment_);
    const auto uniform_bytes = uniform_stride * material_count;
    if (frame.material_uniforms.handle == VK_NULL_HANDLE ||
        frame.material_uniforms.size < uniform_bytes) {
      DestroyBuffer(frame.material_uniforms);
      frame.material_uniforms = CreateBuffer(
          uniform_bytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    void* mapped{};
    Check(vkMapMemory(device_, frame.material_uniforms.memory, 0,
                      uniform_bytes, 0, &mapped),
          "map material uniform buffer");
    const auto lighting = ExtractDirectionalLighting(snapshot);
    for (std::uint32_t i = 0; i < material_count; ++i) {
      const auto& material = material_records_[i];
      const auto uniforms = MakeMaterialUniforms(material, lighting);
      std::memcpy(static_cast<std::byte*>(mapped) + uniform_stride * i,
                  &uniforms, sizeof(uniforms));
    }
    vkUnmapMemory(device_, frame.material_uniforms.memory);

    frame.generated_parameter_offsets.assign(material_count, 0);
    VkDeviceSize generated_uniform_bytes{};
    for (std::uint32_t i = 0; i < material_count; ++i) {
      if (const auto* artifact = selected_material_artifacts_[i]) {
        generated_uniform_bytes =
            AlignUp(generated_uniform_bytes, uniform_buffer_alignment_);
        frame.generated_parameter_offsets[i] = generated_uniform_bytes;
        generated_uniform_bytes += artifact->parameter_buffer_size;
      }
    }
    if (generated_uniform_bytes != 0U) {
      if (frame.generated_parameter_uniforms.handle == VK_NULL_HANDLE ||
          frame.generated_parameter_uniforms.size <
              generated_uniform_bytes) {
        DestroyBuffer(frame.generated_parameter_uniforms);
        frame.generated_parameter_uniforms = CreateBuffer(
            generated_uniform_bytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      }
      void* generated_mapped{};
      Check(vkMapMemory(device_, frame.generated_parameter_uniforms.memory, 0,
                        generated_uniform_bytes, 0, &generated_mapped),
            "map generated material uniform buffer");
      for (std::uint32_t i = 0; i < material_count; ++i) {
        if (const auto* artifact = selected_material_artifacts_[i]) {
          PackGeneratedMaterialParameters(
              static_cast<std::byte*>(generated_mapped) +
                  frame.generated_parameter_offsets[i],
              material_records_[i], *artifact);
        }
      }
      vkUnmapMemory(device_, frame.generated_parameter_uniforms.memory);
    }

    std::vector<VkDescriptorSetLayout> layouts(material_count);
    for (std::uint32_t i = 0; i < material_count; ++i) {
      const auto* artifact = selected_material_artifacts_[i];
      layouts[i] =
          artifact == nullptr
              ? descriptor_set_layout_
              : generated_descriptor_set_layouts_.at(artifact->module_key);
    }
    frame.material_descriptor_sets.resize(material_count);
    VkDescriptorSetAllocateInfo allocate{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = frame.descriptor_pool;
    allocate.descriptorSetCount = material_count;
    allocate.pSetLayouts = layouts.data();
    Check(vkAllocateDescriptorSets(device_, &allocate,
                                   frame.material_descriptor_sets.data()),
          "allocate material descriptor sets");
    frame_counters_.descriptor_allocation_count += material_count;

    std::size_t maximum_image_infos = material_count;
    for (const auto* artifact : selected_material_artifacts_) {
      if (artifact == nullptr) {
        continue;
      }
      for (const auto& resource : artifact->resource_bindings) {
        if (resource.type == MaterialValueType::Texture2D ||
            resource.type == MaterialValueType::CombinedTextureSampler) {
          maximum_image_infos += resource.array_size;
        }
        if (resource.type == MaterialValueType::Sampler ||
            resource.type == MaterialValueType::CombinedTextureSampler) {
          maximum_image_infos += resource.array_size;
        }
      }
    }
    std::vector<VkDescriptorImageInfo> image_infos;
    image_infos.reserve(maximum_image_infos);
    std::vector<VkDescriptorBufferInfo> buffer_infos(material_count);
    std::vector<VkDescriptorBufferInfo> generated_buffer_infos(material_count);
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(material_count * 2U + maximum_image_infos);
    for (std::uint32_t i = 0; i < material_count; ++i) {
      buffer_infos[i] = {frame.material_uniforms.handle, uniform_stride * i,
                         sizeof(MaterialUniforms)};
      if (const auto* artifact = selected_material_artifacts_[i]) {
        generated_buffer_infos[i] = {
            frame.generated_parameter_uniforms.handle,
            frame.generated_parameter_offsets[i],
            artifact->parameter_buffer_size};
        VkWriteDescriptorSet parameter_write{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        parameter_write.dstSet = frame.material_descriptor_sets[i];
        parameter_write.dstBinding = artifact->parameter_binding;
        parameter_write.descriptorCount = 1;
        parameter_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        parameter_write.pBufferInfo = &generated_buffer_infos[i];
        writes.push_back(parameter_write);

        for (const auto& resource : artifact->resource_bindings) {
          const auto state = std::find_if(
              material_records_[i].generated_resources.entries.begin(),
              material_records_[i].generated_resources.entries.end(),
              [&](const auto& entry) { return entry.name == resource.name; });
          if (state ==
                  material_records_[i].generated_resources.entries.end() ||
              state->type != resource.type ||
              state->values.size() != resource.array_size) {
            throw RendererError(
                RendererErrorCode::InvalidRequest,
                "prepare generated material descriptors",
                "generated resource state does not match the Vulkan artifact");
          }
          const auto write_resources =
              [&](VkDescriptorType descriptor_type, std::uint32_t binding) {
                const auto first_info = image_infos.size();
                for (const auto& value : state->values) {
                  VkDescriptorImageInfo info{};
                  if (descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
                    info.imageView =
                        texture_slots_.at(value.texture.value()).view;
                    info.imageLayout =
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                  } else {
                    info.sampler =
                        sampler_slots_.at(value.sampler.value()).sampler;
                  }
                  image_infos.push_back(info);
                }
                VkWriteDescriptorSet write{
                    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = frame.material_descriptor_sets[i];
                write.dstBinding = binding;
                write.descriptorCount = resource.array_size;
                write.descriptorType = descriptor_type;
                write.pImageInfo = image_infos.data() + first_info;
                writes.push_back(write);
              };
          if (resource.type == MaterialValueType::Texture2D ||
              resource.type == MaterialValueType::CombinedTextureSampler) {
            write_resources(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                            resource.texture_binding);
          }
          if (resource.type == MaterialValueType::Sampler ||
              resource.type == MaterialValueType::CombinedTextureSampler) {
            write_resources(VK_DESCRIPTOR_TYPE_SAMPLER,
                            resource.sampler_binding);
          }
        }

        VkWriteDescriptorSet material_write{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        material_write.dstSet = frame.material_descriptor_sets[i];
        material_write.dstBinding = artifact->material_constants_binding;
        material_write.descriptorCount = 1;
        material_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        material_write.pBufferInfo = &buffer_infos[i];
        writes.push_back(material_write);
        continue;
      }
      auto image_view = FallbackTextureView();
      auto sampler = fallback_sampler_.sampler;
      const auto& material = material_records_[i];
      if (material.base_color_texture) {
        if (material.base_color_texture->texture_index >=
                texture_records_.size() ||
            material.base_color_texture->sampler_index >=
                sampler_records_.size()) {
          throw RendererError(RendererErrorCode::InvalidRequest,
                              "prepare material descriptors",
                              "material texture binding index is invalid");
        }
        const auto texture_handle =
            texture_records_[material.base_color_texture->texture_index]
                .texture;
        const auto sampler_handle =
            sampler_records_[material.base_color_texture->sampler_index]
                .sampler;
        image_view = texture_slots_.at(texture_handle).view;
        sampler = sampler_slots_.at(sampler_handle).sampler;
      }
      image_infos.push_back(
          {sampler, image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
      VkWriteDescriptorSet image_write{
          VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      image_write.dstSet = frame.material_descriptor_sets[i];
      image_write.dstBinding = shader_abi::kConventionalBaseColorTexture.binding;
      image_write.descriptorCount = 1;
      image_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      image_write.pImageInfo = &image_infos.back();
      writes.push_back(image_write);
      VkWriteDescriptorSet buffer_write{
          VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      buffer_write.dstSet = frame.material_descriptor_sets[i];
      buffer_write.dstBinding = shader_abi::kConventionalMaterialConstants.binding;
      buffer_write.descriptorCount = 1;
      buffer_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      buffer_write.pBufferInfo = &buffer_infos[i];
      writes.push_back(buffer_write);
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
    frame_counters_.descriptor_update_count += writes.size();
  }

  void ReleaseGaussianAttributes(GaussianAttributeSlot& slot) {
    ReleaseRange(gaussian_position_arena_, slot.positions);
    ReleaseRange(gaussian_covariance_arena_, slot.covariances);
    ReleaseRange(gaussian_opacity_arena_, slot.opacities);
    ReleaseRange(gaussian_radiance_arena_, slot.radiance);
  }

  // Retains source-space Gaussian attributes independently of the
  // camera-dependent prepared stream. Exact snapshot deltas visit only changed
  // resources; exact particle ranges visit only changed elements of the
  // affected attribute arrays when completion safety permits in-place reuse.
  void SyncGaussianAttributes(const extraction::FrameSnapshot& snapshot,
                              ResourceSyncMode mode) {
    if (mode == ResourceSyncMode::Unchanged) {
      return;
    }

    std::vector<const extraction::GaussianRecord*> records;
    if (mode == ResourceSyncMode::ReplaceAll) {
      for (auto& [handle, slot] : gaussian_attribute_slots_) {
        (void)handle;
        ReleaseGaussianAttributes(slot);
      }
      gaussian_attribute_slots_.clear();
      records.reserve(snapshot.gaussians.size());
      for (const auto& gaussian : snapshot.gaussians) {
        records.push_back(&gaussian);
      }
    } else if (mode == ResourceSyncMode::Full) {
      std::set<std::uint64_t> snapshot_handles;
      for (const auto& gaussian : snapshot.gaussians) {
        snapshot_handles.insert(gaussian.gaussian);
        records.push_back(&gaussian);
      }
      for (auto slot = gaussian_attribute_slots_.begin();
           slot != gaussian_attribute_slots_.end();) {
        if (!snapshot_handles.contains(slot->first)) {
          ReleaseGaussianAttributes(slot->second);
          slot = gaussian_attribute_slots_.erase(slot);
        } else {
          ++slot;
        }
      }
    } else {
      const auto& delta = snapshot.delta->gaussians;
      for (const auto handle : delta.removals) {
        const auto slot = gaussian_attribute_slots_.find(handle);
        if (slot == gaussian_attribute_slots_.end()) {
          continue;
        }
        ReleaseGaussianAttributes(slot->second);
        gaussian_attribute_slots_.erase(slot);
      }
      records.reserve(delta.upserts.size());
      for (std::size_t index = 0; index < delta.upserts.size(); ++index) {
        const auto* record =
            FindDeltaRecord(snapshot.gaussians, delta, index,
                            [](const extraction::GaussianRecord& gaussian) {
                              return gaussian.gaussian;
                            });
        if (!record) {
          throw RendererError(RendererErrorCode::InvalidRequest,
                              "synchronize Gaussian attributes",
                              "snapshot Gaussian delta has no matching record");
        }
        records.push_back(record);
      }
    }

    struct Upload {
      const extraction::GaussianRecord* record{};
      GaussianAttributeSlot* slot{};
      bool inserted{};
      bool positions{};
      bool covariances{};
      bool opacities{};
      bool radiance{};
      bool partial{};
      std::uint32_t coefficient_count{};
    };
    std::vector<Upload> uploads;
    VkDeviceSize staging_bytes{};
    const bool resident_ranges_reusable =
        latest_completed_value_ >= timeline_value_;

    const auto checked_bytes = [](std::size_t count, std::size_t element_size,
                                  std::string_view label) -> VkDeviceSize {
      if (count > std::numeric_limits<VkDeviceSize>::max() / element_size) {
        throw RendererError(RendererErrorCode::Unsupported,
                            "synchronize Gaussian attributes",
                            std::string(label) + " payload is too large");
      }
      return static_cast<VkDeviceSize>(count * element_size);
    };
    for (const auto* record : records) {
      if (!record->positions || !record->covariances || !record->opacities ||
          !record->spherical_harmonics_coefficients ||
          record->positions->size() != record->covariances->size() ||
          record->positions->size() != record->opacities->size() ||
          record->positions->size() >
              std::numeric_limits<std::uint32_t>::max() ||
          record->spherical_harmonics_degree > 3) {
        throw RendererError(RendererErrorCode::InvalidRequest,
                            "synchronize Gaussian attributes",
                            "Gaussian attribute payload is malformed");
      }
      const auto coefficient_count = (record->spherical_harmonics_degree + 1U) *
                                     (record->spherical_harmonics_degree + 1U);
      if (record->spherical_harmonics_coefficients->size() !=
          record->positions->size() * coefficient_count) {
        throw RendererError(RendererErrorCode::InvalidRequest,
                            "synchronize Gaussian attributes",
                            "Gaussian SH payload size is inconsistent");
      }
      for (const auto& range : record->particle_ranges) {
        if (range.first > record->positions->size() ||
            range.count > record->positions->size() - range.first) {
          throw RendererError(RendererErrorCode::InvalidRequest,
                              "synchronize Gaussian attributes",
                              "Gaussian changed range is out of bounds");
        }
      }

      const auto [entry, inserted] =
          gaussian_attribute_slots_.try_emplace(record->gaussian);
      auto& slot = entry->second;
      Upload upload{record, &slot, inserted};
      upload.coefficient_count = coefficient_count;
      upload.positions =
          inserted || slot.positions_revision != record->positions_revision;
      upload.covariances =
          inserted || slot.covariance_revision != record->covariance_revision;
      upload.opacities =
          inserted || slot.opacity_revision != record->opacity_revision;
      upload.radiance = inserted ||
                        slot.radiance_revision != record->radiance_revision ||
                        slot.coefficients_per_particle != coefficient_count;
      const bool attributes_changed = upload.positions || upload.covariances ||
                                      upload.opacities || upload.radiance;
      const bool record_changed =
          inserted || slot.record_revision != record->revision;
      if (!attributes_changed && !record_changed) {
        continue;
      }

      const auto position_bytes =
          checked_bytes(record->positions->size(), sizeof(Vec3), "position");
      const auto covariance_bytes = checked_bytes(
          record->covariances->size(), sizeof(Covariance3), "covariance");
      const auto opacity_bytes =
          checked_bytes(record->opacities->size(), sizeof(float), "opacity");
      const auto radiance_bytes =
          checked_bytes(record->spherical_harmonics_coefficients->size(),
                        sizeof(Vec3), "spherical-harmonic");
      upload.partial =
          attributes_changed && resident_ranges_reusable && !inserted &&
          slot.record_revision == record->particle_base_revision &&
          !record->particle_ranges.empty() &&
          slot.particle_count == record->positions->size() &&
          slot.coefficients_per_particle == coefficient_count &&
          (!upload.positions ||
           slot.positions.size == AlignUp(position_bytes, kArenaAlignment)) &&
          (!upload.covariances ||
           slot.covariances.size ==
               AlignUp(covariance_bytes, kArenaAlignment)) &&
          (!upload.opacities ||
           slot.opacities.size == AlignUp(opacity_bytes, kArenaAlignment)) &&
          (!upload.radiance ||
           slot.radiance.size == AlignUp(radiance_bytes, kArenaAlignment));

      const auto add_bytes = [&](std::size_t element_size,
                                 std::uint32_t multiplier = 1U) {
        if (upload.partial) {
          for (const auto& range : record->particle_ranges) {
            staging_bytes +=
                AlignUp(checked_bytes(static_cast<std::size_t>(range.count) *
                                          multiplier,
                                      element_size, "changed range"),
                        kArenaAlignment);
          }
        } else {
          staging_bytes +=
              AlignUp(checked_bytes(record->positions->size() * multiplier,
                                    element_size, "attribute"),
                      kArenaAlignment);
        }
      };
      if (upload.positions) add_bytes(sizeof(Vec3));
      if (upload.covariances) add_bytes(sizeof(Covariance3));
      if (upload.opacities) add_bytes(sizeof(float));
      if (upload.radiance) add_bytes(sizeof(Vec3), coefficient_count);
      uploads.push_back(upload);
    }

    if (uploads.empty()) {
      return;
    }
    StagingRing::Reservation reservation;
    if (staging_bytes != 0) {
      Buffer retired_staging;
      VkDeviceSize growth_bytes{};
      reservation = gaussian_staging_.Reserve(staging_bytes, retired_staging,
                                              growth_bytes);
      if (growth_bytes != 0) {
        ++frame_counters_.allocation_count;
        ++frame_counters_.buffer_allocation_count;
        frame_counters_.buffer_allocation_bytes += staging_bytes;
        Retire(retired_staging);
      }
    }

    VkDeviceSize cursor{};
    const auto stage = [&](const void* payload, VkDeviceSize bytes,
                           DeviceArena& arena, const BufferRange& range,
                           VkDeviceSize destination_offset) {
      if (bytes == 0) return;
      std::memcpy(reservation.mapped + cursor, payload,
                  static_cast<std::size_t>(bytes));
      pending_copies_.push_back({reservation.buffer,
                                 reservation.offset + cursor,
                                 arena.buffer(range.block),
                                 range.offset + destination_offset, bytes});
      cursor += AlignUp(bytes, kArenaAlignment);
      frame_counters_.upload_bytes += bytes;
      frame_counters_.gaussian_attribute_upload_bytes += bytes;
      ++frame_counters_.gaussian_attribute_copy_range_count;
    };
    for (auto& upload : uploads) {
      auto& slot = *upload.slot;
      const auto& record = *upload.record;
      const auto particle_count = record.positions->size();
      const auto upload_attribute =
          [&](bool changed, const auto& payload, std::size_t stride,
              std::uint32_t multiplier, DeviceArena& arena,
              BufferRange& range) {
            if (!changed) return;
            const auto total_bytes =
                checked_bytes(particle_count * multiplier, stride, "attribute");
            if (!upload.partial) {
              EnsureRange(arena, range, total_bytes);
              stage(payload.data(), total_bytes, arena, range, 0);
              return;
            }
            for (const auto& changed_range : record.particle_ranges) {
              const auto first =
                static_cast<std::size_t>(changed_range.first) * multiplier;
              const auto count =
                static_cast<std::size_t>(changed_range.count) * multiplier;
              stage(payload.data() + first,
                    checked_bytes(count, stride, "changed range"), arena, range,
                    checked_bytes(first, stride, "changed offset"));
            }
          };
      upload_attribute(upload.positions, *record.positions, sizeof(Vec3), 1,
                       gaussian_position_arena_, slot.positions);
      upload_attribute(upload.covariances, *record.covariances,
                       sizeof(Covariance3), 1, gaussian_covariance_arena_,
                       slot.covariances);
      upload_attribute(upload.opacities, *record.opacities, sizeof(float), 1,
                       gaussian_opacity_arena_, slot.opacities);
      upload_attribute(upload.radiance,
                       *record.spherical_harmonics_coefficients, sizeof(Vec3),
                       upload.coefficient_count, gaussian_radiance_arena_,
                       slot.radiance);

      slot.record_revision = record.revision;
      slot.positions_revision = record.positions_revision;
      slot.covariance_revision = record.covariance_revision;
      slot.opacity_revision = record.opacity_revision;
      slot.radiance_revision = record.radiance_revision;
      slot.policy_revision = record.policy_revision;
      slot.transform_revision = record.transform_revision;
      slot.visibility_revision = record.visibility_revision;
      slot.particle_count = static_cast<std::uint32_t>(particle_count);
      slot.coefficients_per_particle = upload.coefficient_count;
      ++slot.generation;
      if (slot.generation == 0) ++slot.generation;
      ++frame_counters_.gaussian_attribute_generation_count;
    }
  }
  // Reconciles GPU geometry residency with one immutable snapshot. Continuous
  // SceneExtractor revisions select only delta records; unrelated scene edits
  // and static frames do not walk the full geometry table. Revision gaps,
  // foreign sources, and manually constructed snapshots use the full path.
  void SyncGeometry(const extraction::FrameSnapshot& snapshot,
                    ResourceSyncMode mode) {
    if (mode == ResourceSyncMode::Unchanged) {
      frame_counters_.geometry_cache_hits += snapshot.geometries.size();
      ++frame_counters_.scene_cache_hits;
      return;
    }

    bool structural_change = false;
    std::vector<const extraction::GeometryRecord*> records;
    if (mode == ResourceSyncMode::ReplaceAll) {
      for (auto& [handle, slot] : geometry_slots_) {
        (void)handle;
        ReleaseRange(vertex_arena_, slot.vertices);
        ReleaseRange(index_arena_, slot.indices);
        ++frame_counters_.geometry_reconcile_count;
      }
      structural_change = !geometry_slots_.empty();
      geometry_slots_.clear();
      records.reserve(snapshot.geometries.size());
      for (const auto& geometry : snapshot.geometries) {
        records.push_back(&geometry);
      }
    } else if (mode == ResourceSyncMode::Full) {
      std::set<std::uint64_t> snapshot_handles;
      for (const auto& geometry : snapshot.geometries) {
        snapshot_handles.insert(geometry.mesh);
      }
      for (auto slot = geometry_slots_.begin(); slot != geometry_slots_.end();) {
        if (!snapshot_handles.contains(slot->first)) {
          ReleaseRange(vertex_arena_, slot->second.vertices);
          ReleaseRange(index_arena_, slot->second.indices);
          slot = geometry_slots_.erase(slot);
          structural_change = true;
          ++frame_counters_.geometry_reconcile_count;
        } else {
          ++slot;
        }
      }
      records.reserve(snapshot.geometries.size());
      for (const auto& geometry : snapshot.geometries) {
        records.push_back(&geometry);
      }
    } else {
      const auto& delta = snapshot.delta->geometries;
      for (const auto handle : delta.removals) {
        ++frame_counters_.geometry_reconcile_count;
        const auto slot = geometry_slots_.find(handle);
        if (slot == geometry_slots_.end()) {
          continue;
        }
        ReleaseRange(vertex_arena_, slot->second.vertices);
        ReleaseRange(index_arena_, slot->second.indices);
        geometry_slots_.erase(slot);
        structural_change = true;
      }
      records.reserve(delta.upserts.size());
      for (std::size_t i = 0; i < delta.upserts.size(); ++i) {
        const auto* record = FindDeltaRecord(
            snapshot.geometries, delta, i,
            [](const extraction::GeometryRecord& geometry) {
              return geometry.mesh;
            });
        if (!record) {
          throw RendererError(RendererErrorCode::InvalidRequest,
                              "synchronize geometry",
                              "snapshot geometry delta has no matching record");
        }
        records.push_back(record);
      }
      frame_counters_.geometry_cache_hits +=
          snapshot.geometries.size() - records.size();
    }
    frame_counters_.geometry_reconcile_count += records.size();

    struct Upload {
      const extraction::GeometryRecord* record{};
      GeometrySlot* slot{};
      bool vertices{};
      bool indices{};
      bool partial_vertices{};
      bool partial_indices{};
      VkDeviceSize vertex_bytes{};
      VkDeviceSize index_bytes{};
    };
    std::vector<Upload> uploads;
    VkDeviceSize staging_bytes{};
    // Partial copies overwrite the resident range, so they are safe only after
    // every submission that could reference that generation has completed.
    const bool resident_ranges_reusable =
        latest_completed_value_ >= timeline_value_;
    for (const auto* geometry_record : records) {
      const auto& geometry = *geometry_record;
      const auto [entry, inserted] = geometry_slots_.try_emplace(geometry.mesh);
      auto& slot = entry->second;
      Upload upload{&geometry, &slot};
      upload.vertices =
          inserted || slot.vertex_revision != geometry.vertex_revision;
      upload.indices =
          inserted || slot.topology_revision != geometry.index_revision;
      if (!upload.vertices && !upload.indices) {
        ++frame_counters_.geometry_cache_hits;
        continue;
      }
      ++frame_counters_.geometry_cache_misses;
      upload.vertex_bytes = static_cast<VkDeviceSize>(
          geometry.vertices->size() * sizeof(extraction::DrawVertex));
      upload.index_bytes = static_cast<VkDeviceSize>(
          geometry.indices->size() * sizeof(std::uint32_t));
      if (upload.vertices) {
        upload.partial_vertices =
            resident_ranges_reusable && !inserted &&
            slot.vertex_revision == geometry.vertex_base_revision &&
            !geometry.vertex_ranges.empty() && slot.vertices.valid() &&
            slot.vertices.size ==
                AlignUp(upload.vertex_bytes, kArenaAlignment);
        if (upload.partial_vertices) {
          ++frame_counters_.geometry_range_reuse_count;
          for (const auto& range : geometry.vertex_ranges) {
            staging_bytes += AlignUp(
                static_cast<VkDeviceSize>(range.count) *
                    sizeof(extraction::DrawVertex),
                kArenaAlignment);
          }
        } else {
          staging_bytes += AlignUp(upload.vertex_bytes, kArenaAlignment);
        }
      }
      if (upload.indices) {
        upload.partial_indices =
            resident_ranges_reusable && !inserted &&
            slot.topology_revision == geometry.index_base_revision &&
            !geometry.index_ranges.empty() && slot.indices.valid() &&
            slot.indices.size == AlignUp(upload.index_bytes, kArenaAlignment);
        if (upload.partial_indices) {
          ++frame_counters_.geometry_range_reuse_count;
          for (const auto& range : geometry.index_ranges) {
            staging_bytes += AlignUp(
                static_cast<VkDeviceSize>(range.count) *
                    sizeof(std::uint32_t),
                kArenaAlignment);
          }
        } else {
          staging_bytes += AlignUp(upload.index_bytes, kArenaAlignment);
        }
      }
      uploads.push_back(upload);
    }

    if (uploads.empty() && !structural_change) {
      ++frame_counters_.scene_cache_hits;
      return;
    }
    ++frame_counters_.scene_cache_misses;
    if (uploads.empty()) {
      return;
    }

    StagingRing::Reservation reservation;
    if (staging_bytes != 0) {
      Buffer retired_staging;
      VkDeviceSize staging_growth_bytes{};
      reservation =
          staging_.Reserve(staging_bytes, retired_staging,
                           staging_growth_bytes);
      frame_counters_.upload_ring_reserved_bytes += staging_bytes;
      if (staging_growth_bytes != 0) {
        ++frame_counters_.allocation_count;
        ++frame_counters_.buffer_allocation_count;
        // Preserve the v3 allocation counter's requested-payload convention;
        // upload_ring_growth_bytes reports the physical ring capacity added.
        frame_counters_.buffer_allocation_bytes += staging_bytes;
        ++frame_counters_.upload_ring_growth_count;
        frame_counters_.upload_ring_growth_bytes += staging_growth_bytes;
        Retire(retired_staging);
      }
    }

    VkDeviceSize cursor{};
    auto stage = [&](const void* payload, VkDeviceSize bytes,
                     DeviceArena& arena, const BufferRange& range,
                     std::uint64_t& resource_upload_bytes,
                     VkDeviceSize destination_offset = 0) {
      if (bytes == 0) {
        return;
      }
      std::memcpy(reservation.mapped + cursor, payload,
                  static_cast<std::size_t>(bytes));
      pending_copies_.push_back({reservation.buffer,
                                 reservation.offset + cursor,
                                 arena.buffer(range.block), range.offset,
                                 bytes});
      pending_copies_.back().destination_offset += destination_offset;
      cursor += AlignUp(bytes, kArenaAlignment);
      frame_counters_.upload_bytes += bytes;
      resource_upload_bytes += bytes;
    };
    for (auto& upload : uploads) {
      auto& slot = *upload.slot;
      if (upload.vertices) {
        if (upload.partial_vertices) {
          for (const auto& range : upload.record->vertex_ranges) {
            const auto byte_offset = static_cast<VkDeviceSize>(range.first) *
                                     sizeof(extraction::DrawVertex);
            const auto bytes = static_cast<VkDeviceSize>(range.count) *
                               sizeof(extraction::DrawVertex);
            stage(upload.record->vertices->data() + range.first, bytes,
                  vertex_arena_, slot.vertices,
                  frame_counters_.vertex_upload_bytes, byte_offset);
          }
        } else {
          EnsureRange(vertex_arena_, slot.vertices, upload.vertex_bytes);
          stage(upload.record->vertices->data(), upload.vertex_bytes,
                vertex_arena_, slot.vertices,
                frame_counters_.vertex_upload_bytes);
        }
        slot.vertex_revision = upload.record->vertex_revision;
      }
      if (upload.indices) {
        if (upload.partial_indices) {
          for (const auto& range : upload.record->index_ranges) {
            const auto byte_offset = static_cast<VkDeviceSize>(range.first) *
                                     sizeof(std::uint32_t);
            const auto bytes = static_cast<VkDeviceSize>(range.count) *
                               sizeof(std::uint32_t);
            stage(upload.record->indices->data() + range.first, bytes,
                  index_arena_, slot.indices,
                  frame_counters_.index_upload_bytes, byte_offset);
          }
        } else {
          EnsureRange(index_arena_, slot.indices, upload.index_bytes);
          stage(upload.record->indices->data(), upload.index_bytes,
                index_arena_, slot.indices,
                frame_counters_.index_upload_bytes);
        }
        slot.index_count =
            static_cast<std::uint32_t>(upload.record->indices->size());
        slot.topology_revision = upload.record->index_revision;
      }
    }
  }

  void EnsureRange(DeviceArena& arena, BufferRange& range,
                   VkDeviceSize bytes) {
    if (bytes == 0) {
      ReleaseRange(arena, range);
      return;
    }
    const auto aligned = AlignUp(bytes, kArenaAlignment);
    if (range.valid() && range.size == aligned &&
        latest_completed_value_ >= timeline_value_) {
      // Reuse in place only when every prior submission has completed. If an
      // older command buffer can still read this range, allocate a new range
      // and retire the old generation at its last possible completion value.
      ++frame_counters_.geometry_range_reuse_count;
      return;
    }
    ReleaseRange(arena, range);
    const auto allocation = arena.Allocate(bytes);
    if (allocation.created_block) {
      ++frame_counters_.allocation_count;
      ++frame_counters_.buffer_allocation_count;
      frame_counters_.buffer_allocation_bytes += allocation.block_bytes;
      ++frame_counters_.geometry_arena_growth_count;
      frame_counters_.geometry_arena_growth_bytes += allocation.block_bytes;
    }
    ++frame_counters_.buffer_suballocation_count;
    range = allocation.range;
  }

  void ReleaseRange(DeviceArena& arena, BufferRange& range) {
    if (!range.valid()) {
      return;
    }
    ++frame_counters_.buffer_range_release_count;
    retired_ranges_.push_back({&arena, range, timeline_value_});
    range = {};
  }

  void RecordUploads(VkCommandBuffer command, bool release_to_graphics) {
    if (pending_copies_.empty() && pending_image_copies_.empty()) {
      return;
    }
    for (const auto& copy : pending_copies_) {
      const VkBufferCopy region{copy.source_offset, copy.destination_offset,
                                copy.size};
      vkCmdCopyBuffer(command, copy.source, copy.destination, 1, &region);
    }
    if (!pending_copies_.empty() && !release_to_graphics) {
      VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask =
          VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
          VK_ACCESS_SHADER_READ_BIT;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                               VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
    pending_copies_.clear();

    if (!pending_image_copies_.empty()) {
      std::vector<VkImageMemoryBarrier> to_transfer;
      to_transfer.reserve(pending_image_copies_.size());
      for (const auto& copy : pending_image_copies_) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = copy.destination;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_transfer.push_back(barrier);
      }
      vkCmdPipelineBarrier(
          command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
          static_cast<std::uint32_t>(to_transfer.size()), to_transfer.data());
      for (const auto& copy : pending_image_copies_) {
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {copy.width, copy.height, 1};
        vkCmdCopyBufferToImage(command, copy.source, copy.destination,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);
      }
      std::vector<VkImageMemoryBarrier> to_shader;
      to_shader.reserve(pending_image_copies_.size());
      for (const auto& copy : pending_image_copies_) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = release_to_graphics
                                          ? transfer_queue_family_
                                          : VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = release_to_graphics
                                          ? queue_family_
                                          : VK_QUEUE_FAMILY_IGNORED;
        barrier.image = copy.destination;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask =
            release_to_graphics ? 0 : VK_ACCESS_SHADER_READ_BIT;
        to_shader.push_back(barrier);
        if (release_to_graphics) {
          pending_graphics_acquire_images_.push_back(copy.destination);
          ++frame_counters_.queue_ownership_transfer_count;
        }
      }
      vkCmdPipelineBarrier(
          command, VK_PIPELINE_STAGE_TRANSFER_BIT,
          release_to_graphics ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
                              : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          0, 0, nullptr, 0, nullptr,
          static_cast<std::uint32_t>(to_shader.size()), to_shader.data());
      pending_image_copies_.clear();
    }
  }

  void RecordUploadAcquires(VkCommandBuffer command) {
    if (pending_graphics_acquire_images_.empty()) {
      return;
    }
    std::vector<VkImageMemoryBarrier> barriers;
    barriers.reserve(pending_graphics_acquire_images_.size());
    for (const auto image : pending_graphics_acquire_images_) {
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      barrier.srcQueueFamilyIndex = transfer_queue_family_;
      barrier.dstQueueFamilyIndex = queue_family_;
      barrier.image = image;
      barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      barrier.subresourceRange.levelCount = 1;
      barrier.subresourceRange.layerCount = 1;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      barriers.push_back(barrier);
    }
    vkCmdPipelineBarrier(
        command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
        static_cast<std::uint32_t>(barriers.size()), barriers.data());
    pending_graphics_acquire_images_.clear();
  }

  void Retire(Buffer& buffer) {
    if (buffer.handle != VK_NULL_HANDLE) {
      deferred_.push_back({buffer, timeline_value_});
      buffer = {};
    }
  }

  void CollectDeferred(std::uint64_t completed,
                       bool write_bindless_descriptors = true) {
    auto iterator = deferred_.begin();
    while (iterator != deferred_.end()) {
      if (iterator->retire_value <= completed) {
        DestroyBuffer(iterator->buffer);
        iterator = deferred_.erase(iterator);
      } else {
        ++iterator;
      }
    }
    auto range = retired_ranges_.begin();
    while (range != retired_ranges_.end()) {
      if (range->retire_value <= completed) {
        range->arena->Release(range->range);
        if (range->arena == &gaussian_position_arena_ ||
            range->arena == &gaussian_covariance_arena_ ||
            range->arena == &gaussian_opacity_arena_ ||
            range->arena == &gaussian_radiance_arena_) {
          ++statistics_.gaussian_attribute_range_retirements;
        } else {
          ++statistics_.geometry_range_retirements;
        }
        range = retired_ranges_.erase(range);
      } else {
        ++range;
      }
    }
    if (bindless_texture_table_) {
      CollectCompletedBindlessSlots(completed);
      if (write_bindless_descriptors) {
        WriteBindlessDescriptors(
            bindless_texture_table_->ConsumeDirtySlots(),
            bindless_sampler_table_->ConsumeDirtySlots());
      }
    }
    auto texture = retired_textures_.begin();
    while (texture != retired_textures_.end()) {
      if (texture->retire_value <= completed) {
        DestroyTexture(texture->texture);
        texture = retired_textures_.erase(texture);
      } else {
        ++texture;
      }
    }
    auto sampler = retired_samplers_.begin();
    while (sampler != retired_samplers_.end()) {
      if (sampler->retire_value <= completed) {
        vkDestroySampler(device_, sampler->sampler, nullptr);
        sampler = retired_samplers_.erase(sampler);
      } else {
        ++sampler;
      }
    }
  }

  void ValidateExtent(std::uint32_t width, std::uint32_t height) const {
    if (width == 0 || height == 0 ||
        width > capabilities_.max_image_dimension_2d ||
        height > capabilities_.max_image_dimension_2d) {
      throw RendererError(RendererErrorCode::Unsupported,
                          "validate render request",
                          "offscreen extent is unsupported");
    }
  }

  static bool IsSupportedAov(Aov aov) noexcept {
    return aov == Aov::Color || aov == Aov::Depth ||
           aov == Aov::PrimId || aov == Aov::InstanceId;
  }

  void ValidateRequest(const RenderRequest& request) const {
    if (!request.snapshot) {
      throw RendererError(RendererErrorCode::InvalidRequest,
                          "validate render request", "snapshot is null");
    }
    switch (request.gpu_driven_indexed.mode) {
      case GpuDrivenIndexedMode::Disabled:
      case GpuDrivenIndexedMode::Prefer:
      case GpuDrivenIndexedMode::Require:
        break;
      default:
        throw RendererError(RendererErrorCode::InvalidRequest,
                            "validate render request",
                            "GPU-driven indexed mode is invalid");
    }
    ValidateExtent(request.width, request.height);
    if (!std::isfinite(request.clear_color.x) ||
        !std::isfinite(request.clear_color.y) ||
        !std::isfinite(request.clear_color.z) ||
        !std::isfinite(request.clear_color.w)) {
      throw RendererError(RendererErrorCode::InvalidRequest,
                          "validate render request",
                          "clear color components must be finite");
    }
    if (request.shaders.vertex.empty() || request.shaders.fragment.empty()) {
      throw RendererError(RendererErrorCode::InvalidRequest,
                          "validate render request",
                          "vertex and fragment shader paths are required");
    }
    if (request.shaders.environment.empty()) {
      throw RendererError(RendererErrorCode::InvalidRequest,
                          "validate render request",
                          "environment HDR path is required");
    }
    if (bindless_texture_table_ &&
        (request.shaders.bindless_vertex.empty() ||
         request.shaders.bindless_fragment.empty())) {
      throw RendererError(
          RendererErrorCode::InvalidRequest, "validate render request",
          "bindless vertex and fragment shader paths are required");
    }
    if (request.present && surface_ == VK_NULL_HANDLE) {
      throw RendererError(RendererErrorCode::InvalidRequest,
                          "validate render request",
                          "presentation was requested without a presentation target");
    }
    if (request.products.empty()) {
      throw RendererError(RendererErrorCode::InvalidRequest,
                          "validate render request",
                          "at least one render product is required");
    }
    std::vector<Aov> seen;
    for (const auto& product : request.products) {
      if (!IsSupportedAov(product.aov)) {
        throw RendererError(RendererErrorCode::Unsupported,
                            "validate render request",
                            "AOV " + std::string(AovName(product.aov)) +
                                " is unsupported");
      }
      if (HasAov(seen, product.aov)) {
        throw RendererError(RendererErrorCode::InvalidRequest,
                            "validate render request",
                            "duplicate AOV " +
                                std::string(AovName(product.aov)));
      }
      seen.push_back(product.aov);
    }
    if (request.present && !HasAov(seen, Aov::Color)) {
      throw RendererError(RendererErrorCode::InvalidRequest,
                          "validate render request",
                          "presentation requires the color AOV");
    }
  }

  static std::vector<Aov> RenderedAovs(const RenderRequest& request) {
    std::vector<Aov> result;
    result.reserve(request.products.size());
    for (const auto& product : request.products) {
      result.push_back(product.aov);
    }
    return result;
  }

  static std::vector<Aov> CpuReadbackAovs(const RenderRequest& request) {
    std::vector<Aov> result;
    for (const auto& product : request.products) {
      if (product.cpu_readback) {
        result.push_back(product.aov);
      }
    }
    return result;
  }

  FrameContext& AcquireFrame(std::uint32_t width, std::uint32_t height,
                             const ShaderPaths& shaders,
                             const std::vector<Aov>& cpu_readback_aovs) {
    auto reusable = [&](FrameContext& frame) {
      return !frame.outstanding && frame.exported_aov_mask == 0 &&
             frame.target.width == width &&
             frame.target.height == height &&
             frame.target.shaders == shaders &&
             frame.target.cpu_readback_aovs == cpu_readback_aovs;
    };
    auto found = std::find_if(frames_.begin(), frames_.end(), reusable);
    if (found == frames_.end()) {
      found = std::find_if(frames_.begin(), frames_.end(),
                           [](const FrameContext& frame) {
                             return !frame.outstanding &&
                                    frame.exported_aov_mask == 0;
                           });
    }
    if (found == frames_.end()) {
      throw RendererError(
          RendererErrorCode::ResourceBusy, "acquire frame context",
          "all frame contexts have unresolved completion tokens");
    }
    if (timeline_semaphore_ == VK_NULL_HANDLE &&
        found->completion_value != 0) {
      Check(vkResetFences(device_, 1, &found->fence), "reset frame fence");
    }
    return *found;
  }

  FrameContext& FindFrame(std::uint64_t completion) {
    const auto found = std::find_if(
        frames_.begin(), frames_.end(), [&](const FrameContext& frame) {
          return frame.outstanding && frame.completion_value == completion;
        });
    if (found == frames_.end()) {
      throw RendererError(RendererErrorCode::InvalidToken,
                          "resolve completion token",
                          "token is unknown or already resolved");
    }
    return *found;
  }

  const FrameContext& FindFrame(std::uint64_t completion) const {
    const auto found = std::find_if(
        frames_.begin(), frames_.end(), [&](const FrameContext& frame) {
          return frame.outstanding && frame.completion_value == completion;
        });
    if (found == frames_.end()) {
      throw RendererError(RendererErrorCode::InvalidToken,
                          "query completion token",
                          "token is unknown or already resolved");
    }
    return *found;
  }

  VkImage CreateImage(std::uint32_t width, std::uint32_t height, VkFormat format,
                      VkImageUsageFlags usage, VkDeviceMemory& memory) {
    ++frame_counters_.allocation_count;
    ++frame_counters_.image_allocation_count;
    VkImage image{};
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Check(vkCreateImage(device_, &image_info, nullptr, &image), "create image");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, image, &requirements);
    frame_counters_.image_allocation_bytes += requirements.size;
    const auto memory_type = FindMemoryTypeRaw(
        physical_device_, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    try {
      memory = memory_budget_.Allocate(requirements.size, memory_type,
                                       "allocate image memory");
      Check(vkBindImageMemory(device_, image, memory, 0), "bind image memory");
    } catch (...) {
      if (memory != VK_NULL_HANDLE) {
        memory_budget_.Free(memory);
        memory = VK_NULL_HANDLE;
      }
      vkDestroyImage(device_, image, nullptr);
      throw;
    }
    return image;
  }

  VkImageView CreateImageView(VkImage image, VkFormat format,
                              VkImageAspectFlags aspect) {
    VkImageView view{};
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange.aspectMask = aspect;
    info.subresourceRange.levelCount = 1;
    info.subresourceRange.layerCount = 1;
    Check(vkCreateImageView(device_, &info, nullptr, &view), "create image view");
    return view;
  }

  void EnsureTarget(RenderTarget& target, std::uint32_t width,
                    std::uint32_t height,
                    const ShaderPaths& shaders,
                    const std::vector<Aov>& cpu_readback_aovs) {
    if (target.width == width && target.height == height &&
        target.shaders == shaders &&
        target.cpu_readback_aovs == cpu_readback_aovs) {
      ++frame_counters_.pipeline_cache_hits;
      return;
    }
    ++frame_counters_.pipeline_cache_misses;
    DestroyTarget(target);
    CreateTarget(width, height, shaders, cpu_readback_aovs);
  }

  void CreateTarget(std::uint32_t width, std::uint32_t height,
                    const ShaderPaths& shaders,
                    const std::vector<Aov>& cpu_readback_aovs) {
    active_target_->width = width;
    active_target_->height = height;
    active_target_->shaders = shaders;
    active_target_->cpu_readback_aovs = cpu_readback_aovs;
    try {
      active_target_->color = CreateImage(
          width, height, kColorFormat,
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
              VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
          active_target_->color_memory);
      active_target_->color_view =
          CreateImageView(active_target_->color, kColorFormat,
                          VK_IMAGE_ASPECT_COLOR_BIT);
      active_target_->depth = CreateImage(
          width, height, kDepthFormat,
          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
              VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
          active_target_->depth_memory);
      active_target_->depth_view =
          CreateImageView(active_target_->depth, kDepthFormat,
                          VK_IMAGE_ASPECT_DEPTH_BIT);
      active_target_->prim_id = CreateImage(
          width, height, kIdFormat,
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
          active_target_->prim_id_memory);
      active_target_->prim_id_view = CreateImageView(
          active_target_->prim_id, kIdFormat, VK_IMAGE_ASPECT_COLOR_BIT);
      active_target_->instance_id = CreateImage(
          width, height, kIdFormat,
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
          active_target_->instance_id_memory);
      active_target_->instance_id_view = CreateImageView(
          active_target_->instance_id, kIdFormat, VK_IMAGE_ASPECT_COLOR_BIT);
      CreateRenderPass();
      const std::array<VkImageView, 4> views{
          active_target_->color_view, active_target_->depth_view,
          active_target_->prim_id_view,
          active_target_->instance_id_view};
      VkFramebufferCreateInfo framebuffer_info{
          VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      framebuffer_info.renderPass = active_target_->render_pass;
      framebuffer_info.attachmentCount = static_cast<std::uint32_t>(views.size());
      framebuffer_info.pAttachments = views.data();
      framebuffer_info.width = width;
      framebuffer_info.height = height;
      framebuffer_info.layers = 1;
      Check(vkCreateFramebuffer(device_, &framebuffer_info, nullptr,
                                &active_target_->framebuffer),
            "create framebuffer");
      const auto color_bytes = static_cast<VkDeviceSize>(width) * height * 4U;
      const auto depth_bytes = static_cast<VkDeviceSize>(width) * height *
                               sizeof(float);
      const auto create_readback = [&](Aov aov, Buffer& buffer,
                                       VkDeviceSize bytes) {
        if (HasAov(cpu_readback_aovs, aov)) {
          buffer = CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }
      };
      create_readback(Aov::Color, active_target_->color_readback, color_bytes);
      create_readback(Aov::Depth, active_target_->depth_readback, depth_bytes);
      create_readback(Aov::PrimId, active_target_->prim_id_readback,
                      depth_bytes);
      create_readback(Aov::InstanceId, active_target_->instance_id_readback,
                      depth_bytes);
      VkPushConstantRange push_range{};
      push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
                              VK_SHADER_STAGE_FRAGMENT_BIT;
      push_range.size = sizeof(PushConstants);
      VkPipelineLayoutCreateInfo layout_info{
          VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
      std::array<VkDescriptorSetLayout, 2> bindless_layouts{};
      bindless_layouts[shader_abi::kBindlessTextures.set] =
          bindless_descriptor_set_layout_;
      bindless_layouts[shader_abi::kBindlessMaterialConstants.set] =
          bindless_material_descriptor_set_layout_;
      if (bindless_texture_table_) {
        layout_info.setLayoutCount =
            static_cast<std::uint32_t>(bindless_layouts.size());
        layout_info.pSetLayouts = bindless_layouts.data();
      } else {
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &descriptor_set_layout_;
      }
      layout_info.pushConstantRangeCount = 1;
      layout_info.pPushConstantRanges = &push_range;
      Check(vkCreatePipelineLayout(device_, &layout_info, nullptr,
                                   &active_target_->pipeline_layout),
            "create material pipeline layout");
      for (const auto& [module_key, descriptor_layout] :
           generated_descriptor_set_layouts_) {
        VkPipelineLayoutCreateInfo generated_layout_info{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        generated_layout_info.setLayoutCount = 1;
        generated_layout_info.pSetLayouts = &descriptor_layout;
        generated_layout_info.pushConstantRangeCount = 1;
        generated_layout_info.pPushConstantRanges = &push_range;
        VkPipelineLayout generated_layout{};
        Check(vkCreatePipelineLayout(device_, &generated_layout_info, nullptr,
                                     &generated_layout),
              "create generated material pipeline layout");
        active_target_->generated_pipeline_layouts.emplace(module_key,
                                                            generated_layout);
      }
    } catch (...) {
      DestroyTarget(*active_target_);
      throw;
    }
  }

  void CreateRenderPass() {
    std::array<VkAttachmentDescription, 4> attachments{};
    attachments[0].format = kColorFormat;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    attachments[1].format = kDepthFormat;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    for (std::size_t i = 2; i < attachments.size(); ++i) {
      attachments[i] = attachments[0];
      attachments[i].format = kIdFormat;
    }
    const std::array<VkAttachmentReference, 3> color_references{{
        {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
    }};
    const VkAttachmentReference depth_reference{
        1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription mesh_subpass{};
    mesh_subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    mesh_subpass.colorAttachmentCount =
        static_cast<std::uint32_t>(color_references.size());
    mesh_subpass.pColorAttachments = color_references.data();
    mesh_subpass.pDepthStencilAttachment = &depth_reference;
    const VkAttachmentReference gaussian_color_reference{
        0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription gaussian_color_subpass{};
    gaussian_color_subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    gaussian_color_subpass.colorAttachmentCount = 1;
    gaussian_color_subpass.pColorAttachments = &gaussian_color_reference;
    gaussian_color_subpass.pDepthStencilAttachment = &depth_reference;
    const std::array<std::uint32_t, 2> gaussian_color_preserved{2, 3};
    gaussian_color_subpass.preserveAttachmentCount =
        static_cast<std::uint32_t>(gaussian_color_preserved.size());
    gaussian_color_subpass.pPreserveAttachments =
        gaussian_color_preserved.data();
    const std::array<VkAttachmentReference, 2> gaussian_id_references{{
        {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
    }};
    VkSubpassDescription gaussian_id_subpass{};
    gaussian_id_subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    gaussian_id_subpass.colorAttachmentCount =
        static_cast<std::uint32_t>(gaussian_id_references.size());
    gaussian_id_subpass.pColorAttachments = gaussian_id_references.data();
    gaussian_id_subpass.pDepthStencilAttachment = &depth_reference;
    const std::uint32_t gaussian_id_preserved = 0;
    gaussian_id_subpass.preserveAttachmentCount = 1;
    gaussian_id_subpass.pPreserveAttachments = &gaussian_id_preserved;
    const std::array<VkSubpassDescription, 3> subpasses{
        mesh_subpass, gaussian_color_subpass, gaussian_id_subpass};
    std::array<VkSubpassDependency, 6> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = 1;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    dependencies[2].srcSubpass = 1;
    dependencies[2].dstSubpass = 2;
    dependencies[2].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[2].dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[2].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[2].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    dependencies[3].srcSubpass = 0;
    dependencies[3].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[3].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[3].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[3].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[3].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    dependencies[4].srcSubpass = 1;
    dependencies[4].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[4].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[4].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[4].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[4].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    dependencies[5].srcSubpass = 2;
    dependencies[5].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[5].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[5].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[5].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[5].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    info.pAttachments = attachments.data();
    info.subpassCount = static_cast<std::uint32_t>(subpasses.size());
    info.pSubpasses = subpasses.data();
    info.dependencyCount = static_cast<std::uint32_t>(dependencies.size());
    info.pDependencies = dependencies.data();
    Check(vkCreateRenderPass(device_, &info, nullptr, &active_target_->render_pass),
          "create color/depth render pass");
  }

  VkShaderModule GetShaderModule(const std::filesystem::path& path) {
    const auto found = shader_modules_.find(path);
    if (found != shader_modules_.end()) {
      ++frame_counters_.shader_module_cache_hits;
      return found->second;
    }
    ++frame_counters_.shader_module_cache_misses;
    const auto code = ReadSpirv(path);
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = code.size() * sizeof(std::uint32_t);
    info.pCode = code.data();
    VkShaderModule module{};
    Check(vkCreateShaderModule(device_, &info, nullptr, &module),
          "create shader module");
    shader_modules_.emplace(path, module);
    return module;
  }

  VkPipeline CreatePipeline(
      const ShaderPaths& shaders, std::uint32_t variant_key,
      const GeneratedMaterialArtifact* generated_artifact = nullptr,
      bool gpu_driven_pipeline = false) {
    ++frame_counters_.pipeline_creation_count;
    const bool gpu_scene_pipeline =
        generated_artifact == nullptr &&
        (variant_key & kGpuScenePipelineFlag) != 0U;
    const auto gpu_scene_vertex =
        shaders.gpu_scene_vertex.empty()
            ? shaders.bindless_vertex.parent_path() /
                  "triangle.gpu-scene.vert.spv"
            : shaders.gpu_scene_vertex;
    const auto gpu_scene_fragment =
        shaders.gpu_scene_fragment.empty()
            ? shaders.bindless_fragment.parent_path() /
                  "triangle.gpu-scene.frag.spv"
            : shaders.gpu_scene_fragment;
    const auto gpu_driven_vertex =
        shaders.gpu_driven_vertex.empty()
            ? shaders.bindless_vertex.parent_path() /
                  "gpu-driven-forward.vert.spv"
            : shaders.gpu_driven_vertex;
    const auto gpu_driven_fragment =
        shaders.gpu_driven_fragment.empty()
            ? shaders.bindless_fragment.parent_path() /
                  "gpu-driven-forward.frag.spv"
            : shaders.gpu_driven_fragment;
    const auto vertex_shader = GetShaderModule(
        gpu_driven_pipeline
            ? gpu_driven_vertex
            : gpu_scene_pipeline
            ? gpu_scene_vertex
            : (bindless_texture_table_ ? shaders.bindless_vertex
                                       : shaders.vertex));
    const auto fragment_path =
        generated_artifact != nullptr
            ? generated_artifact->fragment
            : (gpu_driven_pipeline
                   ? gpu_driven_fragment
                   : gpu_scene_pipeline
                   ? gpu_scene_fragment
                   : (bindless_texture_table_ ? shaders.bindless_fragment
                                              : shaders.fragment));
    const auto fragment_shader = GetShaderModule(fragment_path);
    const auto* fragment_entry =
        generated_artifact != nullptr
            ? generated_artifact->fragment_entry_point.c_str()
            : "main";
    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_VERTEX_BIT, vertex_shader, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_FRAGMENT_BIT, fragment_shader, fragment_entry,
         nullptr}}};
    const VkVertexInputBindingDescription binding{
        0, sizeof(extraction::DrawVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array<VkVertexInputAttributeDescription, 4> attributes{{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT,
         static_cast<std::uint32_t>(offsetof(extraction::DrawVertex, position))},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT,
         static_cast<std::uint32_t>(offsetof(extraction::DrawVertex, normal))},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
         static_cast<std::uint32_t>(offsetof(extraction::DrawVertex, color))},
        {3, 0, VK_FORMAT_R32G32_SFLOAT,
         static_cast<std::uint32_t>(offsetof(extraction::DrawVertex, texcoord))},
    }};
    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount =
        static_cast<std::uint32_t>(attributes.size());
    vertex_input.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo input_assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport_state{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = (variant_key & kDoubleSidedFlag) != 0U
                          ? VK_CULL_MODE_NONE
                          : VK_CULL_MODE_BACK_BIT;
    raster.frontFace =
        (variant_key & kCounterClockwiseFrontFaceFlag) != 0U
            ? VK_FRONT_FACE_COUNTER_CLOCKWISE
            : VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                       VK_COLOR_COMPONENT_G_BIT |
                                       VK_COLOR_COMPONENT_B_BIT |
                                       VK_COLOR_COMPONENT_A_BIT;
    std::array<VkPipelineColorBlendAttachmentState, 3> blend_attachments{
        blend_attachment, blend_attachment, blend_attachment};
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount =
        static_cast<std::uint32_t>(blend_attachments.size());
    blend.pAttachments = blend_attachments.data();
    const std::array<VkDynamicState, 2> dynamic_states{
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount =
        static_cast<std::uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();
    VkGraphicsPipelineCreateInfo pipeline_info{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline_info.stageCount = static_cast<std::uint32_t>(stages.size());
    pipeline_info.pStages = stages.data();
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &raster;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pDepthStencilState = &depth;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.pDynamicState = &dynamic;
    pipeline_info.layout = generated_artifact != nullptr
                               ? active_target_->generated_pipeline_layouts.at(
                                     generated_artifact->module_key)
                               : active_target_->pipeline_layout;
    pipeline_info.renderPass = active_target_->render_pass;
    VkPipeline pipeline{};
    Check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1,
                                    &pipeline_info, nullptr, &pipeline),
          "create scene graphics pipeline");
    return pipeline;
  }

  VkPipeline EnsurePipeline(const ShaderPaths& shaders,
                            std::uint32_t variant_key) {
    const auto found = active_target_->pipelines.find(variant_key);
    if (found != active_target_->pipelines.end()) {
      return found->second;
    }
    const auto pipeline = CreatePipeline(shaders, variant_key);
    active_target_->pipelines.emplace(variant_key, pipeline);
    return pipeline;
  }

  VkPipeline EnsureGpuDrivenGraphicsPipeline(const ShaderPaths& shaders,
                                             std::uint32_t variant_key) {
    const auto found = active_target_->gpu_driven_pipelines.find(variant_key);
    if (found != active_target_->gpu_driven_pipelines.end()) {
      ++frame_counters_.pipeline_cache_hits;
      return found->second;
    }
    ++frame_counters_.pipeline_cache_misses;
    const auto pipeline = CreatePipeline(shaders, variant_key, nullptr, true);
    active_target_->gpu_driven_pipelines.emplace(variant_key, pipeline);
    return pipeline;
  }

  VkPipeline EnsureGaussianPipeline(const ShaderPaths& shaders,
                                    bool id_pass = false) {
    auto& cached = id_pass ? active_target_->gaussian_id_pipeline
                           : active_target_->gaussian_pipeline;
    if (cached != VK_NULL_HANDLE) {
      return cached;
    }
    const auto vertex_path = id_pass
        ? (shaders.gaussian_id_vertex.empty()
               ? shaders.vertex.parent_path() / "gaussian-id.vert.spv"
               : shaders.gaussian_id_vertex)
        : (shaders.gaussian_vertex.empty()
               ? shaders.vertex.parent_path() / "gaussian.vert.spv"
               : shaders.gaussian_vertex);
    const auto fragment_path = id_pass
        ? (shaders.gaussian_id_fragment.empty()
               ? shaders.fragment.parent_path() / "gaussian-id.frag.spv"
               : shaders.gaussian_id_fragment)
        : (shaders.gaussian_fragment.empty()
               ? shaders.fragment.parent_path() / "gaussian.frag.spv"
               : shaders.gaussian_fragment);
    ++frame_counters_.pipeline_creation_count;
    const auto vertex_shader = GetShaderModule(vertex_path);
    const auto fragment_shader = GetShaderModule(fragment_path);
    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_VERTEX_BIT, vertex_shader, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_FRAGMENT_BIT, fragment_shader, "main", nullptr},
    }};
    const std::array<VkVertexInputBindingDescription, 2> bindings{{
        {0, sizeof(Vec2), VK_VERTEX_INPUT_RATE_VERTEX},
        {1, sizeof(GaussianGpuInstance), VK_VERTEX_INPUT_RATE_INSTANCE},
    }};
    const std::array<VkVertexInputAttributeDescription, 9> attributes{{
        {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
        {1, 1, VK_FORMAT_R32G32_SFLOAT,
         static_cast<std::uint32_t>(
             offsetof(GaussianGpuInstance, center_pixels))},
        {2, 1, VK_FORMAT_R32G32B32_SFLOAT,
         static_cast<std::uint32_t>(
             offsetof(GaussianGpuInstance, inverse_conic))},
        {3, 1, VK_FORMAT_R32G32B32_SFLOAT,
         static_cast<std::uint32_t>(offsetof(GaussianGpuInstance, radiance))},
        {4, 1, VK_FORMAT_R32_SFLOAT,
         static_cast<std::uint32_t>(offsetof(GaussianGpuInstance, opacity))},
        {5, 1, VK_FORMAT_R32_SFLOAT,
         static_cast<std::uint32_t>(
             offsetof(GaussianGpuInstance, radius_pixels))},
        {6, 1, VK_FORMAT_R32_SFLOAT,
         static_cast<std::uint32_t>(offsetof(GaussianGpuInstance, depth))},
        {7, 1, VK_FORMAT_R32_UINT,
         static_cast<std::uint32_t>(
             offsetof(GaussianGpuInstance, resource_id))},
        {8, 1, VK_FORMAT_R32_UINT,
         static_cast<std::uint32_t>(
             offsetof(GaussianGpuInstance, particle_id))},
    }};
    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount =
        static_cast<std::uint32_t>(bindings.size());
    vertex_input.pVertexBindingDescriptions = bindings.data();
    vertex_input.vertexAttributeDescriptionCount = id_pass
        ? static_cast<std::uint32_t>(attributes.size())
        : static_cast<std::uint32_t>(attributes.size() - 2U);
    vertex_input.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo input_assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport_state{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_FALSE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = id_pass
        ? VK_COLOR_COMPONENT_R_BIT
        : VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (!id_pass) {
      attachment.blendEnable = VK_TRUE;
      attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
      attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      attachment.colorBlendOp = VK_BLEND_OP_ADD;
      attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    const std::array blend_attachments{attachment, attachment};
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = id_pass ? 2U : 1U;
    blend.pAttachments = blend_attachments.data();
    const std::array<VkDynamicState, 2> dynamic_states{
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount =
        static_cast<std::uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();
    VkGraphicsPipelineCreateInfo pipeline_info{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline_info.stageCount = static_cast<std::uint32_t>(stages.size());
    pipeline_info.pStages = stages.data();
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &raster;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pDepthStencilState = &depth;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.pDynamicState = &dynamic;
    pipeline_info.layout = active_target_->pipeline_layout;
    pipeline_info.renderPass = active_target_->render_pass;
    pipeline_info.subpass = id_pass ? 2U : 1U;
    Check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1,
                                    &pipeline_info, nullptr,
                                    &cached),
          "create Gaussian graphics pipeline");
    return cached;
  }

  VkPipeline EnsureGeneratedPipeline(
      const ShaderPaths& shaders, const GeneratedMaterialArtifact& artifact,
      std::uint32_t variant_key) {
    const auto key = std::pair{artifact.module_key, variant_key};
    const auto found = active_target_->generated_pipelines.find(key);
    if (found != active_target_->generated_pipelines.end()) {
      ++frame_counters_.pipeline_cache_hits;
      return found->second;
    }
    ++frame_counters_.pipeline_cache_misses;
    const auto pipeline = CreatePipeline(shaders, variant_key, &artifact);
    active_target_->generated_pipelines.emplace(key, pipeline);
    return pipeline;
  }

  void DestroyTarget(RenderTarget& target) noexcept {
    if (device_ == VK_NULL_HANDLE) {
      return;
    }
    DestroyBuffer(target.instance_id_readback);
    DestroyBuffer(target.prim_id_readback);
    DestroyBuffer(target.depth_readback);
    DestroyBuffer(target.color_readback);
    for (const auto& [key, pipeline] : target.pipelines) {
      (void)key;
      vkDestroyPipeline(device_, pipeline, nullptr);
    }
    for (const auto& [key, pipeline] : target.gpu_driven_pipelines) {
      (void)key;
      vkDestroyPipeline(device_, pipeline, nullptr);
    }
    target.gpu_driven_pipelines.clear();
    for (const auto& [key, pipeline] : target.generated_pipelines) {
      (void)key;
      vkDestroyPipeline(device_, pipeline, nullptr);
    }
    target.generated_pipelines.clear();
    if (target.gaussian_pipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, target.gaussian_pipeline, nullptr);
      target.gaussian_pipeline = VK_NULL_HANDLE;
    }
    if (target.gaussian_id_pipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, target.gaussian_id_pipeline, nullptr);
      target.gaussian_id_pipeline = VK_NULL_HANDLE;
    }
    for (const auto& [key, layout] : target.generated_pipeline_layouts) {
      (void)key;
      vkDestroyPipelineLayout(device_, layout, nullptr);
    }
    target.generated_pipeline_layouts.clear();
    if (target.pipeline_layout != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(device_, target.pipeline_layout, nullptr);
    }
    if (target.framebuffer != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(device_, target.framebuffer, nullptr);
    }
    if (target.render_pass != VK_NULL_HANDLE) {
      vkDestroyRenderPass(device_, target.render_pass, nullptr);
    }
    if (target.instance_id_view != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, target.instance_id_view, nullptr);
    }
    if (target.instance_id != VK_NULL_HANDLE) {
      vkDestroyImage(device_, target.instance_id, nullptr);
    }
    if (target.instance_id_memory != VK_NULL_HANDLE) {
      memory_budget_.Free(target.instance_id_memory);
    }
    if (target.prim_id_view != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, target.prim_id_view, nullptr);
    }
    if (target.prim_id != VK_NULL_HANDLE) {
      vkDestroyImage(device_, target.prim_id, nullptr);
    }
    if (target.prim_id_memory != VK_NULL_HANDLE) {
      memory_budget_.Free(target.prim_id_memory);
    }
    if (target.depth_view != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, target.depth_view, nullptr);
    }
    if (target.depth != VK_NULL_HANDLE) {
      vkDestroyImage(device_, target.depth, nullptr);
    }
    if (target.depth_memory != VK_NULL_HANDLE) {
      memory_budget_.Free(target.depth_memory);
    }
    if (target.color_view != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, target.color_view, nullptr);
    }
    if (target.color != VK_NULL_HANDLE) {
      vkDestroyImage(device_, target.color, nullptr);
    }
    if (target.color_memory != VK_NULL_HANDLE) {
      memory_budget_.Free(target.color_memory);
    }
    target = {};
  }

  void DestroySwapchain() noexcept {
    if (device_ != VK_NULL_HANDLE) {
      for (const auto framebuffer : swapchain_.overlay_framebuffers) {
        vkDestroyFramebuffer(device_, framebuffer, nullptr);
      }
      if (swapchain_.overlay_render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device_, swapchain_.overlay_render_pass, nullptr);
      }
      for (const auto view : swapchain_.image_views) {
        vkDestroyImageView(device_, view, nullptr);
      }
      for (const auto semaphore : swapchain_.render_finished) {
        vkDestroySemaphore(device_, semaphore, nullptr);
      }
    }
    if (device_ != VK_NULL_HANDLE && swapchain_.handle != VK_NULL_HANDLE) {
      vkDestroySwapchainKHR(device_, swapchain_.handle, nullptr);
    }
    swapchain_ = {};
  }

  PresentationOverlayContext MakePresentationOverlayContext(
      PresentationOverlayPhase phase,
      VkCommandBuffer command = VK_NULL_HANDLE) const noexcept {
    PresentationOverlayContext context;
    context.phase = phase;
    context.instance = EncodeHandle(instance_);
    context.physical_device = EncodeHandle(physical_device_);
    context.device = EncodeHandle(device_);
    context.queue = EncodeHandle(queue_);
    context.render_pass = EncodeHandle(swapchain_.overlay_render_pass);
    context.command_buffer = EncodeHandle(command);
    context.api_version = kMinimumVulkanApiVersion;
    context.queue_family = queue_family_;
    context.image_count =
        static_cast<std::uint32_t>(swapchain_.images.size());
    context.width = swapchain_.extent.width;
    context.height = swapchain_.extent.height;
    context.color_format = static_cast<std::uint32_t>(swapchain_.format);
    return context;
  }

  void InitializePresentationOverlay() {
    if (presentation_overlay_ == nullptr) {
      return;
    }
    presentation_overlay_(
        presentation_overlay_user_data_,
        MakePresentationOverlayContext(PresentationOverlayPhase::Initialize));
    presentation_overlay_initialized_ = true;
  }

  void ShutdownPresentationOverlay() noexcept {
    if (!presentation_overlay_initialized_ ||
        presentation_overlay_ == nullptr) {
      return;
    }
    try {
      presentation_overlay_(
          presentation_overlay_user_data_,
          MakePresentationOverlayContext(PresentationOverlayPhase::Shutdown));
    } catch (...) {
      // Destruction and swapchain retirement cannot surface host exceptions.
    }
    presentation_overlay_initialized_ = false;
  }

  VkSurfaceFormatKHR SelectSurfaceFormat() const {
    std::uint32_t count{};
    Check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_,
                                               &count, nullptr),
          "query presentation surface formats");
    if (count == 0) {
      throw RendererError(RendererErrorCode::Unsupported,
                          "create Vulkan swapchain",
                          "presentation surface exposes no formats");
    }
    std::vector<VkSurfaceFormatKHR> formats(count);
    Check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_,
                                               &count, formats.data()),
          "query presentation surface formats");
    const std::array<VkFormat, 4> preferred{
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB};
    for (const auto format : preferred) {
      const auto found = std::find_if(
          formats.begin(), formats.end(), [format](const auto& candidate) {
            return candidate.format == format &&
                   candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
          });
      if (found != formats.end()) {
        return *found;
      }
    }
    return formats.front();
  }

  VkPresentModeKHR SelectPresentMode() const {
    std::uint32_t count{};
    Check(vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_,
                                                    &count, nullptr),
          "query Vulkan present modes");
    std::vector<VkPresentModeKHR> modes(count);
    Check(vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_,
                                                    &count, modes.data()),
          "query Vulkan present modes");
    if (!presentation_vsync_) {
      for (const auto preferred : {VK_PRESENT_MODE_IMMEDIATE_KHR,
                                   VK_PRESENT_MODE_MAILBOX_KHR}) {
        if (std::find(modes.begin(), modes.end(), preferred) != modes.end()) {
          return preferred;
        }
      }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
  }

  void CreateSwapchain(std::uint32_t requested_width,
                       std::uint32_t requested_height) {
    VkSurfaceCapabilitiesKHR surface_capabilities{};
    Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
              physical_device_, surface_, &surface_capabilities),
          "query presentation surface capabilities");
    VkImageUsageFlags image_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (presentation_overlay_ != nullptr) {
      image_usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if ((surface_capabilities.supportedUsageFlags & image_usage) !=
        image_usage) {
      throw RendererError(
          RendererErrorCode::Unsupported, "create Vulkan swapchain",
          presentation_overlay_ == nullptr
              ? "presentation images do not support GPU transfer destinations"
              : "presentation images do not support transfer and overlay color attachment usage");
    }

    const auto surface_format = SelectSurfaceFormat();
    VkFormatProperties source_properties{};
    VkFormatProperties destination_properties{};
    vkGetPhysicalDeviceFormatProperties(physical_device_, kColorFormat,
                                        &source_properties);
    vkGetPhysicalDeviceFormatProperties(physical_device_,
                                        surface_format.format,
                                        &destination_properties);
    if ((source_properties.optimalTilingFeatures &
         VK_FORMAT_FEATURE_BLIT_SRC_BIT) == 0U ||
        (destination_properties.optimalTilingFeatures &
         VK_FORMAT_FEATURE_BLIT_DST_BIT) == 0U) {
      throw RendererError(RendererErrorCode::Unsupported,
                          "create Vulkan swapchain",
                          "offscreen-to-presentation GPU blit is unsupported");
    }

    VkExtent2D extent{};
    if (surface_capabilities.currentExtent.width !=
        std::numeric_limits<std::uint32_t>::max()) {
      extent = surface_capabilities.currentExtent;
    } else {
      extent.width = std::clamp(requested_width,
                                surface_capabilities.minImageExtent.width,
                                surface_capabilities.maxImageExtent.width);
      extent.height = std::clamp(requested_height,
                                 surface_capabilities.minImageExtent.height,
                                 surface_capabilities.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
      throw RendererError(RendererErrorCode::ResourceBusy,
                          "create Vulkan swapchain",
                          "presentation surface is minimized");
    }

    std::uint32_t image_count = surface_capabilities.minImageCount + 1U;
    if (surface_capabilities.maxImageCount != 0) {
      image_count = std::min(image_count,
                             surface_capabilities.maxImageCount);
    }
    VkCompositeAlphaFlagBitsKHR composite_alpha =
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((surface_capabilities.supportedCompositeAlpha & composite_alpha) == 0U) {
      constexpr std::array<VkCompositeAlphaFlagBitsKHR, 3> alternatives{
          VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
          VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
          VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
      const auto found = std::find_if(
          alternatives.begin(), alternatives.end(),
          [&](const auto value) {
            return (surface_capabilities.supportedCompositeAlpha & value) != 0U;
          });
      if (found == alternatives.end()) {
        throw RendererError(RendererErrorCode::Unsupported,
                            "create Vulkan swapchain",
                            "presentation surface exposes no composite alpha mode");
      }
      composite_alpha = *found;
    }

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface_;
    info.minImageCount = image_count;
    info.imageFormat = surface_format.format;
    info.imageColorSpace = surface_format.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = image_usage;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = surface_capabilities.currentTransform;
    info.compositeAlpha = composite_alpha;
    info.presentMode = SelectPresentMode();
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain_.handle;

    VkSwapchainKHR replacement{};
    Check(vkCreateSwapchainKHR(device_, &info, nullptr, &replacement),
          "create Vulkan swapchain");
    std::uint32_t actual_count{};
    Check(vkGetSwapchainImagesKHR(device_, replacement, &actual_count, nullptr),
          "query Vulkan swapchain images");
    std::vector<VkImage> images(actual_count);
    Check(vkGetSwapchainImagesKHR(device_, replacement, &actual_count,
                                  images.data()),
          "query Vulkan swapchain images");
    std::vector<VkImageView> image_views;
    std::vector<VkFramebuffer> overlay_framebuffers;
    VkRenderPass overlay_render_pass{};
    std::vector<VkSemaphore> render_finished(actual_count);
    try {
      image_views.resize(actual_count);
      for (std::uint32_t index = 0; index < actual_count; ++index) {
        VkImageViewCreateInfo view_info{
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = images[index];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = surface_format.format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        Check(vkCreateImageView(device_, &view_info, nullptr,
                                &image_views[index]),
              "create presentation image view");
      }
      if (presentation_overlay_ != nullptr) {
        VkAttachmentDescription attachment{};
        attachment.format = surface_format.format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference color_reference{
            0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_reference;
        VkRenderPassCreateInfo render_pass_info{
            VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        render_pass_info.attachmentCount = 1;
        render_pass_info.pAttachments = &attachment;
        render_pass_info.subpassCount = 1;
        render_pass_info.pSubpasses = &subpass;
        Check(vkCreateRenderPass(device_, &render_pass_info, nullptr,
                                 &overlay_render_pass),
              "create presentation overlay render pass");

        overlay_framebuffers.resize(actual_count);
        for (std::uint32_t index = 0; index < actual_count; ++index) {
          VkFramebufferCreateInfo framebuffer_info{
              VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
          framebuffer_info.renderPass = overlay_render_pass;
          framebuffer_info.attachmentCount = 1;
          framebuffer_info.pAttachments = &image_views[index];
          framebuffer_info.width = extent.width;
          framebuffer_info.height = extent.height;
          framebuffer_info.layers = 1;
          Check(vkCreateFramebuffer(device_, &framebuffer_info, nullptr,
                                    &overlay_framebuffers[index]),
                "create presentation overlay framebuffer");
        }
      }
      VkSemaphoreCreateInfo semaphore_info{
          VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      for (auto& semaphore : render_finished) {
        Check(vkCreateSemaphore(device_, &semaphore_info, nullptr, &semaphore),
              "create per-image presentation completion semaphore");
      }
    } catch (...) {
      for (const auto framebuffer : overlay_framebuffers) {
        if (framebuffer != VK_NULL_HANDLE) {
          vkDestroyFramebuffer(device_, framebuffer, nullptr);
        }
      }
      if (overlay_render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device_, overlay_render_pass, nullptr);
      }
      for (const auto view : image_views) {
        if (view != VK_NULL_HANDLE) {
          vkDestroyImageView(device_, view, nullptr);
        }
      }
      for (const auto semaphore : render_finished) {
        if (semaphore != VK_NULL_HANDLE) {
          vkDestroySemaphore(device_, semaphore, nullptr);
        }
      }
      vkDestroySwapchainKHR(device_, replacement, nullptr);
      throw;
    }

    const bool replacing = swapchain_.handle != VK_NULL_HANDLE;
    if (replacing) {
      ShutdownPresentationOverlay();
      for (const auto framebuffer : swapchain_.overlay_framebuffers) {
        vkDestroyFramebuffer(device_, framebuffer, nullptr);
      }
      if (swapchain_.overlay_render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device_, swapchain_.overlay_render_pass, nullptr);
      }
      for (const auto view : swapchain_.image_views) {
        vkDestroyImageView(device_, view, nullptr);
      }
      for (const auto semaphore : swapchain_.render_finished) {
        vkDestroySemaphore(device_, semaphore, nullptr);
      }
      vkDestroySwapchainKHR(device_, swapchain_.handle, nullptr);
      ++statistics_.swapchain_recreates;
    }
    swapchain_.handle = replacement;
    swapchain_.format = surface_format.format;
    swapchain_.color_space = surface_format.colorSpace;
    swapchain_.present_mode = info.presentMode;
    swapchain_.extent = extent;
    swapchain_.requested_width = requested_width;
    swapchain_.requested_height = requested_height;
    swapchain_.images = std::move(images);
    swapchain_.image_views = std::move(image_views);
    swapchain_.overlay_framebuffers = std::move(overlay_framebuffers);
    swapchain_.render_finished = std::move(render_finished);
    swapchain_.initialized.assign(actual_count, false);
    swapchain_.overlay_render_pass = overlay_render_pass;
    swapchain_.dirty = false;
    InitializePresentationOverlay();
  }

  void EnsureSwapchain(std::uint32_t width, std::uint32_t height) {
    if (swapchain_.handle != VK_NULL_HANDLE && !swapchain_.dirty &&
        swapchain_.requested_width == width &&
        swapchain_.requested_height == height) {
      return;
    }
    if (swapchain_.handle != VK_NULL_HANDLE) {
      Check(vkDeviceWaitIdle(device_), "wait before recreating Vulkan swapchain");
    }
    CreateSwapchain(width, height);
  }

  void PreparePresentation(FrameContext& frame, std::uint32_t width,
                           std::uint32_t height) {
    EnsureSwapchain(width, height);
    auto result = vkAcquireNextImageKHR(
        device_, swapchain_.handle,
        std::numeric_limits<std::uint64_t>::max(), frame.image_available,
        VK_NULL_HANDLE, &frame.present_image_index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
      swapchain_.dirty = true;
      EnsureSwapchain(width, height);
      result = vkAcquireNextImageKHR(
          device_, swapchain_.handle,
          std::numeric_limits<std::uint64_t>::max(), frame.image_available,
          VK_NULL_HANDLE, &frame.present_image_index);
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
      Check(result, "acquire Vulkan presentation image");
    }
    if (result == VK_SUBOPTIMAL_KHR) {
      swapchain_.dirty = true;
    }
    frame.present_pending = true;
  }

  void RecordPresentation(VkCommandBuffer command,
                          const FrameContext& frame) {
    const auto image = swapchain_.images.at(frame.present_image_index);
    VkImageMemoryBarrier to_transfer{
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_transfer.srcAccessMask = 0;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_transfer.oldLayout =
        swapchain_.initialized.at(frame.present_image_index)
            ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
            : VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image = image;
    to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_transfer.subresourceRange.levelCount = 1;
    to_transfer.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &to_transfer);

    VkImageBlit blit{};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[1] = {static_cast<std::int32_t>(active_target_->width),
                          static_cast<std::int32_t>(active_target_->height), 1};
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[1] = {static_cast<std::int32_t>(swapchain_.extent.width),
                          static_cast<std::int32_t>(swapchain_.extent.height), 1};
    vkCmdBlitImage(command, active_target_->color,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   VK_FILTER_NEAREST);

    if (presentation_overlay_ != nullptr) {
      VkImageMemoryBarrier to_overlay = to_transfer;
      to_overlay.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      to_overlay.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      to_overlay.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      to_overlay.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &to_overlay);

      VkRenderPassBeginInfo begin_info{
          VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      begin_info.renderPass = swapchain_.overlay_render_pass;
      begin_info.framebuffer =
          swapchain_.overlay_framebuffers.at(frame.present_image_index);
      begin_info.renderArea.extent = swapchain_.extent;
      vkCmdBeginRenderPass(command, &begin_info, VK_SUBPASS_CONTENTS_INLINE);
      presentation_overlay_(
          presentation_overlay_user_data_,
          MakePresentationOverlayContext(PresentationOverlayPhase::Render,
                                         command));
      vkCmdEndRenderPass(command);
    } else {
      VkImageMemoryBarrier to_present = to_transfer;
      to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      to_present.dstAccessMask = 0;
      to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &to_present);
    }
    frame_counters_.present_count = 1;
    frame_counters_.presentation_copy_bytes =
        static_cast<std::uint64_t>(swapchain_.extent.width) *
        swapchain_.extent.height * 4U;
  }

  void PresentFrame(const FrameContext& frame) {
    VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    info.waitSemaphoreCount = 1;
    const auto render_finished =
        swapchain_.render_finished.at(frame.present_image_index);
    info.pWaitSemaphores = &render_finished;
    info.swapchainCount = 1;
    info.pSwapchains = &swapchain_.handle;
    info.pImageIndices = &frame.present_image_index;
    const auto result = vkQueuePresentKHR(queue_, &info);
    swapchain_.initialized.at(frame.present_image_index) = true;
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
      swapchain_.dirty = true;
      if (result == VK_SUBOPTIMAL_KHR) {
        ++statistics_.frames_presented;
      }
      return;
    }
    Check(result, "present Vulkan swapchain image");
    ++statistics_.frames_presented;
  }

  // Recovers from a Submit failure between image acquisition and queue
  // submission. The acquire semaphore may still carry a pending signal, so it
  // is drained with a wait-only submission rather than destroyed, and the
  // swapchain is retired so the presentation engine releases the
  // never-presented image on recreation.
  void ReclaimAcquiredImage(FrameContext& frame) noexcept {
    frame.present_pending = false;
    swapchain_.dirty = true;
    if (queue_ == VK_NULL_HANDLE || frame.image_available == VK_NULL_HANDLE) {
      return;
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &frame.image_available;
    submit.pWaitDstStageMask = &wait_stage;
    if (vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS) {
      (void)vkQueueWaitIdle(queue_);
    }
  }

  struct DrawPipelineVariant {
    std::uint32_t feature_mask{};
    std::uint32_t variant_key{};
  };

  DrawPipelineVariant MakeDrawPipelineVariant(
      const extraction::DrawRecord& draw,
      const extraction::FrameSnapshot& snapshot) const {
    const auto& geometry = geometry_records_[draw.geometry_index];
    const auto& material = material_records_[draw.material_index];
    auto feature_mask = static_cast<std::uint32_t>(material.features);
    if (!geometry.has_colors) {
      feature_mask &=
          ~static_cast<std::uint32_t>(MaterialFeature::VertexColor);
    }
    if (!geometry.has_texcoords) {
      feature_mask &=
          ~static_cast<std::uint32_t>(MaterialFeature::BaseColorTexture);
    }
    auto variant_key = feature_mask;
    if (material.alpha_mode == AlphaMode::Masked) {
      variant_key |= kMaskedAlphaFlag;
    }
    if (material.double_sided) {
      variant_key |= kDoubleSidedFlag;
    }
    if (snapshot.front_face == FrontFaceWinding::CounterClockwise) {
      variant_key |= kCounterClockwiseFrontFaceFlag;
    }
    return {feature_mask, variant_key};
  }

  void PreflightGeneratedMaterialPipelines(
      const extraction::FrameSnapshot& snapshot) {
    std::set<std::string> failed_modules;
    for (std::size_t i = 0; i < draw_records_.size(); ++i) {
      const auto& draw = draw_records_[i];
      const auto* artifact =
          selected_material_artifacts_[draw.material_index];
      if (artifact == nullptr) {
        continue;
      }
      if (failed_modules.contains(artifact->module_key)) {
        RejectGeneratedMaterial(
            draw.material_index, MaterialDiagnosticCategory::TargetFailure,
            "the registered Vulkan material artifact could not create a "
            "Forward pipeline");
        continue;
      }
      const auto variant = MakeDrawPipelineVariant(draw, snapshot);
      try {
        (void)EnsureGeneratedPipeline(active_target_->shaders, *artifact,
                                      variant.variant_key);
      } catch (const RendererError& error) {
        if (error.code() == RendererErrorCode::DeviceLost ||
            error.code() == RendererErrorCode::ResourceExhausted ||
            error.code() == RendererErrorCode::Timeout) {
          throw;
        }
        failed_modules.insert(artifact->module_key);
        RejectGeneratedMaterial(
            draw.material_index, MaterialDiagnosticCategory::TargetFailure,
            "the registered Vulkan material artifact could not create a "
            "Forward pipeline");
      }
    }
  }

  void RecordGpuDrivenDispatch(VkCommandBuffer command,
                               const FrameContext& frame,
                               const extraction::FrameSnapshot& snapshot,
                               const GpuDrivenIndexedRequest& request) {
    if (!frame.gpu_driven.selected) {
      return;
    }
    for (const auto& batch : frame.gpu_driven.batches) {
      vkCmdFillBuffer(command, frame.gpu_driven.dispatch_counters.handle,
                      batch.counter_offset,
                      sizeof(shader_abi::GpuDrivenIndexedDispatchCounters),
                      0U);
    }
    VkMemoryBarrier counter_clear_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    counter_clear_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    counter_clear_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                          VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &counter_clear_barrier, 0, nullptr, 0, nullptr);
    for (const auto& batch : frame.gpu_driven.batches) {
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                        batch.compute_pipeline);
      const std::array descriptor_sets{
          bindless_descriptor_set_, frame.bindless_material_descriptor_set,
          batch.descriptor_set};
      const std::uint32_t dynamic_offset{};
      vkCmdBindDescriptorSets(
          command, VK_PIPELINE_BIND_POINT_COMPUTE,
          gpu_driven_pipeline_layout_, 0,
          static_cast<std::uint32_t>(descriptor_sets.size()),
          descriptor_sets.data(), 1, &dynamic_offset);
      shader_abi::GpuDrivenIndexedConstants constants;
      constants.view_projection = Multiply(snapshot.projection, snapshot.view);
      constants.visibility_mask = request.visibility_mask;
      constants.candidate_count = batch.candidate_count;
      constants.flags = 0;
      if (request.enable_visibility_mask_culling) {
        constants.flags |= shader_abi::kGpuDrivenVisibilityMaskCulling;
      }
      if (request.enable_frustum_culling) {
        constants.flags |= shader_abi::kGpuDrivenFrustumCulling;
      }
      vkCmdPushConstants(command, gpu_driven_pipeline_layout_,
                         VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                         &constants);
      const auto workgroup_count =
          shader_abi::GpuDrivenIndexedWorkgroupCount(batch.candidate_count);
      vkCmdDispatch(command, workgroup_count, 1, 1);
    }
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                            VK_ACCESS_SHADER_READ_BIT |
                            VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(
        command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);
  }

  void RecordFrame(VkCommandBuffer command,
                   const FrameContext& frame,
                   const extraction::FrameSnapshot& snapshot,
                   const Vec4& clear_color,
                   const std::vector<Aov>& cpu_readback_aovs) {
    std::array<VkClearValue, 4> clear{};
    clear[0].color = {
        {clear_color.x, clear_color.y, clear_color.z, clear_color.w}};
    clear[1].depthStencil = {1.0F, 0};
    clear[2].color.uint32[0] = std::numeric_limits<std::uint32_t>::max();
    clear[3].color.uint32[0] = std::numeric_limits<std::uint32_t>::max();
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = active_target_->render_pass;
    pass.framebuffer = active_target_->framebuffer;
    pass.renderArea = {{0, 0}, {active_target_->width, active_target_->height}};
    pass.clearValueCount = static_cast<std::uint32_t>(clear.size());
    pass.pClearValues = clear.data();
    vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0.0F, 0.0F, static_cast<float>(active_target_->width),
                              static_cast<float>(active_target_->height), 0.0F, 1.0F};
    const VkRect2D scissor{{0, 0}, {active_target_->width, active_target_->height}};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    const auto view_projection = Multiply(snapshot.projection, snapshot.view);
    const auto gpu_scene_draw_slots =
        gpu_scene_buffers_.pending_update
            ? gpu_scene_buffers_.pending_draw_slot_indices
            : (gpu_scene_buffers_.has_resident_update &&
                       gpu_scene_buffers_.source_id == snapshot.source_id &&
                       gpu_scene_buffers_.revision == snapshot.revision
                   ? gpu_scene_buffers_.draw_slot_indices
                   : nullptr);
    const bool gpu_scene_ready =
        bindless_texture_table_ && gpu_scene_draw_slots &&
        gpu_scene_draw_slots->size() == draw_records_.size();
    if (frame.gpu_driven.selected) {
      const std::array descriptor_sets{
          bindless_descriptor_set_, frame.bindless_material_descriptor_set};
      const std::uint32_t dynamic_offset{};
      vkCmdBindDescriptorSets(
          command, VK_PIPELINE_BIND_POINT_GRAPHICS,
          active_target_->pipeline_layout, 0,
          static_cast<std::uint32_t>(descriptor_sets.size()),
          descriptor_sets.data(), 1, &dynamic_offset);
      const shader_abi::GpuDrivenForwardConstants push{view_projection};
      vkCmdPushConstants(command, active_target_->pipeline_layout,
                         VK_SHADER_STAGE_VERTEX_BIT |
                             VK_SHADER_STAGE_FRAGMENT_BIT,
                         0, sizeof(push), &push);
      for (const auto& batch : frame.gpu_driven.batches) {
        const auto vertex_buffer = vertex_arena_.buffer(batch.vertex_block);
        constexpr VkDeviceSize zero_offset{};
        vkCmdBindVertexBuffers(command, 0, 1, &vertex_buffer, &zero_offset);
        vkCmdBindIndexBuffer(command, index_arena_.buffer(batch.index_block), 0,
                             VK_INDEX_TYPE_UINT32);
        const auto pipeline = EnsureGpuDrivenGraphicsPipeline(
            active_target_->shaders, batch.pipeline_variant);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdDrawIndexedIndirectCount(
            command, frame.gpu_driven.indirect_commands.handle,
            batch.command_offset,
            frame.gpu_driven.dispatch_counters.handle,
            batch.counter_offset +
                offsetof(shader_abi::GpuDrivenIndexedDispatchCounters,
                         visible_count),
            batch.candidate_count,
            sizeof(render::GpuIndexedIndirectCommand));
        ++frame_counters_.gpu_driven_indirect_draw_count;
      }
    } else {
      for (std::size_t i = 0; i < draw_records_.size(); ++i) {
      const auto& draw = draw_records_[i];
      const auto& geometry = geometry_records_[draw.geometry_index];
      const auto& slot = geometry_slots_.at(geometry.mesh);
      const auto vertex_buffer = vertex_arena_.buffer(slot.vertices.block);
      vkCmdBindVertexBuffers(command, 0, 1, &vertex_buffer,
                             &slot.vertices.offset);
      vkCmdBindIndexBuffer(command, index_arena_.buffer(slot.indices.block),
                           slot.indices.offset, VK_INDEX_TYPE_UINT32);
      const auto& instance = instance_records_[draw.instance_index];
      const auto& material = material_records_[draw.material_index];
      const auto variant = MakeDrawPipelineVariant(draw, snapshot);
      auto feature_mask = variant.feature_mask;
      const auto* generated_artifact =
          selected_material_artifacts_[draw.material_index];
      const bool use_gpu_scene =
          gpu_scene_ready && generated_artifact == nullptr;
      if (generated_artifact != nullptr) {
        ++frame_counters_.generated_material_draw_count;
      } else if (material.module) {
        ++frame_counters_.generated_material_fallback_count;
      }
      const auto pipeline =
          generated_artifact != nullptr
              ? EnsureGeneratedPipeline(active_target_->shaders,
                                        *generated_artifact,
                                        variant.variant_key)
              : EnsurePipeline(active_target_->shaders,
                               variant.variant_key |
                                   (use_gpu_scene
                                        ? kGpuScenePipelineFlag
                                        : 0U));
      const auto pipeline_layout =
          generated_artifact != nullptr
              ? active_target_->generated_pipeline_layouts.at(
                    generated_artifact->module_key)
              : active_target_->pipeline_layout;
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      if (bindless_texture_table_) {
        const auto dynamic_offset_value =
            frame.material_uniform_stride * draw.material_index;
        if (dynamic_offset_value >
            std::numeric_limits<std::uint32_t>::max()) {
          throw RendererError(RendererErrorCode::Unsupported,
                              "record bindless material",
                              "dynamic material offset exceeds uint32 range");
        }
        const auto dynamic_offset =
            static_cast<std::uint32_t>(dynamic_offset_value);
        std::array<VkDescriptorSet, 2> descriptor_sets{};
        descriptor_sets[shader_abi::kBindlessTextures.set] =
            bindless_descriptor_set_;
        descriptor_sets[shader_abi::kBindlessMaterialConstants.set] =
            frame.bindless_material_descriptor_set;
        vkCmdBindDescriptorSets(
            command, VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipeline_layout, 0,
            static_cast<std::uint32_t>(descriptor_sets.size()),
            descriptor_sets.data(), 1, &dynamic_offset);
      } else {
        const auto descriptor_set =
            frame.material_descriptor_sets[draw.material_index];
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_layout, 0, 1,
                                &descriptor_set, 0, nullptr);
      }
      if (use_gpu_scene) {
        shader_abi::GpuSceneDrawConstants push;
        push.view_projection = view_projection;
        push.draw_slot = (*gpu_scene_draw_slots)[i];
        vkCmdPushConstants(command, pipeline_layout,
                           VK_SHADER_STAGE_VERTEX_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), &push);
        ++frame_counters_.gpu_scene_draw_count;
      } else {
        PushConstants push;
        push.model_view_projection =
            Multiply(view_projection, instance.transform);
        const auto normal_matrix = NormalMatrix(instance.transform);
        push.normal_matrix_column0 = normal_matrix[0];
        push.normal_matrix_column1 = normal_matrix[1];
        push.normal_matrix_column2 = normal_matrix[2];
        push.feature_mask = feature_mask;
        if (material.alpha_mode == AlphaMode::Masked) {
          push.feature_mask |= kMaskedAlphaFlag;
        }
        if (bindless_texture_table_ && material.base_color_texture) {
          const auto texture_handle =
              texture_records_[material.base_color_texture->texture_index]
                  .texture;
          const auto sampler_handle =
              sampler_records_[material.base_color_texture->sampler_index]
                  .sampler;
          const auto texture_slot =
              texture_slots_.at(texture_handle).bindless_slot;
          const auto sampler_slot =
              sampler_slots_.at(sampler_handle).bindless_slot;
          if (sampler_slot.index > kSamplerIndexMask) {
            throw RendererError(
                RendererErrorCode::Unsupported,
                "record bindless material",
                "bindless sampler index exceeds shader encoding");
          }
          push.texture_index = texture_slot.index;
          push.feature_mask |= sampler_slot.index << kSamplerIndexShift;
        }
        push.prim_id = static_cast<std::uint32_t>(geometry.mesh);
        push.instance_id = static_cast<std::uint32_t>(instance.instance);
        vkCmdPushConstants(command, pipeline_layout,
                           VK_SHADER_STAGE_VERTEX_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), &push);
      }
        vkCmdDrawIndexed(command, slot.index_count, 1, 0, 0, 0);
      }
    }
    vkCmdNextSubpass(command, VK_SUBPASS_CONTENTS_INLINE);
    if (frame.timestamp_pool != VK_NULL_HANDLE) {
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                          frame.timestamp_pool, 2);
    }
    if (frame.gaussian_instance_count != 0) {
      const auto pipeline = EnsureGaussianPipeline(active_target_->shaders);
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      const std::array<VkBuffer, 2> vertex_buffers{
          gaussian_corner_vertices_.handle, frame.gaussian_instances.handle};
      const std::array<VkDeviceSize, 2> offsets{};
      vkCmdBindVertexBuffers(command, 0,
                             static_cast<std::uint32_t>(vertex_buffers.size()),
                             vertex_buffers.data(), offsets.data());
      const GaussianPushConstants push{{
          1.0F / static_cast<float>(active_target_->width),
          1.0F / static_cast<float>(active_target_->height),
      }};
      vkCmdPushConstants(command, active_target_->pipeline_layout,
                         VK_SHADER_STAGE_VERTEX_BIT |
                             VK_SHADER_STAGE_FRAGMENT_BIT,
                         0, sizeof(push), &push);
      vkCmdDraw(command, 6, frame.gaussian_instance_count, 0, 0);
      ++frame_counters_.gaussian_draw_count;
    }
    vkCmdNextSubpass(command, VK_SUBPASS_CONTENTS_INLINE);
    if (frame.gaussian_instance_count != 0) {
      const auto pipeline =
          EnsureGaussianPipeline(active_target_->shaders, true);
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      const std::array<VkBuffer, 2> vertex_buffers{
          gaussian_corner_vertices_.handle, frame.gaussian_instances.handle};
      const std::array<VkDeviceSize, 2> offsets{};
      vkCmdBindVertexBuffers(command, 0,
                             static_cast<std::uint32_t>(vertex_buffers.size()),
                             vertex_buffers.data(), offsets.data());
      const GaussianPushConstants push{{
          1.0F / static_cast<float>(active_target_->width),
          1.0F / static_cast<float>(active_target_->height),
      }};
      vkCmdPushConstants(command, active_target_->pipeline_layout,
                         VK_SHADER_STAGE_VERTEX_BIT |
                             VK_SHADER_STAGE_FRAGMENT_BIT,
                         0, sizeof(push), &push);
      vkCmdDraw(command, 6, frame.gaussian_instance_count, 0, 0);
      ++frame_counters_.gaussian_draw_count;
    }
    if (frame.timestamp_pool != VK_NULL_HANDLE) {
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                          frame.timestamp_pool, 3);
    }
    vkCmdEndRenderPass(command);

    if (frame.gpu_driven.selected) {
      for (const auto& batch : frame.gpu_driven.batches) {
        const VkBufferCopy batch_copy{
            batch.counter_offset, batch.counter_offset,
            sizeof(shader_abi::GpuDrivenIndexedDispatchCounters)};
        vkCmdCopyBuffer(command,
                        frame.gpu_driven.dispatch_counters.handle,
                        frame.gpu_driven.counter_readback.handle, 1,
                        &batch_copy);
      }
    }

    if (HasAov(cpu_readback_aovs, Aov::Color)) {
      VkBufferImageCopy copy{};
      copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      copy.imageSubresource.layerCount = 1;
      copy.imageExtent = {active_target_->width, active_target_->height, 1};
      vkCmdCopyImageToBuffer(command, active_target_->color,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             active_target_->color_readback.handle, 1, &copy);
    }
    if (HasAov(cpu_readback_aovs, Aov::Depth)) {
      VkBufferImageCopy copy{};
      copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
      copy.imageSubresource.layerCount = 1;
      copy.imageExtent = {active_target_->width, active_target_->height, 1};
      vkCmdCopyImageToBuffer(command, active_target_->depth,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             active_target_->depth_readback.handle, 1, &copy);
    }
    VkBufferImageCopy id_copy{};
    id_copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    id_copy.imageSubresource.layerCount = 1;
    id_copy.imageExtent = {active_target_->width, active_target_->height, 1};
    if (HasAov(cpu_readback_aovs, Aov::PrimId)) {
      vkCmdCopyImageToBuffer(command, active_target_->prim_id,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             active_target_->prim_id_readback.handle, 1,
                             &id_copy);
    }
    if (HasAov(cpu_readback_aovs, Aov::InstanceId)) {
      vkCmdCopyImageToBuffer(command, active_target_->instance_id,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             active_target_->instance_id_readback.handle, 1,
                             &id_copy);
    }
    if (frame.present_pending) {
      RecordPresentation(command, frame);
    }
  }

  std::uint64_t SubmitTransfers(FrameContext& frame) {
    const auto completion = ++transfer_timeline_value_;
    VkTimelineSemaphoreSubmitInfo timeline{
        VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timeline.signalSemaphoreValueCount = 1;
    timeline.pSignalSemaphoreValues = &completion;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.pNext = &timeline;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.transfer_command_buffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &transfer_timeline_semaphore_;
    Check(vkQueueSubmit(transfer_queue_, 1, &submit, VK_NULL_HANDLE),
          "submit asynchronous resource uploads");
    ++frame_counters_.transfer_submission_count;
    ++transfer_submission_count_;
    transfer_uploaded_bytes_ += frame_counters_.upload_bytes;
    transfer_ownership_count_ +=
        frame_counters_.queue_ownership_transfer_count;
    return completion;
  }

  std::uint64_t SubmitFrame(FrameContext& frame,
                            std::uint64_t transfer_completion) {
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.command_buffer;

    std::array<VkSemaphore, 2> wait_semaphores{};
    std::array<VkPipelineStageFlags, 2> wait_stages{};
    std::array<std::uint64_t, 2> wait_values{};
    std::uint32_t wait_count{};
    if (transfer_completion != 0) {
      wait_semaphores[wait_count] = transfer_timeline_semaphore_;
      wait_stages[wait_count] = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                                VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      wait_values[wait_count] = transfer_completion;
      ++wait_count;
    }
    if (frame.present_pending) {
      wait_semaphores[wait_count] = frame.image_available;
      wait_stages[wait_count] = VK_PIPELINE_STAGE_TRANSFER_BIT;
      wait_values[wait_count] = 0;
      ++wait_count;
    }
    submit.waitSemaphoreCount = wait_count;
    submit.pWaitSemaphores = wait_semaphores.data();
    submit.pWaitDstStageMask = wait_stages.data();

    std::array<VkSemaphore, 2> signal_semaphores{};
    std::array<std::uint64_t, 2> signal_values{};
    std::uint32_t signal_count{};
    std::uint64_t completion{};
    VkTimelineSemaphoreSubmitInfo timeline{
        VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    if (timeline_semaphore_ != VK_NULL_HANDLE) {
      completion = ++timeline_value_;
      timeline.waitSemaphoreValueCount = wait_count;
      timeline.pWaitSemaphoreValues = wait_values.data();
      signal_semaphores[signal_count] = timeline_semaphore_;
      signal_values[signal_count] = completion;
      ++signal_count;
      if (frame.present_pending) {
        signal_semaphores[signal_count] =
            swapchain_.render_finished.at(frame.present_image_index);
        signal_values[signal_count] = 0;
        ++signal_count;
      }
      timeline.signalSemaphoreValueCount = signal_count;
      timeline.pSignalSemaphoreValues = signal_values.data();
      submit.pNext = &timeline;
      submit.signalSemaphoreCount = signal_count;
      submit.pSignalSemaphores = signal_semaphores.data();
      Check(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE),
            "submit extracted scene frame");
    } else {
      completion = ++timeline_value_;
      if (frame.present_pending) {
        signal_semaphores[0] =
            swapchain_.render_finished.at(frame.present_image_index);
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = signal_semaphores.data();
      }
      Check(vkQueueSubmit(queue_, 1, &submit, frame.fence),
            "submit extracted scene frame");
    }
    frame.completion_value = completion;
    return completion;
  }

  void WaitForFrame(FrameContext& frame, std::chrono::nanoseconds timeout) {
    const auto timeout_ns = timeout == std::chrono::nanoseconds::max()
                                ? std::numeric_limits<std::uint64_t>::max()
                                : static_cast<std::uint64_t>(timeout.count());
    if (timeline_semaphore_ != VK_NULL_HANDLE) {
      VkSemaphoreWaitInfo wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
      wait_info.semaphoreCount = 1;
      wait_info.pSemaphores = &timeline_semaphore_;
      wait_info.pValues = &frame.completion_value;
      Check(vkWaitSemaphores(device_, &wait_info, timeout_ns),
            "wait for render completion");
    } else {
      Check(vkWaitForFences(device_, 1, &frame.fence, VK_TRUE, timeout_ns),
            "wait for render completion");
    }
  }

  std::uint64_t ReadGpuTimestampSpanNanoseconds(
      const FrameContext& frame, std::uint32_t first,
      std::uint32_t second) const {
    if (frame.timestamp_pool == VK_NULL_HANDLE) {
      return 0;
    }
    if (second != first + 1U) {
      throw std::logic_error("timestamp span queries must be adjacent");
    }
    std::array<std::uint64_t, 2> timestamps{};
    Check(vkGetQueryPoolResults(device_, frame.timestamp_pool, first,
                                static_cast<std::uint32_t>(timestamps.size()),
                                sizeof(timestamps), timestamps.data(),
                                sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT),
          "read frame timestamp queries");
    const std::uint64_t mask = selected_timestamp_valid_bits_ >= 64U
                                   ? std::numeric_limits<std::uint64_t>::max()
                                   : (std::uint64_t{1}
                                      << selected_timestamp_valid_bits_) - 1U;
    const auto ticks = (timestamps[1] - timestamps[0]) & mask;
    return static_cast<std::uint64_t>(
        static_cast<long double>(ticks) * timestamp_period_ns_);
  }

  std::uint64_t ReadGpuExecutionNanoseconds(const FrameContext& frame) const {
    return ReadGpuTimestampSpanNanoseconds(frame, 0, 1);
  }

  std::uint64_t ReadGaussianRasterNanoseconds(
      const FrameContext& frame) const {
    return ReadGpuTimestampSpanNanoseconds(frame, 2, 3);
  }

  ImageRgba8 ReadColor(std::uint32_t width, std::uint32_t height) {
    frame_counters_.readback_bytes +=
        static_cast<std::uint64_t>(width) * height * 4U;
    ImageRgba8 result;
    result.product = MakeRenderProduct(width, height, Aov::Color);
    result.row_pitch_bytes = width * BytesPerPixel(result.product.format);
    result.pixels.resize(static_cast<std::size_t>(result.row_pitch_bytes) *
                         height);
    void* mapped{};
    ++frame_counters_.map_count;
    Check(vkMapMemory(device_, active_target_->color_readback.memory, 0,
                      active_target_->color_readback.size, 0, &mapped),
          "map color readback");
    std::memcpy(result.pixels.data(), mapped, result.pixels.size());
    vkUnmapMemory(device_, active_target_->color_readback.memory);
    return result;
  }

  ImageDepth32 ReadDepth(std::uint32_t width, std::uint32_t height) {
    frame_counters_.readback_bytes +=
        static_cast<std::uint64_t>(width) * height * sizeof(float);
    ImageDepth32 result;
    result.product = MakeRenderProduct(width, height, Aov::Depth);
    result.row_pitch_bytes = width * BytesPerPixel(result.product.format);
    result.pixels.resize(static_cast<std::size_t>(width) * height);
    void* mapped{};
    ++frame_counters_.map_count;
    Check(vkMapMemory(device_, active_target_->depth_readback.memory, 0,
                      active_target_->depth_readback.size, 0, &mapped),
          "map depth readback");
    std::memcpy(result.pixels.data(), mapped,
                result.pixels.size() * sizeof(float));
    vkUnmapMemory(device_, active_target_->depth_readback.memory);
    return result;
  }

  ImageUint32 ReadId(std::uint32_t width, std::uint32_t height, Aov aov,
                     const Buffer& readback) {
    frame_counters_.readback_bytes +=
        static_cast<std::uint64_t>(width) * height * sizeof(std::uint32_t);
    ImageUint32 result;
    result.product = MakeRenderProduct(width, height, aov);
    result.row_pitch_bytes = width * BytesPerPixel(result.product.format);
    result.pixels.resize(static_cast<std::size_t>(width) * height);
    void* mapped{};
    ++frame_counters_.map_count;
    Check(vkMapMemory(device_, readback.memory, 0, readback.size, 0, &mapped),
          "map id readback");
    std::memcpy(result.pixels.data(), mapped,
                result.pixels.size() * sizeof(std::uint32_t));
    vkUnmapMemory(device_, readback.memory);
    return result;
  }

  void ResolveGpuDrivenCounters(FrameContext& frame) {
    if (!frame.gpu_driven.selected) {
      return;
    }
    std::uint64_t candidate_count{};
    std::uint64_t visible_count{};
    std::uint64_t visibility_mask_culled_count{};
    std::uint64_t frustum_culled_count{};
    std::vector<shader_abi::GpuDrivenIndexedDispatchCounters> batch_counters(
        frame.gpu_driven.batches.size());
    void* mapped{};
    Check(vkMapMemory(device_, frame.gpu_driven.counter_readback.memory, 0,
                      frame.gpu_driven.counter_readback.size, 0, &mapped),
          "map GPU-driven dispatch counters");
    for (std::size_t i = 0; i < frame.gpu_driven.batches.size(); ++i) {
      std::memcpy(&batch_counters[i],
                  static_cast<const std::byte*>(mapped) +
                      frame.gpu_driven.batches[i].counter_offset,
                  sizeof(batch_counters[i]));
    }
    vkUnmapMemory(device_, frame.gpu_driven.counter_readback.memory);
    for (std::size_t i = 0; i < frame.gpu_driven.batches.size(); ++i) {
      const auto& batch = frame.gpu_driven.batches[i];
      const auto& counters = batch_counters[i];
      if (counters.candidate_count != batch.candidate_count ||
          static_cast<std::uint64_t>(counters.visible_count) +
                  counters.visibility_mask_culled_count +
                  counters.frustum_culled_count !=
              counters.candidate_count) {
        throw RendererError(
            RendererErrorCode::BackendFailure,
            "resolve GPU-driven dispatch counters",
            "compute counters violate an arena/pipeline batch partition");
      }
      candidate_count += counters.candidate_count;
      visible_count += counters.visible_count;
      visibility_mask_culled_count +=
          counters.visibility_mask_culled_count;
      frustum_culled_count += counters.frustum_culled_count;
    }
    if (candidate_count != frame.gpu_driven.candidate_count) {
      throw RendererError(RendererErrorCode::BackendFailure,
                          "resolve GPU-driven dispatch counters",
                          "batch candidate counts do not match the frame");
    }
    frame_counters_.gpu_driven_visible_draw_count = visible_count;
    frame_counters_.gpu_driven_visibility_mask_culled_count =
        visibility_mask_culled_count;
    frame_counters_.gpu_driven_frustum_culled_count =
        frustum_culled_count;
    frame_counters_.gpu_scene_draw_count = visible_count;
  }

  static VKAPI_ATTR VkBool32 VKAPI_CALL ValidationCallback(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT type,
      const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
      void* user_data) {
    auto* self = static_cast<Impl*>(user_data);
    const char* message =
        callback_data != nullptr && callback_data->pMessage != nullptr
            ? callback_data->pMessage
            : "unknown validation message";
    // Validation and performance messages are renderer-owned quality signals and
    // are counted as failures. General loader/host diagnostics stay observable on
    // stderr but are not counted as renderer validation failures.
    const bool renderer_signal =
        (type & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                 VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) != 0U;
    if (renderer_signal) {
      self->validation_messages_.fetch_add(1, std::memory_order_relaxed);
      std::cerr << "Merlin Vulkan validation: " << message << '\n';
    } else {
      std::cerr << "Merlin Vulkan general: " << message << '\n';
    }
    if (self->diagnostic_sink_ != nullptr) {
      const bool performance_signal =
          (type & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) != 0U;
      const char* message_id =
          callback_data != nullptr && callback_data->pMessageIdName != nullptr
              ? callback_data->pMessageIdName
              : "vulkan";
      Diagnostic diagnostic;
      diagnostic.code = renderer_signal
                            ? (performance_signal ? "vulkan.performance"
                                                  : "vulkan.validation")
                            : "vulkan.general";
      diagnostic.severity =
          (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0U
              ? DiagnosticSeverity::Error
              : DiagnosticSeverity::Warning;
      diagnostic.disposition = renderer_signal
                                   ? DiagnosticDisposition::Rejected
                                   : DiagnosticDisposition::Ignored;
      diagnostic.source = message_id;
      diagnostic.message = message;
      diagnostic.recovery = renderer_signal ? "fix-vulkan-diagnostic" : "none";
      // A host callback must never unwind through the Vulkan C ABI.
      try {
        self->diagnostic_sink_->Report(diagnostic);
      } catch (...) {
      }
    }
    return VK_FALSE;
  }

  VkInstance instance_{};
  VkSurfaceKHR surface_{};
  VkPhysicalDevice physical_device_{};
  VkDevice device_{};
  VkQueue queue_{};
  VkQueue transfer_queue_{};
  VkDebugUtilsMessengerEXT debug_messenger_{};
  VkSemaphore timeline_semaphore_{};
  VkSemaphore transfer_timeline_semaphore_{};
  std::uint32_t queue_family_{};
  std::uint32_t transfer_queue_family_{VK_QUEUE_FAMILY_IGNORED};
  std::uint32_t selected_timestamp_valid_bits_{};
  float timestamp_period_ns_{};
  std::uint64_t timeline_value_{};
  std::uint64_t transfer_timeline_value_{};
  std::uint64_t transfer_submission_count_{};
  std::uint64_t transfer_uploaded_bytes_{};
  std::uint64_t transfer_ownership_count_{};
  std::uint64_t aov_image_export_count_{};
  std::uint64_t active_aov_image_leases_{};
  std::uint64_t latest_completed_value_{};
  VkDeviceSize uniform_buffer_alignment_{16U};
  VkDeviceSize storage_buffer_alignment_{4U};
  VkDeviceSize max_storage_buffer_range_{};
  std::uint32_t max_draw_indirect_count_{};
  std::uint32_t max_compute_work_group_count_x_{};
  bool owns_vulkan_context_{true};
  const std::uint64_t owner_id_{
      g_renderer_owner.fetch_add(1, std::memory_order_relaxed)};
  RendererCapabilities capabilities_;
  RendererStatistics statistics_;
  FrameCounters frame_counters_;
  DeviceMemoryBudget memory_budget_;
  IndexedTableView<extraction::GeometryRecord> geometry_records_;
  IndexedTableView<extraction::TextureRecord> texture_records_;
  IndexedTableView<extraction::SamplerRecord> sampler_records_;
  IndexedTableView<extraction::MaterialRecord> material_records_;
  IndexedTableView<extraction::InstanceRecord> instance_records_;
  DenseTableView<extraction::DrawRecord> draw_records_;
  detail::GaussianPreparationResult prepared_gaussians_;
  extraction::PersistentTable<extraction::GaussianRecord>
      gaussian_preparation_source_;
  Mat4 gaussian_preparation_view_;
  Mat4 gaussian_preparation_projection_;
  std::uint32_t gaussian_preparation_width_{};
  std::uint32_t gaussian_preparation_height_{};
  bool gaussian_preparation_cache_valid_{};
  std::uint64_t gaussian_preparation_generation_{};
  std::vector<FrameContext> frames_;
  std::vector<RetiredBuffer> deferred_;
  std::vector<RetiredTexture> retired_textures_;
  std::vector<RetiredSampler> retired_samplers_;
  std::filesystem::path environment_path_;
  detail::DiffuseEnvironment environment_lighting_;
  DeviceArena vertex_arena_;
  DeviceArena index_arena_;
  DeviceArena gaussian_position_arena_;
  DeviceArena gaussian_covariance_arena_;
  DeviceArena gaussian_opacity_arena_;
  DeviceArena gaussian_radiance_arena_;
  StagingRing staging_;
  StagingRing gaussian_staging_;
  StagingRing gpu_scene_staging_;
  StagingRing gpu_driven_staging_;
  GpuSceneBuffers gpu_scene_buffers_;
  Buffer gaussian_corner_vertices_;
  std::map<std::uint64_t, GeometrySlot> geometry_slots_;
  std::map<std::uint64_t, GaussianAttributeSlot> gaussian_attribute_slots_;
  std::map<std::uint64_t, TextureSlot> texture_slots_;
  std::map<std::uint64_t, SamplerSlot> sampler_slots_;
  std::vector<RetiredRange> retired_ranges_;
  std::vector<PendingCopy> pending_copies_;
  std::vector<PendingImageCopy> pending_image_copies_;
  std::vector<VkImage> pending_graphics_acquire_images_;
  std::vector<std::uint64_t> pending_texture_handles_;
  std::vector<Buffer> frame_upload_buffers_;
  std::unique_ptr<BindlessTextureTable> bindless_texture_table_;
  std::unique_ptr<BindlessSamplerTable> bindless_sampler_table_;
  std::vector<VkImageView> bindless_texture_views_;
  std::vector<VkSampler> bindless_samplers_;
  std::vector<TextureSlot> reserved_bindless_textures_;
  bool force_reserved_bindless_texture_writes_{};
  VkDescriptorSetLayout bindless_descriptor_set_layout_{};
  VkDescriptorSetLayout bindless_material_descriptor_set_layout_{};
  VkDescriptorSetLayout gpu_driven_descriptor_set_layout_{};
  VkPipelineLayout gpu_driven_pipeline_layout_{};
  std::map<std::filesystem::path, VkPipeline> gpu_driven_pipelines_;
  VkDescriptorPool bindless_descriptor_pool_{};
  VkDescriptorSet bindless_descriptor_set_{};
  TextureSlot fallback_texture_;
  SamplerSlot fallback_sampler_;
  VkDescriptorSetLayout descriptor_set_layout_{};
  std::map<std::string, VkDescriptorSetLayout>
      generated_descriptor_set_layouts_;
  std::map<std::string, GeneratedMaterialArtifact>
      generated_material_artifacts_;
  std::vector<const GeneratedMaterialArtifact*> selected_material_artifacts_;
  std::vector<MaterialDiagnostic> frame_material_diagnostics_;
  std::map<std::filesystem::path, VkShaderModule> shader_modules_;
  std::uint64_t resident_snapshot_source_{};
  std::uint64_t resident_snapshot_revision_{};
  bool resource_residency_dirty_{};
  bool presentation_vsync_{true};
  void* presentation_overlay_user_data_{};
  PresentationOptions::RenderOverlay presentation_overlay_{};
  bool presentation_overlay_initialized_{};
  SwapchainState swapchain_;
  RenderTarget* active_target_{};
  std::atomic<std::uint64_t> validation_messages_{};
  DiagnosticSink* diagnostic_sink_{};
};

Renderer::Renderer(RendererOptions options)
    : impl_(std::make_unique<Impl>(options)) {}
Renderer::~Renderer() = default;
Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;

const RendererCapabilities& Renderer::capabilities() const noexcept {
  return impl_->capabilities_;
}

RendererStatistics Renderer::statistics() const noexcept {
  impl_->memory_budget_.Refresh();
  auto result = impl_->statistics_;
  result.validation_messages =
      impl_->validation_messages_.load(std::memory_order_relaxed);
  result.aov_image_export_count = impl_->aov_image_export_count_;
  result.active_aov_image_leases = impl_->active_aov_image_leases_;
  result.pending_geometry_retirements = 0;
  result.pending_gaussian_attribute_retirements = 0;
  result.geometry_arena_blocks =
      impl_->vertex_arena_.block_count() + impl_->index_arena_.block_count();
  result.vertex_arena = impl_->vertex_arena_.telemetry();
  result.index_arena = impl_->index_arena_.telemetry();
  const auto add_gaussian_arena = [&](const ArenaTelemetry& source) {
    auto& destination = result.gaussian_attribute_arena;
    destination.capacity_bytes += source.capacity_bytes;
    destination.resident_bytes += source.resident_bytes;
    destination.peak_resident_bytes += source.peak_resident_bytes;
    destination.free_bytes += source.free_bytes;
    destination.largest_free_span_bytes = std::max(
        destination.largest_free_span_bytes, source.largest_free_span_bytes);
    destination.retiring_bytes += source.retiring_bytes;
    destination.allocation_count += source.allocation_count;
    destination.release_count += source.release_count;
    destination.active_ranges += source.active_ranges;
    destination.peak_active_ranges += source.peak_active_ranges;
    destination.retiring_ranges += source.retiring_ranges;
    destination.free_spans += source.free_spans;
    destination.blocks += source.blocks;
    destination.growth_count += source.growth_count;
  };
  add_gaussian_arena(impl_->gaussian_position_arena_.telemetry());
  add_gaussian_arena(impl_->gaussian_covariance_arena_.telemetry());
  add_gaussian_arena(impl_->gaussian_opacity_arena_.telemetry());
  add_gaussian_arena(impl_->gaussian_radiance_arena_.telemetry());
  result.upload_ring = impl_->staging_.telemetry();
  result.gaussian_attribute_upload_ring =
      impl_->gaussian_staging_.telemetry();
  result.gpu_scene_upload_ring = impl_->gpu_scene_staging_.telemetry();
  result.gpu_scene_buffers = impl_->gpu_scene_buffers_.enabled();
  result.gpu_scene_capacity_bytes =
      impl_->gpu_scene_buffers_.geometries.size +
      impl_->gpu_scene_buffers_.instances.size +
      impl_->gpu_scene_buffers_.materials.size +
      impl_->gpu_scene_buffers_.draws.size;
  result.gaussian_attribute_resources =
      static_cast<std::uint32_t>(impl_->gaussian_attribute_slots_.size());
  result.memory_budget = impl_->memory_budget_.telemetry();
  result.transfer_queue = {
      impl_->capabilities_.async_transfer_queue,
      impl_->queue_family_,
      impl_->transfer_queue_family_,
      impl_->transfer_submission_count_,
      impl_->transfer_uploaded_bytes_,
      impl_->transfer_ownership_count_,
      impl_->transfer_timeline_value_,
  };
  for (const auto& retired : impl_->retired_ranges_) {
    auto* arena = &result.gaussian_attribute_arena;
    if (retired.arena == &impl_->vertex_arena_) {
      arena = &result.vertex_arena;
      ++result.pending_geometry_retirements;
    } else if (retired.arena == &impl_->index_arena_) {
      arena = &result.index_arena;
      ++result.pending_geometry_retirements;
    } else {
      ++result.pending_gaussian_attribute_retirements;
    }
    ++arena->retiring_ranges;
    arena->retiring_bytes += retired.range.size;
  }
  if (impl_->bindless_texture_table_) {
    result.bindless_texture_slots =
        impl_->bindless_texture_table_->telemetry();
    result.bindless_samplers = impl_->bindless_sampler_table_->telemetry();
  }
  return result;
}

bool HasAov(const std::vector<Aov>& aovs, Aov aov) noexcept {
  return std::find(aovs.begin(), aovs.end(), aov) != aovs.end();
}

bool HasCpuReadback(const RenderResult& result, Aov aov) noexcept {
  return HasAov(result.cpu_readback_aovs, aov);
}

void ValidateRenderResult(const RenderResult& result) {
  if (result.rendered_aovs.empty()) {
    throw std::invalid_argument("render result has no rendered AOVs");
  }
  for (const auto aov : result.cpu_readback_aovs) {
    if (!HasAov(result.rendered_aovs, aov)) {
      throw std::invalid_argument("CPU readback AOV was not rendered");
    }
  }

  std::uint32_t width{};
  std::uint32_t height{};
  auto validate_metadata = [&](const RenderProduct& product, Aov aov,
                               std::uint32_t row_pitch) {
    if (!IsCanonicalRenderProduct(product) || product.aov != aov) {
      throw std::invalid_argument("invalid " + std::string(AovName(aov)) +
                                  " render product metadata");
    }
    if (row_pitch != TightRowPitchBytes(product)) {
      throw std::invalid_argument("render product row pitch is not tight");
    }
    if (width == 0) {
      width = product.width;
      height = product.height;
    } else if (width != product.width || height != product.height) {
      throw std::invalid_argument("render product extents do not match");
    }
  };
  if (HasCpuReadback(result, Aov::Color)) {
    validate_metadata(result.color.product, Aov::Color,
                      result.color.row_pitch_bytes);
    const auto bytes = static_cast<std::uint64_t>(result.color.row_pitch_bytes) *
                       result.color.product.height;
    if (bytes != result.color.pixels.size()) {
      throw std::invalid_argument("color render product payload size is invalid");
    }
  }
  if (HasCpuReadback(result, Aov::Depth)) {
    validate_metadata(result.depth.product, Aov::Depth,
                      result.depth.row_pitch_bytes);
    const auto values = static_cast<std::uint64_t>(result.depth.product.width) *
                        result.depth.product.height;
    if (values != result.depth.pixels.size()) {
      throw std::invalid_argument("depth render product payload size is invalid");
    }
    if (std::any_of(result.depth.pixels.begin(), result.depth.pixels.end(),
                    [](float value) {
                      return !std::isfinite(value) || value < 0.0F ||
                             value > 1.0F;
                    })) {
      throw std::invalid_argument(
          "depth render product contains invalid values");
    }
  }
  auto validate_id = [&](const ImageUint32& image, Aov aov) {
    validate_metadata(image.product, aov, image.row_pitch_bytes);
    const auto values = static_cast<std::uint64_t>(image.product.width) *
                        image.product.height;
    if (values != image.pixels.size()) {
      throw std::invalid_argument("ID render product payload size is invalid");
    }
  };
  if (HasCpuReadback(result, Aov::PrimId)) {
    validate_id(result.prim_id, Aov::PrimId);
  }
  if (HasCpuReadback(result, Aov::InstanceId)) {
    validate_id(result.instance_id, Aov::InstanceId);
  }
  if (result.completion_value == 0) {
    throw std::invalid_argument("render result has no completion value");
  }
}

CompletionToken Renderer::Submit(const RenderRequest& request) {
  return CompletionToken(impl_->owner_id_, impl_->Submit(request));
}

bool Renderer::IsComplete(CompletionToken token) const {
  if (!token || token.owner_ != impl_->owner_id_) {
    throw RendererError(RendererErrorCode::InvalidToken,
                        "query completion token",
                        "token belongs to a different renderer");
  }
  return impl_->IsComplete(token.value_);
}

AovImageExport Renderer::AcquireAovImage(CompletionToken token, Aov aov) {
  if (!token || token.owner_ != impl_->owner_id_) {
    throw RendererError(RendererErrorCode::InvalidToken,
                        "acquire AOV image",
                        "token belongs to another renderer");
  }
  auto result = impl_->AcquireAovImage(token.value_, aov);
  result.lease.owner_ = impl_->owner_id_;
  result.lease.completion_ = token.value_;
  result.lease.aov_ = aov;
  return result;
}

void Renderer::ReleaseAovImage(AovImageLease&& lease) {
  if (!lease || lease.owner_ != impl_->owner_id_) {
    throw RendererError(RendererErrorCode::InvalidToken,
                        "release AOV image",
                        "lease belongs to another renderer");
  }
  impl_->ReleaseAovImage(lease.completion_, lease.aov_);
  lease.owner_ = 0;
  lease.completion_ = 0;
}

RenderResult Renderer::Resolve(CompletionToken token,
                               std::chrono::nanoseconds timeout) {
  if (!token || token.owner_ != impl_->owner_id_) {
    throw RendererError(RendererErrorCode::InvalidToken,
                        "resolve completion token",
                        "token belongs to a different renderer");
  }
  if (timeout < std::chrono::nanoseconds::zero()) {
    throw RendererError(RendererErrorCode::InvalidRequest,
                        "resolve completion token",
                        "timeout must not be negative");
  }
  auto result = impl_->Resolve(token.value_, timeout);
  ValidateRenderResult(result);
  return result;
}

RenderResult Renderer::Render(const extraction::FrameSnapshot& snapshot,
                              std::uint32_t width, std::uint32_t height,
                              const ShaderPaths& shaders) {
  RenderRequest request;
  // Submit consumes all snapshot CPU storage before returning. An empty-owner
  // alias keeps the synchronous compatibility path allocation-free without
  // weakening the owning boundary required by the asynchronous public API.
  request.snapshot = std::shared_ptr<const extraction::FrameSnapshot>(
      std::shared_ptr<const extraction::FrameSnapshot>{}, &snapshot);
  request.width = width;
  request.height = height;
  request.shaders = shaders;
  request.products = {{Aov::Color, true}, {Aov::Depth, true},
                      {Aov::PrimId, true}, {Aov::InstanceId, true}};
  return Resolve(Submit(request));
}

}  // namespace merlin::vulkan
