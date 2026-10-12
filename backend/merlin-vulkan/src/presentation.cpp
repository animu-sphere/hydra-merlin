#include "renderer_impl.hpp"

namespace merlin::vulkan {

void Renderer::Impl::DestroySwapchain() noexcept {
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

PresentationOverlayContext Renderer::Impl::MakePresentationOverlayContext(
    PresentationOverlayPhase phase,
    VkCommandBuffer command) const noexcept {
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

void Renderer::Impl::InitializePresentationOverlay() {
  if (presentation_overlay_ == nullptr) {
    return;
  }
  presentation_overlay_(
      presentation_overlay_user_data_,
      MakePresentationOverlayContext(PresentationOverlayPhase::Initialize));
  presentation_overlay_initialized_ = true;
}

void Renderer::Impl::ShutdownPresentationOverlay() noexcept {
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

VkSurfaceFormatKHR Renderer::Impl::SelectSurfaceFormat() const {
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

VkPresentModeKHR Renderer::Impl::SelectPresentMode() const {
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

void Renderer::Impl::CreateSwapchain(std::uint32_t requested_width,
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

void Renderer::Impl::EnsureSwapchain(std::uint32_t width, std::uint32_t height) {
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

void Renderer::Impl::PreparePresentation(FrameContext& frame, std::uint32_t width,
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

void Renderer::Impl::RecordPresentation(VkCommandBuffer command,
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

void Renderer::Impl::PresentFrame(const FrameContext& frame) {
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
void Renderer::Impl::ReclaimAcquiredImage(FrameContext& frame) noexcept {
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

} // namespace merlin::vulkan
