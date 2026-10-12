#include "renderer_impl.hpp"

namespace merlin::vulkan {

VkImage Renderer::Impl::CreateImage(std::uint32_t width, std::uint32_t height, VkFormat format,
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

VkImageView Renderer::Impl::CreateImageView(VkImage image, VkFormat format,
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

void Renderer::Impl::EnsureTarget(RenderTarget& target, std::uint32_t width,
    std::uint32_t height,
    const ShaderPaths& shaders,
    const std::vector<Aov>& cpu_readback_aovs,
    bool gaussian_tile_raster_images, bool gaussian_float_color) {
  if (target.width == width && target.height == height &&
      target.shaders == shaders &&
      target.cpu_readback_aovs == cpu_readback_aovs &&
      target.gaussian_tile_raster_images == gaussian_tile_raster_images &&
      target.gaussian_float_color == gaussian_float_color) {
    ++frame_counters_.pipeline_cache_hits;
    return;
  }
  ++frame_counters_.pipeline_cache_misses;
  DestroyTarget(target);
  CreateTarget(width, height, shaders, cpu_readback_aovs,
      gaussian_tile_raster_images, gaussian_float_color);
}

VkImageUsageFlags Renderer::Impl::TargetImageUsage(Aov aov,
    bool gaussian_tile_raster_images) {
  const VkImageUsageFlags tile_raster_usage =
      gaussian_tile_raster_images
          ? (aov == Aov::Depth ? VK_IMAGE_USAGE_SAMPLED_BIT
                               : VK_IMAGE_USAGE_STORAGE_BIT)
          : 0U;
  switch (aov) {
  case Aov::Color:
    return VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
           tile_raster_usage;
  case Aov::Depth:
    return VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | tile_raster_usage;
  default:
    return VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | tile_raster_usage;
  }
}

void Renderer::Impl::CreateTarget(std::uint32_t width, std::uint32_t height,
    const ShaderPaths& shaders,
    const std::vector<Aov>& cpu_readback_aovs,
    bool gaussian_tile_raster_images, bool gaussian_float_color) {
  active_target_->width = width;
  active_target_->height = height;
  active_target_->shaders = shaders;
  active_target_->cpu_readback_aovs = cpu_readback_aovs;
  active_target_->gaussian_tile_raster_images = gaussian_tile_raster_images;
  active_target_->gaussian_float_color = gaussian_float_color;
  try {
    active_target_->color = CreateImage(
        width, height, kColorFormat,
        TargetImageUsage(Aov::Color, false) |
            (gaussian_float_color ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0U),
        active_target_->color_memory);
    active_target_->color_view =
        CreateImageView(active_target_->color, kColorFormat,
            VK_IMAGE_ASPECT_COLOR_BIT);
    if (gaussian_float_color) {
      active_target_->gaussian_color = CreateImage(width, height,
          kGaussianColorFormat,
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
              (gaussian_tile_raster_images ? VK_IMAGE_USAGE_STORAGE_BIT : 0U),
          active_target_->gaussian_color_memory);
      active_target_->gaussian_color_view = CreateImageView(
          active_target_->gaussian_color, kGaussianColorFormat,
          VK_IMAGE_ASPECT_COLOR_BIT);
    }
    active_target_->depth = CreateImage(
        width, height, kDepthFormat,
        TargetImageUsage(Aov::Depth, gaussian_tile_raster_images),
        active_target_->depth_memory);
    active_target_->depth_view =
        CreateImageView(active_target_->depth, kDepthFormat,
            VK_IMAGE_ASPECT_DEPTH_BIT);
    active_target_->prim_id = CreateImage(
        width, height, kIdFormat,
        TargetImageUsage(Aov::PrimId, gaussian_tile_raster_images),
        active_target_->prim_id_memory);
    active_target_->prim_id_view = CreateImageView(
        active_target_->prim_id, kIdFormat, VK_IMAGE_ASPECT_COLOR_BIT);
    active_target_->instance_id = CreateImage(
        width, height, kIdFormat,
        TargetImageUsage(Aov::InstanceId, gaussian_tile_raster_images),
        active_target_->instance_id_memory);
    active_target_->instance_id_view = CreateImageView(
        active_target_->instance_id, kIdFormat, VK_IMAGE_ASPECT_COLOR_BIT);
    CreateRenderPass();
    const std::array<VkImageView, 4> views{
        gaussian_float_color ? active_target_->gaussian_color_view
                             : active_target_->color_view,
        active_target_->depth_view,
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
        // Uncached coherent memory can make the CPU memcpy dominate a frame.
        // Prefer cached memory, but keep coherence required so the existing
        // completion wait and map/read path need no cache invalidation.
        buffer = CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
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

void Renderer::Impl::CreateRenderPass() {
  std::array<VkAttachmentDescription, 4> attachments{};
  attachments[0].format = active_target_->gaussian_float_color
                              ? kGaussianColorFormat
                              : kColorFormat;
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

void Renderer::Impl::DestroyTarget(RenderTarget& target) noexcept {
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
  if (target.gaussian_sorted_pipeline != VK_NULL_HANDLE) {
    vkDestroyPipeline(device_, target.gaussian_sorted_pipeline, nullptr);
    target.gaussian_sorted_pipeline = VK_NULL_HANDLE;
  }
  if (target.gaussian_sorted_id_pipeline != VK_NULL_HANDLE) {
    vkDestroyPipeline(device_, target.gaussian_sorted_id_pipeline, nullptr);
    target.gaussian_sorted_id_pipeline = VK_NULL_HANDLE;
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
  if (target.gaussian_color_view != VK_NULL_HANDLE) {
    vkDestroyImageView(device_, target.gaussian_color_view, nullptr);
  }
  if (target.gaussian_color != VK_NULL_HANDLE) {
    vkDestroyImage(device_, target.gaussian_color, nullptr);
  }
  if (target.gaussian_color_memory != VK_NULL_HANDLE) {
    memory_budget_.Free(target.gaussian_color_memory);
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

} // namespace merlin::vulkan
