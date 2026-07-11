#include <merlin/vulkan/renderer.hpp>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace merlin::vulkan {
namespace {

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
  std::vector<std::uint32_t> code(static_cast<std::size_t>(end) / sizeof(std::uint32_t));
  stream.seekg(0);
  stream.read(reinterpret_cast<char*>(code.data()), end);
  if (!stream) {
    throw std::runtime_error("could not read SPIR-V file: " + path.string());
  }
  return code;
}

bool HasLayer(const char* name) {
  std::uint32_t count{};
  Check(vkEnumerateInstanceLayerProperties(&count, nullptr), "enumerate instance layers");
  std::vector<VkLayerProperties> layers(count);
  Check(vkEnumerateInstanceLayerProperties(&count, layers.data()), "enumerate instance layers");
  return std::any_of(layers.begin(), layers.end(), [name](const auto& layer) {
    return std::strcmp(layer.layerName, name) == 0;
  });
}

}  // namespace

class Renderer::Impl {
 public:
  explicit Impl(RendererOptions options) {
    constexpr const char* validation_layer = "VK_LAYER_KHRONOS_validation";
    const bool use_validation = options.enable_validation && HasLayer(validation_layer);
    const std::vector<const char*> layers = use_validation
        ? std::vector<const char*>{validation_layer}
        : std::vector<const char*>{};

    VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app_info.pApplicationName = "hdMerlin";
    app_info.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app_info.pEngineName = "Merlin";
    app_info.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app_info.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app_info;
    instance_info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    instance_info.ppEnabledLayerNames = layers.data();
    Check(vkCreateInstance(&instance_info, nullptr, &instance_), "create Vulkan instance");

    std::uint32_t device_count{};
    Check(vkEnumeratePhysicalDevices(instance_, &device_count, nullptr),
          "enumerate physical devices");
    if (device_count == 0) {
      throw std::runtime_error("no Vulkan physical device is available");
    }
    std::vector<VkPhysicalDevice> devices(device_count);
    Check(vkEnumeratePhysicalDevices(instance_, &device_count, devices.data()),
          "enumerate physical devices");

    for (auto candidate : devices) {
      std::uint32_t queue_count{};
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count, nullptr);
      std::vector<VkQueueFamilyProperties> queues(queue_count);
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count, queues.data());
      for (std::uint32_t i = 0; i < queue_count; ++i) {
        if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
          physical_device_ = candidate;
          queue_family_ = i;
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
      Check(vkCreateSemaphore(device_, &semaphore_info, nullptr, &timeline_semaphore_),
            "create frame timeline semaphore");
    }
  }

  ~Impl() {
    if (device_ != VK_NULL_HANDLE) {
      vkDeviceWaitIdle(device_);
      if (timeline_semaphore_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, timeline_semaphore_, nullptr);
      }
      vkDestroyDevice(device_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
      vkDestroyInstance(instance_, nullptr);
    }
  }

  std::uint32_t FindMemoryType(std::uint32_t bits, VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory);
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
      if ((bits & (1U << i)) != 0U &&
          (memory.memoryTypes[i].propertyFlags & properties) == properties) {
        return i;
      }
    }
    throw std::runtime_error("no compatible Vulkan memory type");
  }

  ImageRgba8 RenderTriangle(std::uint32_t width, std::uint32_t height,
                            const ShaderPaths& shaders) {
    if (width == 0 || height == 0 ||
        width > capabilities_.max_image_dimension_2d ||
        height > capabilities_.max_image_dimension_2d) {
      throw std::invalid_argument("offscreen extent is unsupported");
    }

    const auto vertex_code = ReadSpirv(shaders.vertex);
    const auto fragment_code = ReadSpirv(shaders.fragment);

    VkImage image{};
    VkDeviceMemory image_memory{};
    VkImageView image_view{};
    VkRenderPass render_pass{};
    VkFramebuffer framebuffer{};
    VkShaderModule vertex_shader{};
    VkShaderModule fragment_shader{};
    VkPipelineLayout pipeline_layout{};
    VkPipeline pipeline{};
    VkBuffer readback{};
    VkDeviceMemory readback_memory{};
    VkCommandPool command_pool{};
    VkFence fence{};

    auto cleanup = [&] {
      if (fence) vkDestroyFence(device_, fence, nullptr);
      if (command_pool) vkDestroyCommandPool(device_, command_pool, nullptr);
      if (readback) vkDestroyBuffer(device_, readback, nullptr);
      if (readback_memory) vkFreeMemory(device_, readback_memory, nullptr);
      if (pipeline) vkDestroyPipeline(device_, pipeline, nullptr);
      if (pipeline_layout) vkDestroyPipelineLayout(device_, pipeline_layout, nullptr);
      if (vertex_shader) vkDestroyShaderModule(device_, vertex_shader, nullptr);
      if (fragment_shader) vkDestroyShaderModule(device_, fragment_shader, nullptr);
      if (framebuffer) vkDestroyFramebuffer(device_, framebuffer, nullptr);
      if (render_pass) vkDestroyRenderPass(device_, render_pass, nullptr);
      if (image_view) vkDestroyImageView(device_, image_view, nullptr);
      if (image) vkDestroyImage(device_, image, nullptr);
      if (image_memory) vkFreeMemory(device_, image_memory, nullptr);
    };

    try {
      VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      image_info.imageType = VK_IMAGE_TYPE_2D;
      image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
      image_info.extent = {width, height, 1};
      image_info.mipLevels = 1;
      image_info.arrayLayers = 1;
      image_info.samples = VK_SAMPLE_COUNT_1_BIT;
      image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
      image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      Check(vkCreateImage(device_, &image_info, nullptr, &image), "create offscreen image");

      VkMemoryRequirements image_requirements{};
      vkGetImageMemoryRequirements(device_, image, &image_requirements);
      VkMemoryAllocateInfo image_alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      image_alloc.allocationSize = image_requirements.size;
      image_alloc.memoryTypeIndex = FindMemoryType(
          image_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      Check(vkAllocateMemory(device_, &image_alloc, nullptr, &image_memory),
            "allocate offscreen memory");
      Check(vkBindImageMemory(device_, image, image_memory, 0), "bind offscreen memory");

      VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view_info.image = image;
      view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
      view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      view_info.subresourceRange.levelCount = 1;
      view_info.subresourceRange.layerCount = 1;
      Check(vkCreateImageView(device_, &view_info, nullptr, &image_view),
            "create offscreen image view");

      VkAttachmentDescription attachment{};
      attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
      attachment.samples = VK_SAMPLE_COUNT_1_BIT;
      attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      VkAttachmentReference color_reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      VkSubpassDescription subpass{};
      subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      subpass.colorAttachmentCount = 1;
      subpass.pColorAttachments = &color_reference;
      VkSubpassDependency dependency{};
      dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
      dependency.dstSubpass = 0;
      dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      VkRenderPassCreateInfo render_pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
      render_pass_info.attachmentCount = 1;
      render_pass_info.pAttachments = &attachment;
      render_pass_info.subpassCount = 1;
      render_pass_info.pSubpasses = &subpass;
      render_pass_info.dependencyCount = 1;
      render_pass_info.pDependencies = &dependency;
      Check(vkCreateRenderPass(device_, &render_pass_info, nullptr, &render_pass),
            "create render pass");

      VkFramebufferCreateInfo framebuffer_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      framebuffer_info.renderPass = render_pass;
      framebuffer_info.attachmentCount = 1;
      framebuffer_info.pAttachments = &image_view;
      framebuffer_info.width = width;
      framebuffer_info.height = height;
      framebuffer_info.layers = 1;
      Check(vkCreateFramebuffer(device_, &framebuffer_info, nullptr, &framebuffer),
            "create framebuffer");

      auto create_shader = [&](const std::vector<std::uint32_t>& code, VkShaderModule* module) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = code.size() * sizeof(std::uint32_t);
        info.pCode = code.data();
        Check(vkCreateShaderModule(device_, &info, nullptr, module), "create shader module");
      };
      create_shader(vertex_code, &vertex_shader);
      create_shader(fragment_code, &fragment_shader);

      const VkPipelineShaderStageCreateInfo stages[] = {
          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
           VK_SHADER_STAGE_VERTEX_BIT, vertex_shader, "main", nullptr},
          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
           VK_SHADER_STAGE_FRAGMENT_BIT, fragment_shader, "main", nullptr}};
      VkPipelineVertexInputStateCreateInfo vertex_input{
          VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      VkPipelineInputAssemblyStateCreateInfo input_assembly{
          VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
      input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      VkViewport viewport{0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height),
                          0.0F, 1.0F};
      VkRect2D scissor{{0, 0}, {width, height}};
      VkPipelineViewportStateCreateInfo viewport_state{
          VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
      viewport_state.viewportCount = 1;
      viewport_state.pViewports = &viewport;
      viewport_state.scissorCount = 1;
      viewport_state.pScissors = &scissor;
      VkPipelineRasterizationStateCreateInfo raster{
          VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
      raster.polygonMode = VK_POLYGON_MODE_FILL;
      raster.cullMode = VK_CULL_MODE_NONE;
      raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
      raster.lineWidth = 1.0F;
      VkPipelineMultisampleStateCreateInfo multisample{
          VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
      multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
      VkPipelineColorBlendAttachmentState blend_attachment{};
      blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
      VkPipelineColorBlendStateCreateInfo blend{
          VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
      blend.attachmentCount = 1;
      blend.pAttachments = &blend_attachment;
      VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
      Check(vkCreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout),
            "create pipeline layout");
      VkGraphicsPipelineCreateInfo pipeline_info{
          VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
      pipeline_info.stageCount = 2;
      pipeline_info.pStages = stages;
      pipeline_info.pVertexInputState = &vertex_input;
      pipeline_info.pInputAssemblyState = &input_assembly;
      pipeline_info.pViewportState = &viewport_state;
      pipeline_info.pRasterizationState = &raster;
      pipeline_info.pMultisampleState = &multisample;
      pipeline_info.pColorBlendState = &blend;
      pipeline_info.layout = pipeline_layout;
      pipeline_info.renderPass = render_pass;
      Check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info,
                                      nullptr, &pipeline),
            "create graphics pipeline");

      const VkDeviceSize byte_size = static_cast<VkDeviceSize>(width) * height * 4U;
      VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      buffer_info.size = byte_size;
      buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      Check(vkCreateBuffer(device_, &buffer_info, nullptr, &readback),
            "create readback buffer");
      VkMemoryRequirements buffer_requirements{};
      vkGetBufferMemoryRequirements(device_, readback, &buffer_requirements);
      VkMemoryAllocateInfo buffer_alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      buffer_alloc.allocationSize = buffer_requirements.size;
      buffer_alloc.memoryTypeIndex = FindMemoryType(
          buffer_requirements.memoryTypeBits,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      Check(vkAllocateMemory(device_, &buffer_alloc, nullptr, &readback_memory),
            "allocate readback memory");
      Check(vkBindBufferMemory(device_, readback, readback_memory, 0),
            "bind readback memory");

      VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      pool_info.queueFamilyIndex = queue_family_;
      Check(vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool),
            "create command pool");
      VkCommandBufferAllocateInfo command_alloc{
          VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      command_alloc.commandPool = command_pool;
      command_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      command_alloc.commandBufferCount = 1;
      VkCommandBuffer command{};
      Check(vkAllocateCommandBuffers(device_, &command_alloc, &command),
            "allocate command buffer");
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      Check(vkBeginCommandBuffer(command, &begin), "begin command buffer");
      VkClearValue clear{};
      clear.color = {{0.018F, 0.025F, 0.045F, 1.0F}};
      VkRenderPassBeginInfo pass_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      pass_begin.renderPass = render_pass;
      pass_begin.framebuffer = framebuffer;
      pass_begin.renderArea = {{0, 0}, {width, height}};
      pass_begin.clearValueCount = 1;
      pass_begin.pClearValues = &clear;
      vkCmdBeginRenderPass(command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      vkCmdDraw(command, 3, 1, 0, 0);
      vkCmdEndRenderPass(command);
      VkBufferImageCopy copy{};
      copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      copy.imageSubresource.layerCount = 1;
      copy.imageExtent = {width, height, 1};
      vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             readback, 1, &copy);
      Check(vkEndCommandBuffer(command), "end command buffer");

      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submit.commandBufferCount = 1;
      submit.pCommandBuffers = &command;
      if (timeline_semaphore_ != VK_NULL_HANDLE) {
        const auto signal_value = ++timeline_value_;
        VkTimelineSemaphoreSubmitInfo timeline_submit{
            VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timeline_submit.signalSemaphoreValueCount = 1;
        timeline_submit.pSignalSemaphoreValues = &signal_value;
        submit.pNext = &timeline_submit;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &timeline_semaphore_;
        Check(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE),
              "submit offscreen frame");
        VkSemaphoreWaitInfo wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wait_info.semaphoreCount = 1;
        wait_info.pSemaphores = &timeline_semaphore_;
        wait_info.pValues = &signal_value;
        Check(vkWaitSemaphores(device_, &wait_info,
                               std::numeric_limits<std::uint64_t>::max()),
              "wait for frame timeline");
      } else {
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        Check(vkCreateFence(device_, &fence_info, nullptr, &fence),
              "create render fence");
        Check(vkQueueSubmit(queue_, 1, &submit, fence), "submit offscreen frame");
        Check(vkWaitForFences(device_, 1, &fence, VK_TRUE,
                              std::numeric_limits<std::uint64_t>::max()),
              "wait for offscreen frame");
      }

      ImageRgba8 result{width, height,
                        std::vector<std::uint8_t>(static_cast<std::size_t>(byte_size))};
      void* mapped{};
      Check(vkMapMemory(device_, readback_memory, 0, byte_size, 0, &mapped),
            "map readback memory");
      std::memcpy(result.pixels.data(), mapped, result.pixels.size());
      vkUnmapMemory(device_, readback_memory);
      cleanup();
      return result;
    } catch (...) {
      cleanup();
      throw;
    }
  }

  VkInstance instance_{};
  VkPhysicalDevice physical_device_{};
  VkDevice device_{};
  VkQueue queue_{};
  VkSemaphore timeline_semaphore_{};
  std::uint64_t timeline_value_{};
  std::uint32_t queue_family_{};
  DeviceCapabilities capabilities_;
};

Renderer::Renderer(RendererOptions options) : impl_(std::make_unique<Impl>(options)) {}
Renderer::~Renderer() = default;
Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;

const DeviceCapabilities& Renderer::capabilities() const noexcept {
  return impl_->capabilities_;
}

ImageRgba8 Renderer::RenderTriangle(std::uint32_t width, std::uint32_t height,
                                    const ShaderPaths& shaders) {
  return impl_->RenderTriangle(width, height, shaders);
}

}  // namespace merlin::vulkan
