#include <merlin/vulkan/renderer.hpp>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace merlin::vulkan {
namespace {

constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

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
  }

  ~Impl() {
    if (device_ != VK_NULL_HANDLE) {
      (void)vkDeviceWaitIdle(device_);
      for (auto& retired : deferred_) {
        DestroyBuffer(retired.buffer);
      }
      deferred_.clear();
      DestroyBuffer(scene_vertices_);
      DestroyBuffer(scene_indices_);
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

  RenderResult Render(const extraction::ExtractedScene& scene,
                      std::uint32_t width, std::uint32_t height,
                      const ShaderPaths& shaders) {
    ValidateExtent(width, height);
    auto& frame = BeginFrame();
    EnsureTarget(width, height, shaders);
    UploadScene(scene);

    Check(vkResetCommandPool(device_, frame.command_pool, 0),
          "reset frame command pool");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(frame.command_buffer, &begin),
          "begin frame command buffer");
    RecordFrame(frame.command_buffer, scene);
    Check(vkEndCommandBuffer(frame.command_buffer), "end frame command buffer");

    const auto completion = SubmitAndWait(frame);
    ++statistics_.frames_submitted;
    CollectDeferred(completion);

    RenderResult result;
    result.color = ReadColor(width, height);
    result.depth = ReadDepth(width, height);
    result.scene_revision = scene.revision;
    result.completion_value = completion;
    return result;
  }

  struct Buffer {
    VkBuffer handle{};
    VkDeviceMemory memory{};
    VkDeviceSize size{};
  };

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

  struct RenderTarget {
    std::uint32_t width{};
    std::uint32_t height{};
    VkImage color{};
    VkDeviceMemory color_memory{};
    VkImageView color_view{};
    VkImage depth{};
    VkDeviceMemory depth_memory{};
    VkImageView depth_view{};
    VkRenderPass render_pass{};
    VkFramebuffer framebuffer{};
    VkPipelineLayout pipeline_layout{};
    VkPipeline pipeline{};
    Buffer color_readback;
    Buffer depth_readback;
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
    app_info.apiVersion = VK_API_VERSION_1_2;

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
      throw std::runtime_error("no Vulkan graphics queue is available");
    }

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_device_, &properties);
    capabilities_.device_name = properties.deviceName;
    capabilities_.api_version = properties.apiVersion;
    capabilities_.max_image_dimension_2d = properties.limits.maxImageDimension2D;
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

  std::uint32_t FindMemoryType(std::uint32_t bits,
                               VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory);
    for (std::uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
      if ((bits & (1U << index)) != 0U &&
          (memory.memoryTypes[index].propertyFlags & properties) == properties) {
        return index;
      }
    }
    throw std::runtime_error("no compatible Vulkan memory type");
  }

  Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags properties) {
    Buffer result;
    result.size = size;
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    Check(vkCreateBuffer(device_, &buffer_info, nullptr, &result.handle),
          "create buffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, result.handle, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits,
                                                 properties);
    try {
      Check(vkAllocateMemory(device_, &allocation, nullptr, &result.memory),
            "allocate buffer memory");
      Check(vkBindBufferMemory(device_, result.handle, result.memory, 0),
            "bind buffer memory");
    } catch (...) {
      DestroyBuffer(result);
      throw;
    }
    return result;
  }

  void DestroyBuffer(Buffer& buffer) noexcept {
    if (buffer.handle != VK_NULL_HANDLE) {
      vkDestroyBuffer(device_, buffer.handle, nullptr);
    }
    if (buffer.memory != VK_NULL_HANDLE) {
      vkFreeMemory(device_, buffer.memory, nullptr);
    }
    buffer = {};
  }

  template <typename T>
  Buffer CreateUploadedBuffer(const std::vector<T>& values,
                              VkBufferUsageFlags usage) {
    if (values.empty()) {
      return {};
    }
    const auto bytes = static_cast<VkDeviceSize>(values.size() * sizeof(T));
    auto buffer = CreateBuffer(bytes, usage,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* mapped{};
    try {
      Check(vkMapMemory(device_, buffer.memory, 0, bytes, 0, &mapped),
            "map scene buffer");
      std::memcpy(mapped, values.data(), static_cast<std::size_t>(bytes));
      vkUnmapMemory(device_, buffer.memory);
    } catch (...) {
      if (mapped != nullptr) {
        vkUnmapMemory(device_, buffer.memory);
      }
      DestroyBuffer(buffer);
      throw;
    }
    return buffer;
  }

  void UploadScene(const extraction::ExtractedScene& scene) {
    if (uploaded_scene_ == &scene && uploaded_revision_ == scene.revision) {
      return;
    }
    auto vertices = CreateUploadedBuffer(scene.vertices,
                                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    Buffer indices;
    try {
      indices = CreateUploadedBuffer(scene.indices,
                                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    } catch (...) {
      DestroyBuffer(vertices);
      throw;
    }
    Retire(scene_vertices_);
    Retire(scene_indices_);
    scene_vertices_ = vertices;
    scene_indices_ = indices;
    uploaded_revision_ = scene.revision;
    uploaded_scene_ = &scene;
    ++statistics_.scene_uploads;
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
    allocation.memoryTypeIndex = FindMemoryType(
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
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
      return;
    }
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
      CreateRenderPass();
      const std::array<VkImageView, 2> views{target_.color_view,
                                             target_.depth_view};
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
      CreatePipeline(shaders);
    } catch (...) {
      DestroyTarget();
      throw;
    }
  }

  void CreateRenderPass() {
    std::array<VkAttachmentDescription, 2> attachments{};
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
    const VkAttachmentReference color_reference{
        0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depth_reference{
        1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_reference;
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
      const VkVertexInputAttributeDescription attribute{
          0, 0, VK_FORMAT_R32G32B32_SFLOAT,
          static_cast<std::uint32_t>(offsetof(extraction::DrawVertex, position))};
      VkPipelineVertexInputStateCreateInfo vertex_input{
          VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      vertex_input.vertexBindingDescriptionCount = 1;
      vertex_input.pVertexBindingDescriptions = &binding;
      vertex_input.vertexAttributeDescriptionCount = 1;
      vertex_input.pVertexAttributeDescriptions = &attribute;
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
      VkPipelineColorBlendStateCreateInfo blend{
          VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
      blend.attachmentCount = 1;
      blend.pAttachments = &blend_attachment;
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
                   const extraction::ExtractedScene& scene) {
    std::array<VkClearValue, 2> clear{};
    clear[0].color = {{0.018F, 0.025F, 0.045F, 1.0F}};
    clear[1].depthStencil = {1.0F, 0};
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
    if (scene_vertices_.handle != VK_NULL_HANDLE &&
        scene_indices_.handle != VK_NULL_HANDLE) {
      const VkDeviceSize offset = 0;
      vkCmdBindVertexBuffers(command, 0, 1, &scene_vertices_.handle, &offset);
      vkCmdBindIndexBuffer(command, scene_indices_.handle, 0,
                           VK_INDEX_TYPE_UINT32);
      const auto view_projection = Multiply(scene.projection, scene.view);
      for (const auto& draw : scene.draws) {
        PushConstants push{Multiply(view_projection, draw.transform),
                           draw.base_color};
        vkCmdPushConstants(command, target_.pipeline_layout,
                           VK_SHADER_STAGE_VERTEX_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), &push);
        vkCmdDrawIndexed(command, draw.index_count, 1, draw.first_index,
                         draw.vertex_offset, 0);
      }
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
    ImageRgba8 result{width, height,
                      std::vector<std::uint8_t>(
                          static_cast<std::size_t>(width) * height * 4U)};
    void* mapped{};
    Check(vkMapMemory(device_, target_.color_readback.memory, 0,
                      target_.color_readback.size, 0, &mapped),
          "map color readback");
    std::memcpy(result.pixels.data(), mapped, result.pixels.size());
    vkUnmapMemory(device_, target_.color_readback.memory);
    return result;
  }

  ImageDepth32 ReadDepth(std::uint32_t width, std::uint32_t height) {
    ImageDepth32 result{width, height,
                        std::vector<float>(static_cast<std::size_t>(width) *
                                           height)};
    void* mapped{};
    Check(vkMapMemory(device_, target_.depth_readback.memory, 0,
                      target_.depth_readback.size, 0, &mapped),
          "map depth readback");
    std::memcpy(result.pixels.data(), mapped,
                result.pixels.size() * sizeof(float));
    vkUnmapMemory(device_, target_.depth_readback.memory);
    return result;
  }

  static VKAPI_ATTR VkBool32 VKAPI_CALL ValidationCallback(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT type,
      const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
      void* user_data) {
    (void)severity;
    (void)type;
    auto* self = static_cast<Impl*>(user_data);
    self->validation_messages_.fetch_add(1, std::memory_order_relaxed);
    std::cerr << "Merlin Vulkan validation: "
              << (callback_data != nullptr && callback_data->pMessage != nullptr
                      ? callback_data->pMessage
                      : "unknown validation message")
              << '\n';
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
  std::uint64_t uploaded_revision_{std::numeric_limits<std::uint64_t>::max()};
  const extraction::ExtractedScene* uploaded_scene_{};
  DeviceCapabilities capabilities_;
  RendererStatistics statistics_;
  std::vector<FrameContext> frames_;
  std::vector<RetiredBuffer> deferred_;
  Buffer scene_vertices_;
  Buffer scene_indices_;
  RenderTarget target_;
  std::atomic<std::uint64_t> validation_messages_{};
};

Renderer::Renderer(RendererOptions options)
    : impl_(std::make_unique<Impl>(options)) {}
Renderer::~Renderer() = default;
Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;

const DeviceCapabilities& Renderer::capabilities() const noexcept {
  return impl_->capabilities_;
}

RendererStatistics Renderer::statistics() const noexcept {
  auto result = impl_->statistics_;
  result.validation_messages =
      impl_->validation_messages_.load(std::memory_order_relaxed);
  return result;
}

RenderResult Renderer::Render(const extraction::ExtractedScene& scene,
                              std::uint32_t width, std::uint32_t height,
                              const ShaderPaths& shaders) {
  return impl_->Render(scene, width, height, shaders);
}

}  // namespace merlin::vulkan
