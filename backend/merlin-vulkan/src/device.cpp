#include "renderer_impl.hpp"

namespace merlin::vulkan {

void Renderer::Impl::AdoptBorrowedContext(const RendererOptions& options) {
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
  max_compute_work_group_count_y_ =
      properties.properties.limits.maxComputeWorkGroupCount[1];
  max_per_stage_descriptor_storage_buffers_ =
      properties.properties.limits.maxPerStageDescriptorStorageBuffers;
  max_descriptor_set_storage_buffers_ =
      properties.properties.limits.maxDescriptorSetStorageBuffers;
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

void Renderer::Impl::CreateDevice(const RendererOptions& options) {
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
  max_compute_work_group_count_y_ =
      properties.properties.limits.maxComputeWorkGroupCount[1];
  max_per_stage_descriptor_storage_buffers_ =
      properties.properties.limits.maxPerStageDescriptorStorageBuffers;
  max_descriptor_set_storage_buffers_ =
      properties.properties.limits.maxDescriptorSetStorageBuffers;
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

VKAPI_ATTR VkBool32 VKAPI_CALL Renderer::Impl::ValidationCallback(
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

} // namespace merlin::vulkan
