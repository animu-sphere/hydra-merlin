#include <merlin/vulkan/renderer.hpp>

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
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace merlin::vulkan {
namespace {

using CpuClock = std::chrono::steady_clock;

std::uint64_t ElapsedNanoseconds(CpuClock::time_point start) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(CpuClock::now() - start)
          .count());
}

constexpr std::uint32_t kMinimumVulkanApiVersion = VK_MAKE_API_VERSION(
    0, MERLIN_VULKAN_MIN_VERSION_MAJOR, MERLIN_VULKAN_MIN_VERSION_MINOR, 0);

constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat kIdFormat = VK_FORMAT_R32_UINT;

void Check(VkResult result, const char* operation) {
  if (result != VK_SUCCESS) {
    throw std::runtime_error(std::string(operation) + " failed with VkResult " +
                             std::to_string(static_cast<int>(result)));
  }
}

std::vector<std::uint32_t> ReadSpirv(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    throw std::runtime_error("could not open SPIR-V file: " + path.string());
  }
  const auto end = stream.tellg();
  if (end <= 0 || (end % static_cast<std::streamoff>(sizeof(std::uint32_t))) != 0) {
    throw std::runtime_error("invalid SPIR-V file size: " + path.string());
  }
  std::vector<std::uint32_t> code(static_cast<std::size_t>(end) /
                                  sizeof(std::uint32_t));
  stream.seekg(0);
  stream.read(reinterpret_cast<char*>(code.data()), end);
  if (!stream) {
    throw std::runtime_error("could not read SPIR-V file: " + path.string());
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

struct PushConstants {
  Mat4 model_view_projection;
  Vec4 base_color;
  std::uint32_t prim_id{};
  std::uint32_t instance_id{};
};

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

void DestroyBufferRaw(VkDevice device, Buffer& buffer) noexcept {
  if (buffer.handle != VK_NULL_HANDLE) {
    vkDestroyBuffer(device, buffer.handle, nullptr);
  }
  if (buffer.memory != VK_NULL_HANDLE) {
    vkFreeMemory(device, buffer.memory, nullptr);
  }
  buffer = {};
}

Buffer CreateBufferRaw(VkDevice device, VkPhysicalDevice physical_device,
                       VkDeviceSize size, VkBufferUsageFlags usage,
                       VkMemoryPropertyFlags properties) {
  Buffer result;
  result.size = size;
  VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = size;
  buffer_info.usage = usage;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  Check(vkCreateBuffer(device, &buffer_info, nullptr, &result.handle),
        "create buffer");
  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(device, result.handle, &requirements);
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex =
      FindMemoryTypeRaw(physical_device, requirements.memoryTypeBits, properties);
  try {
    Check(vkAllocateMemory(device, &allocation, nullptr, &result.memory),
          "allocate buffer memory");
    Check(vkBindBufferMemory(device, result.handle, result.memory, 0),
          "bind buffer memory");
  } catch (...) {
    DestroyBufferRaw(device, result);
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
  };

  void Initialize(VkDevice device, VkPhysicalDevice physical_device,
                  VkBufferUsageFlags usage) {
    device_ = device;
    physical_device_ = physical_device;
    usage_ = usage;
  }

  void Destroy() noexcept {
    for (auto& block : blocks_) {
      DestroyBufferRaw(device_, block.buffer);
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
        return {range, false};
      }
    }
    const auto block_bytes = std::max(kMinArenaBlockBytes, aligned);
    Block block;
    block.buffer = CreateBufferRaw(device_, physical_device_, block_bytes,
                                   usage_ | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (block_bytes > aligned) {
      block.free_spans.push_back({aligned, block_bytes - aligned});
    }
    blocks_.push_back(std::move(block));
    return {{static_cast<std::uint32_t>(blocks_.size() - 1U), 0, aligned}, true};
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
  }

  [[nodiscard]] VkBuffer buffer(std::uint32_t block) const noexcept {
    return blocks_[block].buffer.handle;
  }

  [[nodiscard]] std::uint32_t block_count() const noexcept {
    return static_cast<std::uint32_t>(blocks_.size());
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

  VkDevice device_{};
  VkPhysicalDevice physical_device_{};
  VkBufferUsageFlags usage_{};
  std::vector<Block> blocks_;
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

  void Initialize(VkDevice device, VkPhysicalDevice physical_device) {
    device_ = device;
    physical_device_ = physical_device;
  }

  void Destroy() noexcept {
    if (buffer_.memory != VK_NULL_HANDLE) {
      vkUnmapMemory(device_, buffer_.memory);
    }
    DestroyBufferRaw(device_, buffer_);
    mapped_ = nullptr;
    regions_.clear();
    pending_ = {};
  }

  // Reserves one region for the current frame. `retired` receives the previous
  // buffer when the ring grew; the caller must defer its destruction until
  // in-flight frames complete.
  Reservation Reserve(VkDeviceSize bytes, Buffer& retired, bool& grew) {
    if (pending_.active) {
      throw std::logic_error("staging region is already reserved this frame");
    }
    const auto aligned = AlignUp(bytes, kArenaAlignment);
    VkDeviceSize offset{};
    if (!TryPlace(aligned, offset)) {
      retired = buffer_;
      if (retired.memory != VK_NULL_HANDLE) {
        vkUnmapMemory(device_, retired.memory);
      }
      grew = true;
      buffer_ = {};
      mapped_ = nullptr;
      regions_.clear();
      head_ = 0;
      const auto capacity =
          std::max({kMinStagingBytes, aligned, retired.size * 2U});
      buffer_ = CreateBufferRaw(device_, physical_device_, capacity,
                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      void* mapped{};
      Check(vkMapMemory(device_, buffer_.memory, 0, buffer_.size, 0, &mapped),
            "map staging ring");
      mapped_ = static_cast<std::byte*>(mapped);
      offset = 0;
    }
    pending_ = {true, offset, offset + aligned};
    head_ = offset + aligned;
    return {mapped_ + offset, buffer_.handle, offset};
  }

  void FinishFrame(std::uint64_t completion_value) {
    if (!pending_.active) {
      return;
    }
    regions_.push_back({pending_.begin, pending_.end, completion_value});
    pending_ = {};
  }

  void Collect(std::uint64_t completed) {
    while (!regions_.empty() && regions_.front().completion <= completed) {
      regions_.erase(regions_.begin());
    }
    if (regions_.empty() && !pending_.active) {
      head_ = 0;
    }
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

  VkDevice device_{};
  VkPhysicalDevice physical_device_{};
  Buffer buffer_;
  std::byte* mapped_{};
  VkDeviceSize head_{};
  std::vector<Region> regions_;
  PendingRegion pending_;
};

}  // namespace

class Renderer::Impl {
 public:
  explicit Impl(RendererOptions options) {
    if (options.frames_in_flight < 2 || options.frames_in_flight > 8) {
      throw std::invalid_argument("frames_in_flight must be between 2 and 8");
    }
    CreateDevice(options);
    CreateFrameContexts(options.frames_in_flight);
    statistics_.frame_context_count = options.frames_in_flight;
    vertex_arena_.Initialize(device_, physical_device_,
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    index_arena_.Initialize(device_, physical_device_,
                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    staging_.Initialize(device_, physical_device_);
  }

  ~Impl() {
    if (device_ != VK_NULL_HANDLE) {
      (void)vkDeviceWaitIdle(device_);
      for (auto& retired : deferred_) {
        DestroyBuffer(retired.buffer);
      }
      deferred_.clear();
      retired_ranges_.clear();
      staging_.Destroy();
      vertex_arena_.Destroy();
      index_arena_.Destroy();
      DestroyTarget();
      for (auto& frame : frames_) {
        if (frame.fence != VK_NULL_HANDLE) {
          vkDestroyFence(device_, frame.fence, nullptr);
        }
        if (frame.command_pool != VK_NULL_HANDLE) {
          vkDestroyCommandPool(device_, frame.command_pool, nullptr);
        }
      }
      if (timeline_semaphore_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, timeline_semaphore_, nullptr);
      }
      vkDestroyDevice(device_, nullptr);
    }
    if (debug_messenger_ != VK_NULL_HANDLE) {
      const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
          vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
      if (destroy != nullptr) {
        destroy(instance_, debug_messenger_, nullptr);
      }
    }
    if (instance_ != VK_NULL_HANDLE) {
      vkDestroyInstance(instance_, nullptr);
    }
  }

  RenderResult Render(const extraction::FrameSnapshot& snapshot,
                      std::uint32_t width, std::uint32_t height,
                      const ShaderPaths& shaders) {
    const auto backend_start = CpuClock::now();
    frame_counters_ = {};
    for (const auto& draw : snapshot.draws) {
      ++frame_counters_.draw_count;
      frame_counters_.triangle_count +=
          snapshot.geometries[draw.geometry_index].indices->size() / 3U;
    }

    ValidateExtent(width, height);
    auto& frame = BeginFrame();
    EnsureTarget(width, height, shaders);

    const auto upload_start = CpuClock::now();
    SyncGeometry(snapshot);
    const auto upload_ns = ElapsedNanoseconds(upload_start);

    const auto recording_start = CpuClock::now();
    Check(vkResetCommandPool(device_, frame.command_pool, 0),
          "reset frame command pool");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(frame.command_buffer, &begin),
          "begin frame command buffer");
    RecordUploads(frame.command_buffer);
    RecordFrame(frame.command_buffer, snapshot);
    Check(vkEndCommandBuffer(frame.command_buffer), "end frame command buffer");
    const auto recording_ns = ElapsedNanoseconds(recording_start);

    const auto readback_start = CpuClock::now();
    const auto completion = SubmitAndWait(frame);
    ++statistics_.frames_submitted;
    staging_.FinishFrame(completion);
    staging_.Collect(completion);
    CollectDeferred(completion);

    RenderResult result;
    result.color = ReadColor(width, height);
    result.depth = ReadDepth(width, height);
    result.prim_id = ReadId(width, height, Aov::PrimId,
                            target_.prim_id_readback);
    result.instance_id = ReadId(width, height, Aov::InstanceId,
                                target_.instance_id_readback);
    const auto readback_ns = ElapsedNanoseconds(readback_start);
    result.scene_revision = snapshot.revision;
    result.completion_value = completion;
    result.cpu_timings.upload_ns = upload_ns;
    result.cpu_timings.command_recording_ns = recording_ns;
    result.cpu_timings.readback_ns = readback_ns;
    result.cpu_timings.backend_total_ns = ElapsedNanoseconds(backend_start);
    result.counters = frame_counters_;
    return result;
  }

  struct FrameContext {
    VkCommandPool command_pool{};
    VkCommandBuffer command_buffer{};
    VkFence fence{};
    std::uint64_t completion_value{};
  };

  struct RetiredBuffer {
    Buffer buffer;
    std::uint64_t retire_value{};
  };

  // GPU residency for one mesh, keyed by the serialized RenderWorld handle
  // (slot index + generation) with per-sub-resource revisions. A revision
  // mismatch re-uploads only the stale sub-resource; a size-preserving edit
  // reuses the existing range in place.
  struct GeometrySlot {
    std::uint64_t points_revision{};
    std::uint64_t topology_revision{};
    BufferRange vertices;
    BufferRange indices;
    std::uint32_t index_count{};
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
    VkPipeline pipeline{};
    Buffer color_readback;
    Buffer depth_readback;
    Buffer prim_id_readback;
    Buffer instance_id_readback;
    ShaderPaths shaders;
  };

  void CreateDevice(const RendererOptions& options) {
    constexpr const char* validation_layer = "VK_LAYER_KHRONOS_validation";
    const bool use_validation =
        options.enable_validation && HasLayer(validation_layer);
    const std::vector<const char*> layers = use_validation
        ? std::vector<const char*>{validation_layer}
        : std::vector<const char*>{};
    const bool use_debug_utils =
        use_validation && HasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const std::vector<const char*> extensions = use_debug_utils
        ? std::vector<const char*>{VK_EXT_DEBUG_UTILS_EXTENSION_NAME}
        : std::vector<const char*>{};

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
      throw std::runtime_error(std::string("Vulkan ") +
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
      throw std::runtime_error("no Vulkan physical device is available");
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
      for (std::uint32_t index = 0; index < queue_count; ++index) {
        if ((queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
          physical_device_ = candidate;
          queue_family_ = index;
          selected_queue_flags = queues[index].queueFlags;
          break;
        }
      }
      if (physical_device_ != VK_NULL_HANDLE) {
        break;
      }
    }
    if (physical_device_ == VK_NULL_HANDLE) {
      throw std::runtime_error(std::string("no Vulkan ") +
                               MERLIN_VULKAN_MIN_VERSION_STRING +
                               " physical device with a graphics queue is available");
    }

    VkPhysicalDeviceDriverProperties driver_properties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
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
    capabilities_.validation_enabled = use_validation;
    capabilities_.graphics_queue = true;
    capabilities_.compute_queue =
        (selected_queue_flags & VK_QUEUE_COMPUTE_BIT) != 0U;
    capabilities_.transfer_queue =
        (selected_queue_flags & VK_QUEUE_TRANSFER_BIT) != 0U;

    VkFormatProperties depth_properties{};
    vkGetPhysicalDeviceFormatProperties(physical_device_, kDepthFormat,
                                        &depth_properties);
    const auto required_depth = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if ((depth_properties.optimalTilingFeatures & required_depth) != required_depth) {
      throw std::runtime_error("D32 depth attachment readback is unsupported");
    }

    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &timeline;
    vkGetPhysicalDeviceFeatures2(physical_device_, &features);
    capabilities_.timeline_semaphore = timeline.timelineSemaphore == VK_TRUE;

    const float priority = 1.0F;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = queue_family_;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = capabilities_.timeline_semaphore ? &timeline : nullptr;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    Check(vkCreateDevice(physical_device_, &device_info, nullptr, &device_),
          "create Vulkan device");
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    if (capabilities_.timeline_semaphore) {
      VkSemaphoreTypeCreateInfo type_info{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
      type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
      type_info.initialValue = 0;
      VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      semaphore_info.pNext = &type_info;
      Check(vkCreateSemaphore(device_, &semaphore_info, nullptr,
                              &timeline_semaphore_),
            "create frame timeline semaphore");
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
      if (timeline_semaphore_ == VK_NULL_HANDLE) {
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        Check(vkCreateFence(device_, &fence_info, nullptr, &frame.fence),
              "create frame fence");
      }
    }
  }

  Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags properties) {
    ++frame_counters_.allocation_count;
    ++frame_counters_.buffer_allocation_count;
    return CreateBufferRaw(device_, physical_device_, size, usage, properties);
  }

  void DestroyBuffer(Buffer& buffer) noexcept {
    DestroyBufferRaw(device_, buffer);
  }

  // Reconciles GPU geometry residency with one immutable snapshot: retires
  // slots whose mesh left the snapshot, then stages only the sub-resources
  // whose revision changed. Transform-, visibility-, and material-only edits
  // reach this function with every record clean and stage zero bytes.
  void SyncGeometry(const extraction::FrameSnapshot& snapshot) {
    bool structural_change = false;
    auto record = snapshot.geometries.begin();
    for (auto slot = geometry_slots_.begin(); slot != geometry_slots_.end();) {
      while (record != snapshot.geometries.end() &&
             record->mesh < slot->first) {
        ++record;
      }
      if (record == snapshot.geometries.end() || record->mesh != slot->first) {
        ReleaseRange(vertex_arena_, slot->second.vertices);
        ReleaseRange(index_arena_, slot->second.indices);
        slot = geometry_slots_.erase(slot);
        structural_change = true;
      } else {
        ++slot;
      }
    }

    struct Upload {
      const extraction::GeometryRecord* record{};
      GeometrySlot* slot{};
      bool vertices{};
      bool indices{};
      VkDeviceSize vertex_bytes{};
      VkDeviceSize index_bytes{};
    };
    std::vector<Upload> uploads;
    VkDeviceSize staging_bytes{};
    for (const auto& geometry : snapshot.geometries) {
      const auto [entry, inserted] = geometry_slots_.try_emplace(geometry.mesh);
      auto& slot = entry->second;
      Upload upload{&geometry, &slot};
      upload.vertices =
          inserted || slot.points_revision != geometry.points_revision;
      upload.indices =
          inserted || slot.topology_revision != geometry.topology_revision;
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
        staging_bytes += AlignUp(upload.vertex_bytes, kArenaAlignment);
      }
      if (upload.indices) {
        staging_bytes += AlignUp(upload.index_bytes, kArenaAlignment);
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
      bool staging_grew{};
      reservation =
          staging_.Reserve(staging_bytes, retired_staging, staging_grew);
      if (staging_grew) {
        ++frame_counters_.allocation_count;
        ++frame_counters_.buffer_allocation_count;
        Retire(retired_staging);
      }
    }

    VkDeviceSize cursor{};
    auto stage = [&](const void* payload, VkDeviceSize bytes,
                     DeviceArena& arena, const BufferRange& range) {
      if (bytes == 0) {
        return;
      }
      std::memcpy(reservation.mapped + cursor, payload,
                  static_cast<std::size_t>(bytes));
      pending_copies_.push_back({reservation.buffer,
                                 reservation.offset + cursor,
                                 arena.buffer(range.block), range.offset,
                                 bytes});
      cursor += AlignUp(bytes, kArenaAlignment);
      frame_counters_.upload_bytes += bytes;
    };
    for (auto& upload : uploads) {
      auto& slot = *upload.slot;
      if (upload.vertices) {
        EnsureRange(vertex_arena_, slot.vertices, upload.vertex_bytes);
        stage(upload.record->vertices->data(), upload.vertex_bytes,
              vertex_arena_, slot.vertices);
        slot.points_revision = upload.record->points_revision;
      }
      if (upload.indices) {
        EnsureRange(index_arena_, slot.indices, upload.index_bytes);
        stage(upload.record->indices->data(), upload.index_bytes, index_arena_,
              slot.indices);
        slot.index_count =
            static_cast<std::uint32_t>(upload.record->indices->size());
        slot.topology_revision = upload.record->topology_revision;
      }
    }
    if (staging_bytes != 0) {
      ++statistics_.scene_uploads;
    }
  }

  void EnsureRange(DeviceArena& arena, BufferRange& range,
                   VkDeviceSize bytes) {
    if (bytes == 0) {
      ReleaseRange(arena, range);
      return;
    }
    const auto aligned = AlignUp(bytes, kArenaAlignment);
    if (range.valid() && range.size == aligned) {
      // Size-preserving edits update the existing range in place. Frames are
      // currently synchronized before reuse; asynchronous execution must
      // version ranges instead of writing over readable memory.
      return;
    }
    ReleaseRange(arena, range);
    const auto allocation = arena.Allocate(bytes);
    if (allocation.created_block) {
      ++frame_counters_.allocation_count;
      ++frame_counters_.buffer_allocation_count;
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

  void RecordUploads(VkCommandBuffer command) {
    if (pending_copies_.empty()) {
      return;
    }
    for (const auto& copy : pending_copies_) {
      const VkBufferCopy region{copy.source_offset, copy.destination_offset,
                                copy.size};
      vkCmdCopyBuffer(command, copy.source, copy.destination, 1, &region);
    }
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &barrier, 0,
                         nullptr, 0, nullptr);
    pending_copies_.clear();
  }

  void Retire(Buffer& buffer) {
    if (buffer.handle != VK_NULL_HANDLE) {
      deferred_.push_back({buffer, timeline_value_});
      buffer = {};
    }
  }

  void CollectDeferred(std::uint64_t completed) {
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
        ++statistics_.geometry_range_retirements;
        range = retired_ranges_.erase(range);
      } else {
        ++range;
      }
    }
  }

  void ValidateExtent(std::uint32_t width, std::uint32_t height) const {
    if (width == 0 || height == 0 ||
        width > capabilities_.max_image_dimension_2d ||
        height > capabilities_.max_image_dimension_2d) {
      throw std::invalid_argument("offscreen extent is unsupported");
    }
  }

  FrameContext& BeginFrame() {
    auto& frame = frames_[next_frame_];
    next_frame_ = (next_frame_ + 1U) % frames_.size();
    if (frame.completion_value == 0) {
      return frame;
    }
    if (timeline_semaphore_ != VK_NULL_HANDLE) {
      VkSemaphoreWaitInfo wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
      wait_info.semaphoreCount = 1;
      wait_info.pSemaphores = &timeline_semaphore_;
      wait_info.pValues = &frame.completion_value;
      Check(vkWaitSemaphores(device_, &wait_info,
                             std::numeric_limits<std::uint64_t>::max()),
            "wait for reusable frame context");
    } else {
      Check(vkWaitForFences(device_, 1, &frame.fence, VK_TRUE,
                            std::numeric_limits<std::uint64_t>::max()),
            "wait for reusable frame fence");
      Check(vkResetFences(device_, 1, &frame.fence), "reset frame fence");
    }
    CollectDeferred(frame.completion_value);
    return frame;
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
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = FindMemoryTypeRaw(
        physical_device_, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    try {
      Check(vkAllocateMemory(device_, &allocation, nullptr, &memory),
            "allocate image memory");
      Check(vkBindImageMemory(device_, image, memory, 0), "bind image memory");
    } catch (...) {
      if (memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, memory, nullptr);
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

  void EnsureTarget(std::uint32_t width, std::uint32_t height,
                    const ShaderPaths& shaders) {
    if (target_.width == width && target_.height == height &&
        target_.shaders.vertex == shaders.vertex &&
        target_.shaders.fragment == shaders.fragment) {
      ++frame_counters_.pipeline_cache_hits;
      return;
    }
    ++frame_counters_.pipeline_cache_misses;
    Check(vkDeviceWaitIdle(device_), "wait before resizing render products");
    DestroyTarget();
    CreateTarget(width, height, shaders);
  }

  void CreateTarget(std::uint32_t width, std::uint32_t height,
                    const ShaderPaths& shaders) {
    target_.width = width;
    target_.height = height;
    target_.shaders = shaders;
    try {
      target_.color = CreateImage(width, height, kColorFormat,
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
          target_.color_memory);
      target_.color_view = CreateImageView(target_.color, kColorFormat,
                                           VK_IMAGE_ASPECT_COLOR_BIT);
      target_.depth = CreateImage(width, height, kDepthFormat,
          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
              VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
          target_.depth_memory);
      target_.depth_view = CreateImageView(target_.depth, kDepthFormat,
                                           VK_IMAGE_ASPECT_DEPTH_BIT);
      target_.prim_id = CreateImage(
          width, height, kIdFormat,
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
          target_.prim_id_memory);
      target_.prim_id_view = CreateImageView(
          target_.prim_id, kIdFormat, VK_IMAGE_ASPECT_COLOR_BIT);
      target_.instance_id = CreateImage(
          width, height, kIdFormat,
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
          target_.instance_id_memory);
      target_.instance_id_view = CreateImageView(
          target_.instance_id, kIdFormat, VK_IMAGE_ASPECT_COLOR_BIT);
      CreateRenderPass();
      const std::array<VkImageView, 4> views{
          target_.color_view, target_.depth_view, target_.prim_id_view,
          target_.instance_id_view};
      VkFramebufferCreateInfo framebuffer_info{
          VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      framebuffer_info.renderPass = target_.render_pass;
      framebuffer_info.attachmentCount = static_cast<std::uint32_t>(views.size());
      framebuffer_info.pAttachments = views.data();
      framebuffer_info.width = width;
      framebuffer_info.height = height;
      framebuffer_info.layers = 1;
      Check(vkCreateFramebuffer(device_, &framebuffer_info, nullptr,
                                &target_.framebuffer),
            "create framebuffer");
      const auto color_bytes = static_cast<VkDeviceSize>(width) * height * 4U;
      const auto depth_bytes = static_cast<VkDeviceSize>(width) * height *
                               sizeof(float);
      target_.color_readback = CreateBuffer(
          color_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      target_.depth_readback = CreateBuffer(
          depth_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      target_.prim_id_readback = CreateBuffer(
          depth_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      target_.instance_id_readback = CreateBuffer(
          depth_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      CreatePipeline(shaders);
    } catch (...) {
      DestroyTarget();
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
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount =
        static_cast<std::uint32_t>(color_references.size());
    subpass.pColorAttachments = color_references.data();
    subpass.pDepthStencilAttachment = &depth_reference;
    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = static_cast<std::uint32_t>(dependencies.size());
    info.pDependencies = dependencies.data();
    Check(vkCreateRenderPass(device_, &info, nullptr, &target_.render_pass),
          "create color/depth render pass");
  }

  void CreatePipeline(const ShaderPaths& shaders) {
    ++frame_counters_.pipeline_creation_count;
    const auto vertex_code = ReadSpirv(shaders.vertex);
    const auto fragment_code = ReadSpirv(shaders.fragment);
    VkShaderModule vertex_shader{};
    VkShaderModule fragment_shader{};
    auto create_shader = [&](const std::vector<std::uint32_t>& code,
                             VkShaderModule& module) {
      VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      info.codeSize = code.size() * sizeof(std::uint32_t);
      info.pCode = code.data();
      Check(vkCreateShaderModule(device_, &info, nullptr, &module),
            "create shader module");
    };
    try {
      create_shader(vertex_code, vertex_shader);
      create_shader(fragment_code, fragment_shader);
      const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
           VK_SHADER_STAGE_VERTEX_BIT, vertex_shader, "main", nullptr},
          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
           VK_SHADER_STAGE_FRAGMENT_BIT, fragment_shader, "main", nullptr}}};
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
      raster.cullMode = VK_CULL_MODE_NONE;
      raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
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
      dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
      dynamic.pDynamicStates = dynamic_states.data();
      VkPushConstantRange push_range{};
      push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
                              VK_SHADER_STAGE_FRAGMENT_BIT;
      push_range.size = sizeof(PushConstants);
      VkPipelineLayoutCreateInfo layout_info{
          VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
      layout_info.pushConstantRangeCount = 1;
      layout_info.pPushConstantRanges = &push_range;
      Check(vkCreatePipelineLayout(device_, &layout_info, nullptr,
                                   &target_.pipeline_layout),
            "create pipeline layout");
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
      pipeline_info.layout = target_.pipeline_layout;
      pipeline_info.renderPass = target_.render_pass;
      Check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1,
                                      &pipeline_info, nullptr,
                                      &target_.pipeline),
            "create scene graphics pipeline");
    } catch (...) {
      if (fragment_shader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device_, fragment_shader, nullptr);
      }
      if (vertex_shader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device_, vertex_shader, nullptr);
      }
      throw;
    }
    vkDestroyShaderModule(device_, fragment_shader, nullptr);
    vkDestroyShaderModule(device_, vertex_shader, nullptr);
  }

  void DestroyTarget() noexcept {
    if (device_ == VK_NULL_HANDLE) {
      return;
    }
    DestroyBuffer(target_.instance_id_readback);
    DestroyBuffer(target_.prim_id_readback);
    DestroyBuffer(target_.depth_readback);
    DestroyBuffer(target_.color_readback);
    if (target_.pipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, target_.pipeline, nullptr);
    }
    if (target_.pipeline_layout != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(device_, target_.pipeline_layout, nullptr);
    }
    if (target_.framebuffer != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(device_, target_.framebuffer, nullptr);
    }
    if (target_.render_pass != VK_NULL_HANDLE) {
      vkDestroyRenderPass(device_, target_.render_pass, nullptr);
    }
    if (target_.instance_id_view != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, target_.instance_id_view, nullptr);
    }
    if (target_.instance_id != VK_NULL_HANDLE) {
      vkDestroyImage(device_, target_.instance_id, nullptr);
    }
    if (target_.instance_id_memory != VK_NULL_HANDLE) {
      vkFreeMemory(device_, target_.instance_id_memory, nullptr);
    }
    if (target_.prim_id_view != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, target_.prim_id_view, nullptr);
    }
    if (target_.prim_id != VK_NULL_HANDLE) {
      vkDestroyImage(device_, target_.prim_id, nullptr);
    }
    if (target_.prim_id_memory != VK_NULL_HANDLE) {
      vkFreeMemory(device_, target_.prim_id_memory, nullptr);
    }
    if (target_.depth_view != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, target_.depth_view, nullptr);
    }
    if (target_.depth != VK_NULL_HANDLE) {
      vkDestroyImage(device_, target_.depth, nullptr);
    }
    if (target_.depth_memory != VK_NULL_HANDLE) {
      vkFreeMemory(device_, target_.depth_memory, nullptr);
    }
    if (target_.color_view != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, target_.color_view, nullptr);
    }
    if (target_.color != VK_NULL_HANDLE) {
      vkDestroyImage(device_, target_.color, nullptr);
    }
    if (target_.color_memory != VK_NULL_HANDLE) {
      vkFreeMemory(device_, target_.color_memory, nullptr);
    }
    target_ = {};
  }

  void RecordFrame(VkCommandBuffer command,
                   const extraction::FrameSnapshot& snapshot) {
    std::array<VkClearValue, 4> clear{};
    clear[0].color = {{0.018F, 0.025F, 0.045F, 1.0F}};
    clear[1].depthStencil = {1.0F, 0};
    clear[2].color.uint32[0] = std::numeric_limits<std::uint32_t>::max();
    clear[3].color.uint32[0] = std::numeric_limits<std::uint32_t>::max();
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = target_.render_pass;
    pass.framebuffer = target_.framebuffer;
    pass.renderArea = {{0, 0}, {target_.width, target_.height}};
    pass.clearValueCount = static_cast<std::uint32_t>(clear.size());
    pass.pClearValues = clear.data();
    vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      target_.pipeline);
    const VkViewport viewport{0.0F, 0.0F, static_cast<float>(target_.width),
                              static_cast<float>(target_.height), 0.0F, 1.0F};
    const VkRect2D scissor{{0, 0}, {target_.width, target_.height}};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    const auto view_projection = Multiply(snapshot.projection, snapshot.view);
    for (const auto& draw : snapshot.draws) {
      const auto& geometry = snapshot.geometries[draw.geometry_index];
      const auto& slot = geometry_slots_.at(geometry.mesh);
      const auto vertex_buffer = vertex_arena_.buffer(slot.vertices.block);
      vkCmdBindVertexBuffers(command, 0, 1, &vertex_buffer,
                             &slot.vertices.offset);
      vkCmdBindIndexBuffer(command, index_arena_.buffer(slot.indices.block),
                           slot.indices.offset, VK_INDEX_TYPE_UINT32);
      const auto& instance = snapshot.instances[draw.instance_index];
      const auto& material = snapshot.materials[draw.material_index];
      PushConstants push{Multiply(view_projection, instance.transform),
                         material.base_color,
                         static_cast<std::uint32_t>(geometry.mesh),
                         static_cast<std::uint32_t>(instance.instance)};
      vkCmdPushConstants(command, target_.pipeline_layout,
                         VK_SHADER_STAGE_VERTEX_BIT |
                             VK_SHADER_STAGE_FRAGMENT_BIT,
                         0, sizeof(push), &push);
      vkCmdDrawIndexed(command, slot.index_count, 1, 0, 0, 0);
    }
    vkCmdEndRenderPass(command);

    VkBufferImageCopy color_copy{};
    color_copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    color_copy.imageSubresource.layerCount = 1;
    color_copy.imageExtent = {target_.width, target_.height, 1};
    vkCmdCopyImageToBuffer(command, target_.color,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           target_.color_readback.handle, 1, &color_copy);
    VkBufferImageCopy depth_copy{};
    depth_copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depth_copy.imageSubresource.layerCount = 1;
    depth_copy.imageExtent = {target_.width, target_.height, 1};
    vkCmdCopyImageToBuffer(command, target_.depth,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           target_.depth_readback.handle, 1, &depth_copy);
    VkBufferImageCopy id_copy{};
    id_copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    id_copy.imageSubresource.layerCount = 1;
    id_copy.imageExtent = {target_.width, target_.height, 1};
    vkCmdCopyImageToBuffer(command, target_.prim_id,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           target_.prim_id_readback.handle, 1, &id_copy);
    vkCmdCopyImageToBuffer(command, target_.instance_id,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           target_.instance_id_readback.handle, 1, &id_copy);
  }

  std::uint64_t SubmitAndWait(FrameContext& frame) {
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.command_buffer;
    std::uint64_t completion{};
    if (timeline_semaphore_ != VK_NULL_HANDLE) {
      completion = ++timeline_value_;
      VkTimelineSemaphoreSubmitInfo timeline{
          VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
      timeline.signalSemaphoreValueCount = 1;
      timeline.pSignalSemaphoreValues = &completion;
      submit.pNext = &timeline;
      submit.signalSemaphoreCount = 1;
      submit.pSignalSemaphores = &timeline_semaphore_;
      Check(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE),
            "submit extracted scene frame");
      VkSemaphoreWaitInfo wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
      wait_info.semaphoreCount = 1;
      wait_info.pSemaphores = &timeline_semaphore_;
      wait_info.pValues = &completion;
      Check(vkWaitSemaphores(device_, &wait_info,
                             std::numeric_limits<std::uint64_t>::max()),
            "wait for render product readback");
    } else {
      completion = ++timeline_value_;
      Check(vkQueueSubmit(queue_, 1, &submit, frame.fence),
            "submit extracted scene frame");
      Check(vkWaitForFences(device_, 1, &frame.fence, VK_TRUE,
                            std::numeric_limits<std::uint64_t>::max()),
            "wait for render product readback");
    }
    frame.completion_value = completion;
    return completion;
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
    Check(vkMapMemory(device_, target_.color_readback.memory, 0,
                      target_.color_readback.size, 0, &mapped),
          "map color readback");
    std::memcpy(result.pixels.data(), mapped, result.pixels.size());
    vkUnmapMemory(device_, target_.color_readback.memory);
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
    Check(vkMapMemory(device_, target_.depth_readback.memory, 0,
                      target_.depth_readback.size, 0, &mapped),
          "map depth readback");
    std::memcpy(result.pixels.data(), mapped,
                result.pixels.size() * sizeof(float));
    vkUnmapMemory(device_, target_.depth_readback.memory);
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
    Check(vkMapMemory(device_, readback.memory, 0, readback.size, 0, &mapped),
          "map id readback");
    std::memcpy(result.pixels.data(), mapped,
                result.pixels.size() * sizeof(std::uint32_t));
    vkUnmapMemory(device_, readback.memory);
    return result;
  }

  static VKAPI_ATTR VkBool32 VKAPI_CALL ValidationCallback(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT type,
      const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
      void* user_data) {
    (void)severity;
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
    return VK_FALSE;
  }

  VkInstance instance_{};
  VkPhysicalDevice physical_device_{};
  VkDevice device_{};
  VkQueue queue_{};
  VkDebugUtilsMessengerEXT debug_messenger_{};
  VkSemaphore timeline_semaphore_{};
  std::uint32_t queue_family_{};
  std::uint64_t timeline_value_{};
  std::size_t next_frame_{};
  RendererCapabilities capabilities_;
  RendererStatistics statistics_;
  FrameCounters frame_counters_;
  std::vector<FrameContext> frames_;
  std::vector<RetiredBuffer> deferred_;
  DeviceArena vertex_arena_;
  DeviceArena index_arena_;
  StagingRing staging_;
  std::map<std::uint64_t, GeometrySlot> geometry_slots_;
  std::vector<RetiredRange> retired_ranges_;
  std::vector<PendingCopy> pending_copies_;
  RenderTarget target_;
  std::atomic<std::uint64_t> validation_messages_{};
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
  auto result = impl_->statistics_;
  result.validation_messages =
      impl_->validation_messages_.load(std::memory_order_relaxed);
  result.pending_geometry_retirements =
      static_cast<std::uint32_t>(impl_->retired_ranges_.size());
  result.geometry_arena_blocks =
      impl_->vertex_arena_.block_count() + impl_->index_arena_.block_count();
  return result;
}

