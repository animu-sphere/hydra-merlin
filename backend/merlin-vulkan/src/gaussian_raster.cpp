#include "renderer_impl.hpp"

namespace merlin::vulkan {

void Renderer::Impl::DestroyGaussianGpuRasterFrameResources(
    GaussianGpuRasterFrameResources& resources) noexcept {
  DestroyBuffer(resources.records);
  DestroyBuffer(resources.draw_arguments);
  DestroyBuffer(resources.draw_readback);
  if (resources.descriptor_pool != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(device_, resources.descriptor_pool, nullptr);
  }
  resources = {};
}

void Renderer::Impl::EnsureGaussianGpuRasterFrameResources(
    GaussianGpuRasterFrameResources& resources, VkDeviceSize record_bytes) {
  if (resources.record_capacity_bytes >= record_bytes &&
      resources.descriptor_pool != VK_NULL_HANDLE) {
    return;
  }
  DestroyGaussianGpuRasterFrameResources(resources);
  resources.record_capacity_bytes = record_bytes;
  resources.records = CreateBuffer(record_bytes,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  resources.draw_arguments = CreateBuffer(sizeof(VkDrawIndirectCommand),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
          VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  resources.draw_readback = CreateBuffer(sizeof(VkDrawIndirectCommand),
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  const VkDescriptorPoolSize pool_size{
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kGaussianSortStorageBufferCount};
  VkDescriptorPoolCreateInfo pool_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  Check(vkCreateDescriptorPool(device_, &pool_info, nullptr,
            &resources.descriptor_pool),
      "create Gaussian raster gather descriptor pool");
  ++frame_counters_.descriptor_pool_creation_count;
  VkDescriptorSetAllocateInfo allocate{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorPool = resources.descriptor_pool;
  allocate.descriptorSetCount = 1;
  allocate.pSetLayouts = &gaussian_sort_descriptor_set_layout_;
  Check(vkAllocateDescriptorSets(device_, &allocate,
            &resources.descriptor_set),
      "allocate Gaussian raster gather descriptor set");
  ++frame_counters_.descriptor_allocation_count;
}

// Rewritten every selected frame: the verified keys alternate between the
// two sort buffers with the pass count, and a recreated prepared-record
// buffer may reuse a destroyed handle's value.
void Renderer::Impl::UpdateGaussianGpuRasterDescriptors(
    const GaussianGpuRasterFrameResources& resources, VkBuffer sorted_keys,
    VkBuffer prepared_records) {
  const std::array<VkBuffer, kGaussianSortStorageBufferCount> buffers{
      sorted_keys,
      prepared_records,
      resources.records.handle,
      resources.draw_arguments.handle,
  };
  static constexpr std::array bindings{
      shader_abi::kGaussianRasterSorted.binding,
      shader_abi::kGaussianRasterPreparedRecords.binding,
      shader_abi::kGaussianRasterRecords.binding,
      shader_abi::kGaussianRasterDraw.binding,
  };
  static_assert(bindings.size() == kGaussianSortStorageBufferCount);
  std::array<VkDescriptorBufferInfo, kGaussianSortStorageBufferCount>
      buffer_infos{};
  std::array<VkWriteDescriptorSet, kGaussianSortStorageBufferCount>
      writes{};
  for (std::size_t i = 0; i < writes.size(); ++i) {
    buffer_infos[i] = {buffers[i], 0, VK_WHOLE_SIZE};
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[i].dstSet = resources.descriptor_set;
    writes[i].dstBinding = bindings[i];
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = &buffer_infos[i];
  }
  vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
      writes.data(), 0, nullptr);
  frame_counters_.descriptor_update_count += writes.size();
}

// Selecting sorted-stream raster replaces the CPU-sorted upload for this
// frame, so the choice is final before any prepared stream is written.
Renderer::Impl::GaussianGpuStageSelection Renderer::Impl::PrepareGaussianGpuRaster(FrameContext& frame,
    const RenderRequest& request, GaussianGpuStageSelection sort) {
  auto& resources = frame.gaussian_gpu_raster;
  resources.selected = false;
  if (sort == GaussianGpuStageSelection::NoWork) {
    return GaussianGpuStageSelection::NoWork;
  }
  if (request.gpu_driven_gaussian_raster ==
      GpuDrivenGaussianRasterMode::Disabled) {
    return GaussianGpuStageSelection::NotRequested;
  }
  const auto unavailable = [&](std::string detail) {
    if (request.gpu_driven_gaussian_raster ==
        GpuDrivenGaussianRasterMode::Require) {
      throw RendererError(RendererErrorCode::Unsupported,
          "select GPU-driven Gaussian raster",
          std::move(detail));
    }
    ++frame_counters_.gaussian_gpu_raster_fallback_count;
  };
  if (sort == GaussianGpuStageSelection::NotRequested) {
    unavailable("GPU-driven Gaussian raster consumes the GPU-sorted stream "
                "and requires GPU-driven Gaussian sorting");
    return GaussianGpuStageSelection::Unavailable;
  }
  if (sort == GaussianGpuStageSelection::Unavailable) {
    unavailable("GPU-driven Gaussian sorting is unavailable");
    return GaussianGpuStageSelection::Unavailable;
  }
  const auto& sorted = frame.gaussian_gpu_sort;
  const auto record_bytes = static_cast<VkDeviceSize>(sorted.key_count) *
                            sizeof(shader_abi::GaussianPreparedRecord);
  if (record_bytes > max_storage_buffer_range_) {
    unavailable("the sorted Gaussian stream exceeds maxStorageBufferRange");
    return GaussianGpuStageSelection::Unavailable;
  }
  VkPipeline gather_pipeline{};
  try {
    gather_pipeline = EnsureGaussianRasterGatherPipeline(request.shaders);
  } catch (const RendererError& error) {
    if (request.gpu_driven_gaussian_raster ==
            GpuDrivenGaussianRasterMode::Require ||
        error.code() == RendererErrorCode::DeviceLost ||
        error.code() == RendererErrorCode::ResourceExhausted ||
        error.code() == RendererErrorCode::Timeout) {
      throw;
    }
    unavailable("the packaged Gaussian raster gather artifact is "
                "unavailable");
    return GaussianGpuStageSelection::Unavailable;
  }
  EnsureGaussianGpuRasterFrameResources(resources, record_bytes);
  UpdateGaussianGpuRasterDescriptors(resources,
      sorted.keys[sorted.pass_count % 2U].handle,
      frame.gaussian_gpu_preparation.prepared_records.handle);
  resources.gather_pipeline = gather_pipeline;
  resources.element_count = sorted.key_count;
  resources.workgroup_count = sorted.block_count;
  resources.selected = true;
  return GaussianGpuStageSelection::Selected;
}

// Runs after the sort, whose barriers already make the final scatter and
// the prepared records visible to later compute work.
void Renderer::Impl::RecordGaussianGpuRasterGather(VkCommandBuffer command,
    const FrameContext& frame) {
  const auto& resources = frame.gaussian_gpu_raster;
  if (!resources.selected) {
    return;
  }
  // Zero instances unless the gather finds a real record.
  const VkDrawIndirectCommand initial{
      shader_abi::kGaussianRasterVertexCount, 0U, 0U, 0U};
  vkCmdUpdateBuffer(command, resources.draw_arguments.handle, 0,
      sizeof(initial), &initial);
  VkMemoryBarrier clear_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  clear_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  clear_barrier.dstAccessMask =
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
      &clear_barrier, 0, nullptr, 0, nullptr);
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
      resources.gather_pipeline);
  vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
      gaussian_sort_pipeline_layout_, 0, 1, &resources.descriptor_set, 0,
      nullptr);
  shader_abi::GaussianRasterGatherConstants constants;
  constants.element_count = resources.element_count;
  vkCmdPushConstants(command, gaussian_sort_pipeline_layout_,
      VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
  vkCmdDispatch(command, resources.workgroup_count, 1, 1);
  ++frame_counters_.gaussian_gpu_raster_dispatch_count;
  VkMemoryBarrier output_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  output_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  output_barrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                                 VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
                                 VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
          VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
          VK_PIPELINE_STAGE_TRANSFER_BIT,
      0, 1, &output_barrier, 0, nullptr, 0, nullptr);
  const VkBufferCopy copy{0, 0, sizeof(VkDrawIndirectCommand)};
  vkCmdCopyBuffer(command, resources.draw_arguments.handle,
      resources.draw_readback.handle, 1, &copy);
}

// Native export, host copies, readback, and presentation all consume the
// same RGBA8 image. Quantize the completed Gaussian composite only once.
void Renderer::Impl::RecordGaussianColorConversion(VkCommandBuffer command) {
  VkImageMemoryBarrier source{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  source.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_SHADER_WRITE_BIT;
  source.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  source.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  source.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  source.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  source.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  source.image = active_target_->gaussian_color;
  source.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  source.subresourceRange.levelCount = 1;
  source.subresourceRange.layerCount = 1;
  auto destination = source;
  destination.srcAccessMask = 0;
  destination.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  destination.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  destination.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  destination.image = active_target_->color;
  const std::array barriers{source, destination};
  vkCmdPipelineBarrier(command,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
      static_cast<std::uint32_t>(barriers.size()), barriers.data());
  VkImageBlit blit{};
  blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  blit.srcSubresource.layerCount = 1;
  blit.srcOffsets[1] = {static_cast<std::int32_t>(active_target_->width),
      static_cast<std::int32_t>(active_target_->height), 1};
  blit.dstSubresource = blit.srcSubresource;
  blit.dstOffsets[1] = blit.srcOffsets[1];
  vkCmdBlitImage(command, active_target_->gaussian_color,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, active_target_->color,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
  destination.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  destination.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  destination.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  destination.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &destination);
}

std::uint64_t Renderer::Impl::ReadGaussianRasterNanoseconds(
    const FrameContext& frame) const {
  return ReadGpuTimestampSpanNanoseconds(frame, 2, 3);
}

// Runs after sort verification, which has already failed the frame for an
// out-of-order stream.
void Renderer::Impl::ResolveGaussianGpuRasterDraw(FrameContext& frame) {
  const auto& resources = frame.gaussian_gpu_raster;
  if (!resources.selected) {
    return;
  }
  VkDrawIndirectCommand draw{};
  void* mapped{};
  Check(vkMapMemory(device_, resources.draw_readback.memory, 0,
            sizeof(draw), 0, &mapped),
      "map Gaussian raster draw arguments");
  std::memcpy(&draw, mapped, sizeof(draw));
  vkUnmapMemory(device_, resources.draw_readback.memory);
  if (draw.vertexCount != shader_abi::kGaussianRasterVertexCount ||
      draw.firstVertex != 0U || draw.firstInstance != 0U ||
      draw.instanceCount != frame_counters_.gaussian_gpu_sorted_count) {
    throw RendererError(RendererErrorCode::BackendFailure,
        "resolve Gaussian raster draw arguments",
        "the indirect draw does not cover exactly the sorted records");
  }
  frame_counters_.gaussian_gpu_raster_instance_count = draw.instanceCount;
}

} // namespace merlin::vulkan
