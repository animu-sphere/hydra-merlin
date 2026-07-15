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

std::string_view RendererErrorCodeName(RendererErrorCode code) noexcept {
  switch (code) {
    case RendererErrorCode::InvalidRequest: return "invalid-request";
    case RendererErrorCode::InvalidToken: return "invalid-token";
    case RendererErrorCode::ResourceBusy: return "resource-busy";
    case RendererErrorCode::Timeout: return "timeout";
    case RendererErrorCode::DeviceLost: return "device-lost";
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
      native_code_(native_code) {}

namespace {

using CpuClock = std::chrono::steady_clock;

std::atomic<std::uint64_t> g_renderer_owner{1};

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
  if (result == VK_SUCCESS) {
    return;
  }
  RendererErrorCode code = RendererErrorCode::BackendFailure;
  if (result == VK_TIMEOUT) {
    code = RendererErrorCode::Timeout;
  } else if (result == VK_ERROR_DEVICE_LOST) {
    code = RendererErrorCode::DeviceLost;
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

struct PushConstants {
  Mat4 model_view_projection;
  Vec4 normal_matrix_column0;
  Vec4 normal_matrix_column1;
  Vec4 normal_matrix_column2;
  std::uint32_t feature_mask{};
  std::uint32_t prim_id{};
  std::uint32_t instance_id{};
  std::uint32_t padding{};
};

static_assert(sizeof(PushConstants) == 128);
static_assert(offsetof(PushConstants, feature_mask) == 112);

struct MaterialUniforms {
  Vec4 base_color;
  Vec4 light_direction_intensity;
  Vec4 light_color_alpha_cutoff;
};

static_assert(sizeof(MaterialUniforms) == 48);

constexpr std::uint32_t kMaskedAlphaFlag = 1U << 28U;
constexpr std::uint32_t kDoubleSidedFlag = 1U << 29U;

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
    VkDeviceSize block_bytes{};
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
        return {range, false, 0};
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
    // RendererOptions caps frame contexts at eight, so this prevents metadata
    // allocation failure after a queue submission has already succeeded.
    regions_.reserve(8);
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
      throw RendererError(RendererErrorCode::InvalidRequest,
                          "create renderer",
                          "frames_in_flight must be between 2 and 8");
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
      for (auto& retired : retired_textures_) {
        DestroyTexture(retired.texture);
      }
      retired_textures_.clear();
      for (const auto& retired : retired_samplers_) {
        vkDestroySampler(device_, retired.sampler, nullptr);
      }
      retired_samplers_.clear();
      for (auto& [handle, texture] : texture_slots_) {
        (void)handle;
        DestroyTexture(texture);
      }
      texture_slots_.clear();
      for (const auto& [handle, sampler] : sampler_slots_) {
        (void)handle;
        vkDestroySampler(device_, sampler.sampler, nullptr);
      }
      sampler_slots_.clear();
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
      vertex_arena_.Destroy();
      index_arena_.Destroy();
      for (auto& frame : frames_) {
        DestroyTarget(frame.target);
        DestroyBuffer(frame.material_uniforms);
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
      }
      for (const auto& [path, module] : shader_modules_) {
        (void)path;
        vkDestroyShaderModule(device_, module, nullptr);
      }
      shader_modules_.clear();
      if (descriptor_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
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

  std::uint64_t Submit(const RenderRequest& request) {
    ValidateRequest(request);
    const auto backend_start = CpuClock::now();
    frame_counters_ = {};
    ResetAbandonedUploads();
    for (const auto& draw : request.snapshot->draws) {
      ++frame_counters_.draw_count;
      ++frame_counters_.visible_primitive_count;
      frame_counters_.triangle_count +=
          request.snapshot->geometries[draw.geometry_index].indices->size() / 3U;
    }

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
    EnsureDescriptorSetLayout();
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

    const auto upload_start = CpuClock::now();
    const auto resource_sync_mode =
        SelectResourceSyncMode(*request.snapshot);
    SyncGeometry(*request.snapshot, resource_sync_mode);
    SyncTextures(*request.snapshot, resource_sync_mode);
    SyncSamplers(*request.snapshot, resource_sync_mode);
    PrepareMaterialDescriptors(frame, *request.snapshot);
    if (frame_counters_.upload_bytes != 0) {
      ++statistics_.scene_uploads;
    }
    const auto upload_ns = ElapsedNanoseconds(upload_start);

    const auto recording_start = CpuClock::now();
    Check(vkResetCommandPool(device_, frame.command_pool, 0),
          "reset frame command pool");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(frame.command_buffer, &begin),
          "begin frame command buffer");
    if (frame.timestamp_pool != VK_NULL_HANDLE) {
      vkCmdResetQueryPool(frame.command_buffer, frame.timestamp_pool, 0, 2);
      vkCmdWriteTimestamp(frame.command_buffer,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          frame.timestamp_pool, 0);
    }
    RecordUploads(frame.command_buffer);
    RecordFrame(frame.command_buffer, frame, *request.snapshot,
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
    frame.cpu_timings.command_recording_ns = recording_ns;
    frame.counters = frame_counters_;
    const auto submission_start = CpuClock::now();
    const auto completion = SubmitFrame(frame);
    frame.cpu_timings.queue_submission_ns =
        ElapsedNanoseconds(submission_start);
    CommitTextureUploads();
    CommitResourceSnapshot(*request.snapshot);
    for (auto& buffer : frame_upload_buffers_) {
      deferred_.push_back({buffer, completion});
      buffer = {};
    }
    frame_upload_buffers_.clear();
    frame.cpu_timings.backend_total_ns = ElapsedNanoseconds(backend_start);
    staging_.FinishFrame(completion);
    frame.outstanding = true;
    ++statistics_.frames_submitted;
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

  RenderResult Resolve(std::uint64_t completion,
                       std::chrono::nanoseconds timeout) {
    auto& frame = FindFrame(completion);
    const auto resolve_start = CpuClock::now();
    const auto wait_start = CpuClock::now();
    WaitForFrame(frame, timeout);
    const auto wait_ns = ElapsedNanoseconds(wait_start);
    latest_completed_value_ = std::max(latest_completed_value_, completion);
    staging_.Collect(completion);
    CollectDeferred(completion);
    active_target_ = &frame.target;
    struct ResetActiveTarget {
      RenderTarget*& target;
      ~ResetActiveTarget() { target = nullptr; }
    } reset_active_target{active_target_};
    frame_counters_ = frame.counters;

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
    result.cpu_timings.backend_total_ns += ElapsedNanoseconds(resolve_start);
    ++frame_counters_.wait_count;
    ++frame_counters_.resolve_count;
    result.counters = frame_counters_;
    frame.outstanding = false;
    frame.counters = {};
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
  };

  struct SamplerSlot {
    std::uint64_t revision{};
    VkSampler sampler{};
  };

  struct RetiredTexture {
    TextureSlot texture;
    std::uint64_t retire_value{};
  };

  struct RetiredSampler {
    VkSampler sampler{};
    std::uint64_t retire_value{};
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
    Buffer color_readback;
    Buffer depth_readback;
    Buffer prim_id_readback;
    Buffer instance_id_readback;
    ShaderPaths shaders;
    std::vector<Aov> cpu_readback_aovs;
  };

  struct FrameContext {
    VkCommandPool command_pool{};
    VkCommandBuffer command_buffer{};
    VkFence fence{};
    VkQueryPool timestamp_pool{};
    std::uint64_t completion_value{};
    bool outstanding{};
    RenderTarget target;
    std::uint64_t scene_revision{};
    std::vector<Aov> rendered_aovs;
    std::vector<Aov> cpu_readback_aovs;
    FrameCpuTimings cpu_timings;
    FrameCounters counters;
    VkDescriptorPool descriptor_pool{};
    std::uint32_t descriptor_capacity{};
    std::vector<VkDescriptorSet> material_descriptor_sets;
    Buffer material_uniforms;
  };

  enum class ResourceSyncMode {
    Full,
    Incremental,
    Unchanged,
  };

  [[nodiscard]] ResourceSyncMode SelectResourceSyncMode(
      const extraction::FrameSnapshot& snapshot) const noexcept {
    if (snapshot.source_id == 0 ||
        snapshot.source_id != resident_snapshot_source_) {
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
      for (std::uint32_t index = 0; index < queue_count; ++index) {
        if ((queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
          physical_device_ = candidate;
          queue_family_ = index;
          selected_queue_flags = queues[index].queueFlags;
          selected_timestamp_valid_bits_ = queues[index].timestampValidBits;
          break;
        }
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
    uniform_buffer_alignment_ = std::max<VkDeviceSize>(
        16U, properties.properties.limits.minUniformBufferOffsetAlignment);
    capabilities_.validation_enabled = use_validation;
    capabilities_.graphics_queue = true;
    capabilities_.compute_queue =
        (selected_queue_flags & VK_QUEUE_COMPUTE_BIT) != 0U;
    capabilities_.transfer_queue =
        (selected_queue_flags & VK_QUEUE_TRANSFER_BIT) != 0U;
    capabilities_.timestamp_queries =
        properties.properties.limits.timestampPeriod > 0.0F &&
        selected_timestamp_valid_bits_ != 0U;
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
      if (capabilities_.timestamp_queries) {
        VkQueryPoolCreateInfo query_info{
            VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query_info.queryCount = 2;
        Check(vkCreateQueryPool(device_, &query_info, nullptr,
                                &frame.timestamp_pool),
              "create frame timestamp query pool");
      }
    }
  }

  Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags properties) {
    ++frame_counters_.allocation_count;
    ++frame_counters_.buffer_allocation_count;
    frame_counters_.buffer_allocation_bytes += size;
    return CreateBufferRaw(device_, physical_device_, size, usage, properties);
  }

  void DestroyBuffer(Buffer& buffer) noexcept {
    DestroyBufferRaw(device_, buffer);
  }

  void DestroyTexture(TextureSlot& texture) noexcept {
    if (texture.view != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, texture.view, nullptr);
    }
    if (texture.image != VK_NULL_HANDLE) {
      vkDestroyImage(device_, texture.image, nullptr);
    }
    if (texture.memory != VK_NULL_HANDLE) {
      vkFreeMemory(device_, texture.memory, nullptr);
    }
    texture = {};
  }

  void ResetAbandonedUploads() {
    pending_image_copies_.clear();
    for (const auto handle : pending_texture_handles_) {
      const auto texture = texture_slots_.find(handle);
      if (texture != texture_slots_.end() &&
          texture->second.pending_upload) {
        DestroyTexture(texture->second);
      }
    }
    pending_texture_handles_.clear();
    if (fallback_texture_.pending_upload) {
      DestroyTexture(fallback_texture_);
    }
    for (auto& buffer : frame_upload_buffers_) {
      DestroyBuffer(buffer);
    }
    frame_upload_buffers_.clear();
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

  void RetireTexture(TextureSlot& texture) {
    if (texture.image != VK_NULL_HANDLE) {
      retired_textures_.push_back({texture, timeline_value_});
      texture = {};
    }
  }

  void RetireSampler(SamplerSlot& sampler) {
    if (sampler.sampler != VK_NULL_HANDLE) {
      retired_samplers_.push_back({sampler.sampler, timeline_value_});
      sampler = {};
    }
  }

  void SyncTextures(const extraction::FrameSnapshot& snapshot,
                    ResourceSyncMode mode) {
    if (mode == ResourceSyncMode::Unchanged) {
      frame_counters_.texture_cache_hits += snapshot.textures.size();
      return;
    }

    std::vector<const extraction::TextureRecord*> records;
    if (mode == ResourceSyncMode::Full) {
      auto record = snapshot.textures.begin();
      for (auto slot = texture_slots_.begin(); slot != texture_slots_.end();) {
        while (record != snapshot.textures.end() &&
               record->texture < slot->first) {
          ++record;
        }
        if (record == snapshot.textures.end() ||
            record->texture != slot->first) {
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
      for (const auto handle : delta.upserts) {
        const auto record = std::lower_bound(
            snapshot.textures.begin(), snapshot.textures.end(), handle,
            [](const extraction::TextureRecord& texture,
               std::uint64_t value) { return texture.texture < value; });
        if (record == snapshot.textures.end() || record->texture != handle) {
          throw RendererError(RendererErrorCode::InvalidRequest,
                              "synchronize textures",
                              "snapshot texture delta has no matching record");
        }
        records.push_back(&*record);
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
      entry->second = CreateTextureResource(
          texture.width, texture.height, texture.revision, *texture.pixels);
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
    if (mode == ResourceSyncMode::Full) {
      auto record = snapshot.samplers.begin();
      for (auto slot = sampler_slots_.begin(); slot != sampler_slots_.end();) {
        while (record != snapshot.samplers.end() &&
               record->sampler < slot->first) {
          ++record;
        }
        if (record == snapshot.samplers.end() ||
            record->sampler != slot->first) {
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
      for (const auto handle : delta.upserts) {
        const auto record = std::lower_bound(
            snapshot.samplers.begin(), snapshot.samplers.end(), handle,
            [](const extraction::SamplerRecord& sampler,
               std::uint64_t value) { return sampler.sampler < value; });
        if (record == snapshot.samplers.end() || record->sampler != handle) {
          throw RendererError(RendererErrorCode::InvalidRequest,
                              "synchronize samplers",
                              "snapshot sampler delta has no matching record");
        }
        records.push_back(&*record);
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
      entry->second.revision = sampler.revision;
      entry->second.sampler = CreateSamplerResource(
          sampler.min_filter, sampler.mag_filter, sampler.address_u,
          sampler.address_v);
    }
  }

  void EnsureFallbackTextureAndSampler() {
    if (fallback_texture_.image == VK_NULL_HANDLE) {
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

  void EnsureDescriptorSetLayout() {
    if (descriptor_set_layout_ != VK_NULL_HANDLE) {
      ++frame_counters_.descriptor_layout_cache_hits;
      return;
    }
    ++frame_counters_.descriptor_layout_cache_misses;
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
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

  void PrepareMaterialDescriptors(
      FrameContext& frame, const extraction::FrameSnapshot& snapshot) {
    frame.material_descriptor_sets.clear();
    if (snapshot.materials.empty()) {
      return;
    }
    EnsureFallbackTextureAndSampler();
    const auto material_count =
        static_cast<std::uint32_t>(snapshot.materials.size());
    if (frame.descriptor_pool == VK_NULL_HANDLE ||
        frame.descriptor_capacity < material_count) {
      if (frame.descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, frame.descriptor_pool, nullptr);
        frame.descriptor_pool = VK_NULL_HANDLE;
        frame.descriptor_capacity = 0;
      }
      const std::array<VkDescriptorPoolSize, 2> sizes{{
          {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, material_count},
          {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, material_count},
      }};
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
      const auto& material = snapshot.materials[i];
      const MaterialUniforms uniforms{
          material.parameters.base_color,
          lighting.direction_intensity,
          {lighting.color.x, lighting.color.y, lighting.color.z,
           material.parameters.alpha_cutoff}};
      std::memcpy(static_cast<std::byte*>(mapped) + uniform_stride * i,
                  &uniforms, sizeof(uniforms));
    }
    vkUnmapMemory(device_, frame.material_uniforms.memory);

    std::vector<VkDescriptorSetLayout> layouts(material_count,
                                                descriptor_set_layout_);
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

    std::vector<VkDescriptorImageInfo> image_infos(material_count);
    std::vector<VkDescriptorBufferInfo> buffer_infos(material_count);
    std::vector<VkWriteDescriptorSet> writes(material_count * 2U);
    for (std::uint32_t i = 0; i < material_count; ++i) {
      auto image_view = fallback_texture_.view;
      auto sampler = fallback_sampler_.sampler;
      const auto& material = snapshot.materials[i];
      if (material.base_color_texture) {
        if (material.base_color_texture->texture_index >=
                snapshot.textures.size() ||
            material.base_color_texture->sampler_index >=
                snapshot.samplers.size()) {
          throw RendererError(RendererErrorCode::InvalidRequest,
                              "prepare material descriptors",
                              "material texture binding index is invalid");
        }
        const auto texture_handle =
            snapshot.textures[material.base_color_texture->texture_index]
                .texture;
        const auto sampler_handle =
            snapshot.samplers[material.base_color_texture->sampler_index]
                .sampler;
        image_view = texture_slots_.at(texture_handle).view;
        sampler = sampler_slots_.at(sampler_handle).sampler;
      }
      image_infos[i] =
          {sampler, image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      buffer_infos[i] = {frame.material_uniforms.handle, uniform_stride * i,
                         sizeof(MaterialUniforms)};
      auto& image_write = writes[i * 2U];
      image_write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      image_write.dstSet = frame.material_descriptor_sets[i];
      image_write.dstBinding = 0;
      image_write.descriptorCount = 1;
      image_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      image_write.pImageInfo = &image_infos[i];
      auto& buffer_write = writes[i * 2U + 1U];
      buffer_write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      buffer_write.dstSet = frame.material_descriptor_sets[i];
      buffer_write.dstBinding = 1;
      buffer_write.descriptorCount = 1;
      buffer_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      buffer_write.pBufferInfo = &buffer_infos[i];
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
    frame_counters_.descriptor_update_count += writes.size();
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
    if (mode == ResourceSyncMode::Full) {
      auto record = snapshot.geometries.begin();
      for (auto slot = geometry_slots_.begin(); slot != geometry_slots_.end();) {
        while (record != snapshot.geometries.end() &&
               record->mesh < slot->first) {
          ++record;
        }
        if (record == snapshot.geometries.end() ||
            record->mesh != slot->first) {
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
      for (const auto handle : delta.upserts) {
        const auto record = std::lower_bound(
            snapshot.geometries.begin(), snapshot.geometries.end(), handle,
            [](const extraction::GeometryRecord& geometry,
               std::uint64_t value) { return geometry.mesh < value; });
        if (record == snapshot.geometries.end() || record->mesh != handle) {
          throw RendererError(RendererErrorCode::InvalidRequest,
                              "synchronize geometry",
                              "snapshot geometry delta has no matching record");
        }
        records.push_back(&*record);
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
      bool staging_grew{};
      reservation =
          staging_.Reserve(staging_bytes, retired_staging, staging_grew);
      if (staging_grew) {
        ++frame_counters_.allocation_count;
        ++frame_counters_.buffer_allocation_count;
        frame_counters_.buffer_allocation_bytes += staging_bytes;
        Retire(retired_staging);
      }
    }

    VkDeviceSize cursor{};
    auto stage = [&](const void* payload, VkDeviceSize bytes,
                     DeviceArena& arena, const BufferRange& range,
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
                  vertex_arena_, slot.vertices, byte_offset);
          }
        } else {
          EnsureRange(vertex_arena_, slot.vertices, upload.vertex_bytes);
          stage(upload.record->vertices->data(), upload.vertex_bytes,
                vertex_arena_, slot.vertices);
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
                  index_arena_, slot.indices, byte_offset);
          }
        } else {
          EnsureRange(index_arena_, slot.indices, upload.index_bytes);
          stage(upload.record->indices->data(), upload.index_bytes,
                index_arena_, slot.indices);
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
      return;
    }
    ReleaseRange(arena, range);
    const auto allocation = arena.Allocate(bytes);
    if (allocation.created_block) {
      ++frame_counters_.allocation_count;
      ++frame_counters_.buffer_allocation_count;
      frame_counters_.buffer_allocation_bytes += allocation.block_bytes;
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
    if (pending_copies_.empty() && pending_image_copies_.empty()) {
      return;
    }
    for (const auto& copy : pending_copies_) {
      const VkBufferCopy region{copy.source_offset, copy.destination_offset,
                                copy.size};
      vkCmdCopyBuffer(command, copy.source, copy.destination, 1, &region);
    }
    if (!pending_copies_.empty()) {
      VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask =
          VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &barrier,
                           0, nullptr, 0, nullptr);
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
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = copy.destination;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        to_shader.push_back(barrier);
      }
      vkCmdPipelineBarrier(
          command, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
          static_cast<std::uint32_t>(to_shader.size()), to_shader.data());
      pending_image_copies_.clear();
    }
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
    ValidateExtent(request.width, request.height);
    if (request.shaders.vertex.empty() || request.shaders.fragment.empty()) {
      throw RendererError(RendererErrorCode::InvalidRequest,
                          "validate render request",
                          "vertex and fragment shader paths are required");
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
      return !frame.outstanding && frame.target.width == width &&
             frame.target.height == height &&
             frame.target.shaders.vertex == shaders.vertex &&
             frame.target.shaders.fragment == shaders.fragment &&
             frame.target.cpu_readback_aovs == cpu_readback_aovs;
    };
    auto found = std::find_if(frames_.begin(), frames_.end(), reusable);
    if (found == frames_.end()) {
      found = std::find_if(frames_.begin(), frames_.end(),
                           [](const FrameContext& frame) {
                             return !frame.outstanding;
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
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    frame_counters_.image_allocation_bytes += requirements.size;
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

  void EnsureTarget(RenderTarget& target, std::uint32_t width,
                    std::uint32_t height,
                    const ShaderPaths& shaders,
                    const std::vector<Aov>& cpu_readback_aovs) {
    if (target.width == width && target.height == height &&
        target.shaders.vertex == shaders.vertex &&
        target.shaders.fragment == shaders.fragment &&
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
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
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
      layout_info.setLayoutCount = 1;
      layout_info.pSetLayouts = &descriptor_set_layout_;
      layout_info.pushConstantRangeCount = 1;
      layout_info.pPushConstantRanges = &push_range;
      Check(vkCreatePipelineLayout(device_, &layout_info, nullptr,
                                   &active_target_->pipeline_layout),
            "create material pipeline layout");
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

  VkPipeline CreatePipeline(const ShaderPaths& shaders,
                            std::uint32_t variant_key) {
    ++frame_counters_.pipeline_creation_count;
    const auto vertex_shader = GetShaderModule(shaders.vertex);
    const auto fragment_shader = GetShaderModule(shaders.fragment);
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
    raster.cullMode = (variant_key & kDoubleSidedFlag) != 0U
                          ? VK_CULL_MODE_NONE
                          : VK_CULL_MODE_BACK_BIT;
    // Merlin snapshots use a conventional Y-up winding while the offscreen
    // Vulkan viewport has a positive height (framebuffer Y-down).
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
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
    pipeline_info.layout = active_target_->pipeline_layout;
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
      vkFreeMemory(device_, target.instance_id_memory, nullptr);
    }
    if (target.prim_id_view != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, target.prim_id_view, nullptr);
    }
    if (target.prim_id != VK_NULL_HANDLE) {
      vkDestroyImage(device_, target.prim_id, nullptr);
    }
    if (target.prim_id_memory != VK_NULL_HANDLE) {
      vkFreeMemory(device_, target.prim_id_memory, nullptr);
    }
    if (target.depth_view != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, target.depth_view, nullptr);
    }
    if (target.depth != VK_NULL_HANDLE) {
      vkDestroyImage(device_, target.depth, nullptr);
    }
    if (target.depth_memory != VK_NULL_HANDLE) {
      vkFreeMemory(device_, target.depth_memory, nullptr);
    }
    if (target.color_view != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, target.color_view, nullptr);
    }
    if (target.color != VK_NULL_HANDLE) {
      vkDestroyImage(device_, target.color, nullptr);
    }
    if (target.color_memory != VK_NULL_HANDLE) {
      vkFreeMemory(device_, target.color_memory, nullptr);
    }
    target = {};
  }

  void RecordFrame(VkCommandBuffer command,
                   const FrameContext& frame,
                   const extraction::FrameSnapshot& snapshot,
                   const std::vector<Aov>& cpu_readback_aovs) {
    std::array<VkClearValue, 4> clear{};
    clear[0].color = {{0.018F, 0.025F, 0.045F, 1.0F}};
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
      const auto pipeline = EnsurePipeline(active_target_->shaders, variant_key);
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      const auto descriptor_set =
          frame.material_descriptor_sets[draw.material_index];
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              active_target_->pipeline_layout, 0, 1,
                              &descriptor_set, 0, nullptr);
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
      push.prim_id = static_cast<std::uint32_t>(geometry.mesh);
      push.instance_id = static_cast<std::uint32_t>(instance.instance);
      vkCmdPushConstants(command, active_target_->pipeline_layout,
                         VK_SHADER_STAGE_VERTEX_BIT |
                             VK_SHADER_STAGE_FRAGMENT_BIT,
                         0, sizeof(push), &push);
      vkCmdDrawIndexed(command, slot.index_count, 1, 0, 0, 0);
    }
    vkCmdEndRenderPass(command);

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
  }

  std::uint64_t SubmitFrame(FrameContext& frame) {
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
    } else {
      completion = ++timeline_value_;
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

  std::uint64_t ReadGpuExecutionNanoseconds(const FrameContext& frame) const {
    if (frame.timestamp_pool == VK_NULL_HANDLE) {
      return 0;
    }
    std::array<std::uint64_t, 2> timestamps{};
    Check(vkGetQueryPoolResults(device_, frame.timestamp_pool, 0,
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
  std::uint32_t selected_timestamp_valid_bits_{};
  float timestamp_period_ns_{};
  std::uint64_t timeline_value_{};
  std::uint64_t latest_completed_value_{};
  VkDeviceSize uniform_buffer_alignment_{16U};
  const std::uint64_t owner_id_{
      g_renderer_owner.fetch_add(1, std::memory_order_relaxed)};
  RendererCapabilities capabilities_;
  RendererStatistics statistics_;
  FrameCounters frame_counters_;
  std::vector<FrameContext> frames_;
  std::vector<RetiredBuffer> deferred_;
  std::vector<RetiredTexture> retired_textures_;
  std::vector<RetiredSampler> retired_samplers_;
  DeviceArena vertex_arena_;
  DeviceArena index_arena_;
  StagingRing staging_;
  std::map<std::uint64_t, GeometrySlot> geometry_slots_;
  std::map<std::uint64_t, TextureSlot> texture_slots_;
  std::map<std::uint64_t, SamplerSlot> sampler_slots_;
  std::vector<RetiredRange> retired_ranges_;
  std::vector<PendingCopy> pending_copies_;
  std::vector<PendingImageCopy> pending_image_copies_;
  std::vector<std::uint64_t> pending_texture_handles_;
  std::vector<Buffer> frame_upload_buffers_;
  TextureSlot fallback_texture_;
  SamplerSlot fallback_sampler_;
  VkDescriptorSetLayout descriptor_set_layout_{};
  std::map<std::filesystem::path, VkShaderModule> shader_modules_;
  std::uint64_t resident_snapshot_source_{};
  std::uint64_t resident_snapshot_revision_{};
  RenderTarget* active_target_{};
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