void ValidateRenderResult(const RenderResult& result) {
  const auto& color = result.color;
  const auto& depth = result.depth;
  const auto& prim_id = result.prim_id;
  const auto& instance_id = result.instance_id;
  if (!IsCanonicalRenderProduct(color.product) ||
      color.product.aov != Aov::Color) {
    throw std::invalid_argument("invalid color render product metadata");
  }
  if (!IsCanonicalRenderProduct(depth.product) ||
      depth.product.aov != Aov::Depth) {
    throw std::invalid_argument("invalid depth render product metadata");
  }
  if (!IsCanonicalRenderProduct(prim_id.product) ||
      prim_id.product.aov != Aov::PrimId ||
      !IsCanonicalRenderProduct(instance_id.product) ||
      instance_id.product.aov != Aov::InstanceId) {
    throw std::invalid_argument("invalid id render product metadata");
  }
  if (color.product.width != depth.product.width ||
      color.product.height != depth.product.height ||
      color.product.width != prim_id.product.width ||
      color.product.height != prim_id.product.height ||
      color.product.width != instance_id.product.width ||
      color.product.height != instance_id.product.height) {
    throw std::invalid_argument("render product extents do not match");
  }
  if (color.row_pitch_bytes != TightRowPitchBytes(color.product) ||
      depth.row_pitch_bytes != TightRowPitchBytes(depth.product) ||
      prim_id.row_pitch_bytes != TightRowPitchBytes(prim_id.product) ||
      instance_id.row_pitch_bytes != TightRowPitchBytes(instance_id.product)) {
    throw std::invalid_argument("render product row pitch is not tight");
  }
  const auto color_bytes = static_cast<std::uint64_t>(color.row_pitch_bytes) *
                           color.product.height;
  const auto depth_values =
      static_cast<std::uint64_t>(depth.product.width) * depth.product.height;
  if (color_bytes != color.pixels.size() ||
      depth_values != depth.pixels.size() ||
      depth_values != prim_id.pixels.size() ||
      depth_values != instance_id.pixels.size()) {
    throw std::invalid_argument("render product payload size is invalid");
  }
  if (std::any_of(depth.pixels.begin(), depth.pixels.end(), [](float value) {
        return !std::isfinite(value) || value < 0.0F || value > 1.0F;
      })) {
    throw std::invalid_argument("depth render product contains invalid values");
  }
  if (result.completion_value == 0) {
    throw std::invalid_argument("render result has no completion value");
  }
}

RenderResult Renderer::Render(const extraction::FrameSnapshot& snapshot,
                              std::uint32_t width, std::uint32_t height,
                              const ShaderPaths& shaders) {
  auto result = impl_->Render(snapshot, width, height, shaders);
  ValidateRenderResult(result);
  return result;
}

}  // namespace merlin::vulkan
