#include "renderer_impl.hpp"

namespace merlin::vulkan {

void Renderer::Impl::DestroyGaussianGpuTileFrameResources(
    GaussianGpuTileFrameResources& resources) noexcept {
  for (auto& pairs : resources.pairs) {
    DestroyBuffer(pairs);
  }
  DestroyBuffer(resources.control);
  DestroyBuffer(resources.verification_readback);
  if (resources.descriptor_pool != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(device_, resources.descriptor_pool, nullptr);
  }
  resources = {};
}

void Renderer::Impl::DestroyGaussianGpuTileRasterFrameResources(
    GaussianGpuTileRasterFrameResources& resources) noexcept {
  if (resources.descriptor_pool != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(device_, resources.descriptor_pool, nullptr);
  }
  resources = {};
}

std::filesystem::path Renderer::Impl::GaussianTileDirectory(
    const ShaderPaths& shaders) {
  return shaders.gaussian_tile_directory.empty()
             ? GaussianSortDirectory(shaders)
             : shaders.gaussian_tile_directory;
}

void Renderer::Impl::EnsureGaussianGpuTileFrameResources(
    GaussianGpuTileFrameResources& resources, VkDeviceSize pair_bytes,
    VkDeviceSize control_bytes) {
  if (resources.pair_capacity_bytes >= pair_bytes &&
      resources.control_capacity_bytes >= control_bytes &&
      resources.descriptor_pool != VK_NULL_HANDLE) {
    return;
  }
  DestroyGaussianGpuTileFrameResources(resources);
  resources.pair_capacity_bytes = pair_bytes;
  resources.control_capacity_bytes = control_bytes;
  for (auto& pairs : resources.pairs) {
    pairs = CreateBuffer(pair_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  }
  resources.control = CreateBuffer(
      control_bytes,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  resources.verification_readback = CreateBuffer(
      sizeof(shader_abi::GaussianTileVerification),
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  constexpr std::uint32_t kSetCount = 2;
  const VkDescriptorPoolSize pool_size{
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      kSetCount * kGaussianSortStorageBufferCount};
  VkDescriptorPoolCreateInfo pool_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = kSetCount;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  Check(vkCreateDescriptorPool(device_, &pool_info, nullptr,
            &resources.descriptor_pool),
      "create Gaussian tile descriptor pool");
  ++frame_counters_.descriptor_pool_creation_count;
  const std::array layouts{gaussian_sort_descriptor_set_layout_,
      gaussian_sort_descriptor_set_layout_};
  VkDescriptorSetAllocateInfo allocate{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorPool = resources.descriptor_pool;
  allocate.descriptorSetCount = kSetCount;
  allocate.pSetLayouts = layouts.data();
  Check(vkAllocateDescriptorSets(device_, &allocate,
            resources.descriptor_sets.data()),
      "allocate Gaussian tile descriptor sets");
  frame_counters_.descriptor_allocation_count += kSetCount;
}

// Rewritten every selected frame: a recreated raster-record buffer may
// reuse a destroyed handle's value.
void Renderer::Impl::UpdateGaussianGpuTileDescriptors(
    const GaussianGpuTileFrameResources& resources, VkBuffer records) {
  std::array<VkDescriptorBufferInfo,
      2 * kGaussianSortStorageBufferCount>
      buffer_infos{};
  std::array<VkWriteDescriptorSet, buffer_infos.size()> writes{};
  for (std::size_t set = 0; set < resources.descriptor_sets.size(); ++set) {
    const std::array<VkBuffer, kGaussianSortStorageBufferCount> buffers{
        resources.pairs[set].handle,
        resources.pairs[1U - set].handle,
        resources.control.handle,
        records,
    };
    static constexpr std::array bindings{
        shader_abi::kGaussianTileSource.binding,
        shader_abi::kGaussianTileDestination.binding,
        shader_abi::kGaussianTileControl.binding,
        shader_abi::kGaussianTileRecords.binding,
    };
    static_assert(bindings.size() == kGaussianSortStorageBufferCount);
    for (std::size_t binding = 0; binding < buffers.size(); ++binding) {
      const auto index = set * kGaussianSortStorageBufferCount + binding;
      buffer_infos[index] = {buffers[binding], 0, VK_WHOLE_SIZE};
      writes[index] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[index].dstSet = resources.descriptor_sets[set];
      writes[index].dstBinding = bindings[binding];
      writes[index].descriptorCount = 1;
      writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[index].pBufferInfo = &buffer_infos[index];
    }
  }
  vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
      writes.data(), 0, nullptr);
  frame_counters_.descriptor_update_count += writes.size();
}

// Replays tile binning over the CPU-sorted reference stream with the same
// bounds, per-record limit, capacity truncation, and stable tile grouping,
// so matching streams produce the GPU verification checksum.
void Renderer::Impl::ComputeGaussianTileReference(GaussianGpuTileFrameResources& resources) {
  if (!resources.selected) {
    return;
  }
  const auto& constants = resources.constants;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs;
  std::uint64_t requested{};
  const auto& gaussians = prepared_gaussians_.gaussians;
  for (std::size_t record = 0; record < gaussians.size(); ++record) {
    shader_abi::GaussianTileRect rect;
    if (!shader_abi::GaussianTileBounds(gaussians[record].center_pixels,
            gaussians[record].radius_pixels, constants.viewport_width,
            constants.viewport_height, rect)) {
      continue;
    }
    std::uint32_t remaining = constants.record_pair_limit;
    for (auto y = rect.y0; y <= rect.y1 && remaining != 0U; ++y) {
      for (auto x = rect.x0; x <= rect.x1 && remaining != 0U; ++x) {
        --remaining;
        ++requested;
        if (pairs.size() < constants.pair_capacity) {
          pairs.emplace_back(y * constants.tile_count_x + x,
              static_cast<std::uint32_t>(record));
        }
      }
    }
  }
  std::stable_sort(pairs.begin(), pairs.end(),
      [](const auto& lhs, const auto& rhs) {
        return lhs.first < rhs.first;
      });
  std::uint32_t checksum{};
  for (std::size_t i = 0; i < pairs.size(); ++i) {
    checksum += static_cast<std::uint32_t>(i + 1U) *
                shader_abi::GaussianTilePairHash(
                    pairs[i].first, pairs[i].second);
  }
  resources.reference_requested_pair_count =
      static_cast<std::uint32_t>(requested);
  resources.reference_checksum = checksum;
  resources.reference_valid = true;
}

// Tile binning consumes the gathered raster-order records and the draw
// arguments' instance count, so it depends on sorted-stream raster for the
// same frame. Its dispatch plan is fixed by the padded record count, the
// pair capacity, and the tile grid.
Renderer::Impl::GaussianGpuStageSelection Renderer::Impl::PrepareGaussianGpuTiles(FrameContext& frame,
    const RenderRequest& request, GaussianGpuStageSelection raster) {
  auto& resources = frame.gaussian_gpu_tiles;
  resources.selected = false;
  if (raster == GaussianGpuStageSelection::NoWork) {
    return GaussianGpuStageSelection::NoWork;
  }
  if (request.gpu_driven_gaussian_tiles ==
      GpuDrivenGaussianTileMode::Disabled) {
    return GaussianGpuStageSelection::NotRequested;
  }
  const auto unavailable = [&](std::string detail) {
    if (request.gpu_driven_gaussian_tiles ==
        GpuDrivenGaussianTileMode::Require) {
      throw RendererError(RendererErrorCode::Unsupported,
          "select GPU-driven Gaussian tile binning",
          std::move(detail));
    }
    ++frame_counters_.gaussian_gpu_tile_fallback_count;
  };
  if (raster == GaussianGpuStageSelection::NotRequested) {
    unavailable("GPU-driven Gaussian tile binning consumes the gathered "
                "sorted stream and requires GPU-driven Gaussian raster");
    return GaussianGpuStageSelection::Unavailable;
  }
  if (raster == GaussianGpuStageSelection::Unavailable) {
    unavailable("GPU-driven Gaussian raster is unavailable");
    return GaussianGpuStageSelection::Unavailable;
  }
  const auto& sorted = frame.gaussian_gpu_sort;
  const auto tile_count_x = shader_abi::GaussianTileGridSize(request.width);
  const auto tile_count_y = shader_abi::GaussianTileGridSize(request.height);
  const auto tile_count =
      static_cast<std::uint64_t>(tile_count_x) * tile_count_y;

  // Default capacity scales with the padded record count; an explicit
  // capacity must fit the device as requested.
  constexpr auto kPairBytes = sizeof(shader_abi::GaussianSortElement);
  const auto device_pairs =
      std::min<std::uint64_t>(max_storage_buffer_range_ / kPairBytes,
          std::numeric_limits<std::uint32_t>::max()) /
      shader_abi::kGaussianTileWorkgroupSize *
      shader_abi::kGaussianTileWorkgroupSize;
  std::uint64_t pair_capacity{};
  if (request.gaussian_tile_pair_capacity != 0U) {
    pair_capacity = AlignUp(
        std::uint64_t{request.gaussian_tile_pair_capacity},
        shader_abi::kGaussianTileWorkgroupSize);
    if (pair_capacity > device_pairs) {
      unavailable("the Gaussian tile pair capacity exceeds "
                  "maxStorageBufferRange");
      return GaussianGpuStageSelection::Unavailable;
    }
  } else {
    pair_capacity = std::min(device_pairs,
        std::max<std::uint64_t>(
            std::uint64_t{sorted.key_count} *
                shader_abi::kGaussianTileDefaultPairsPerRecord,
            shader_abi::kGaussianTileMinimumPairCapacity));
  }
  const auto pair_blocks = pair_capacity /
                           shader_abi::kGaussianTileWorkgroupSize;
  if (pair_blocks == 0U || pair_blocks > max_compute_work_group_count_x_) {
    unavailable("the Gaussian tile pairs exceed maxComputeWorkGroupCount[0]");
    return GaussianGpuStageSelection::Unavailable;
  }

  // Control words, per-record offsets, the pair histogram (one entry per
  // pair slot), then two range words per tile.
  std::vector<GaussianGpuSortScanLevel> offset_levels;
  std::vector<GaussianGpuSortScanLevel> histogram_levels;
  const auto histogram_offset = AppendGaussianScanLevels(
      shader_abi::kGaussianTileControlWordCount, sorted.key_count,
      offset_levels);
  const auto ranges_offset = histogram_offset
                                 ? AppendGaussianScanLevels(
                                       *histogram_offset, pair_capacity,
                                       histogram_levels)
                                 : std::nullopt;
  if (!ranges_offset ||
      *ranges_offset + 2U * tile_count >
          std::numeric_limits<std::uint32_t>::max()) {
    unavailable("the Gaussian tile control words exceed uint32 addressing");
    return GaussianGpuStageSelection::Unavailable;
  }
  const auto control_bytes =
      (*ranges_offset + 2U * tile_count) * sizeof(std::uint32_t);
  const auto pair_bytes = pair_capacity * kPairBytes;
  if (control_bytes > max_storage_buffer_range_) {
    unavailable("a Gaussian tile binding exceeds maxStorageBufferRange");
    return GaussianGpuStageSelection::Unavailable;
  }

  std::array<VkPipeline, kGaussianTileKernelCount> pipelines{};
  try {
    pipelines = EnsureGaussianTileComputePipelines(request.shaders);
  } catch (const RendererError& error) {
    if (request.gpu_driven_gaussian_tiles ==
            GpuDrivenGaussianTileMode::Require ||
        error.code() == RendererErrorCode::DeviceLost ||
        error.code() == RendererErrorCode::ResourceExhausted ||
        error.code() == RendererErrorCode::Timeout) {
      throw;
    }
    unavailable("the packaged Gaussian tile artifacts are unavailable");
    return GaussianGpuStageSelection::Unavailable;
  }
  EnsureGaussianGpuTileFrameResources(resources, pair_bytes, control_bytes);
  UpdateGaussianGpuTileDescriptors(resources,
      frame.gaussian_gpu_raster.records.handle);
  resources.pipelines = pipelines;
  resources.sort_pipelines = sorted.pipelines;
  resources.offset_levels = std::move(offset_levels);
  resources.histogram_levels = std::move(histogram_levels);
  auto& constants = resources.constants;
  constants = {};
  constants.record_bound = sorted.key_count;
  constants.pair_capacity = static_cast<std::uint32_t>(pair_capacity);
  constants.tile_count_x = tile_count_x;
  constants.tile_count_y = tile_count_y;
  constants.viewport_width = request.width;
  constants.viewport_height = request.height;
  constants.offsets_offset = shader_abi::kGaussianTileControlWordCount;
  constants.ranges_offset = static_cast<std::uint32_t>(*ranges_offset);
  // Every per-record count stays within this limit, so the padded prefix
  // sum of counts stays within uint32.
  constants.record_pair_limit = static_cast<std::uint32_t>(
      std::min<std::uint64_t>(tile_count,
          std::numeric_limits<std::uint32_t>::max() / sorted.key_count));
  resources.record_blocks = sorted.block_count;
  resources.pair_blocks = static_cast<std::uint32_t>(pair_blocks);
  // Stable LSD passes over only the tile-index bytes the grid can occupy.
  resources.sort_pass_count =
      tile_count <= 1U
          ? 0U
          : (static_cast<std::uint32_t>(std::bit_width(tile_count - 1U)) +
                shader_abi::kGaussianSortRadixBits - 1U) /
                shader_abi::kGaussianSortRadixBits;
  resources.reference_valid = false;
  frame_counters_.gaussian_gpu_tile_count = tile_count;
  frame_counters_.gaussian_gpu_tile_pair_capacity = pair_capacity;
  resources.selected = true;
  return GaussianGpuStageSelection::Selected;
}

std::filesystem::path Renderer::Impl::GaussianTileRasterDirectory(
    const ShaderPaths& shaders) {
  return shaders.gaussian_tile_raster_directory.empty()
             ? GaussianTileDirectory(shaders)
             : shaders.gaussian_tile_raster_directory;
}

void Renderer::Impl::EnsureGaussianGpuTileRasterFrameResources(
    GaussianGpuTileRasterFrameResources& resources) {
  if (resources.descriptor_pool != VK_NULL_HANDLE) {
    return;
  }
  const std::array<VkDescriptorPoolSize, 3> pool_sizes{{
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          kGaussianTileRasterStorageBufferCount},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          kGaussianTileRasterStorageImageCount},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1U},
  }};
  VkDescriptorPoolCreateInfo pool_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size());
  pool_info.pPoolSizes = pool_sizes.data();
  Check(vkCreateDescriptorPool(device_, &pool_info, nullptr,
            &resources.descriptor_pool),
      "create Gaussian tile raster descriptor pool");
  ++frame_counters_.descriptor_pool_creation_count;
  VkDescriptorSetAllocateInfo allocate{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorPool = resources.descriptor_pool;
  allocate.descriptorSetCount = 1;
  allocate.pSetLayouts = &gaussian_tile_raster_descriptor_set_layout_;
  Check(vkAllocateDescriptorSets(device_, &allocate,
            &resources.descriptor_set),
      "allocate Gaussian tile raster descriptor set");
  ++frame_counters_.descriptor_allocation_count;
}

// Rewritten every selected frame: the grouped pair buffer follows the
// frame's pass count, and recreated buffers or targets may reuse a
// destroyed handle's value. The images are bound in the layouts the
// raster dispatch transitions them to.
void Renderer::Impl::UpdateGaussianGpuTileRasterDescriptors(const FrameContext& frame) {
  const auto& tiles = frame.gaussian_gpu_tiles;
  const std::array<VkDescriptorBufferInfo,
      kGaussianTileRasterStorageBufferCount>
      buffers{{
          {tiles.pairs[tiles.sort_pass_count % 2U].handle, 0, VK_WHOLE_SIZE},
          {tiles.control.handle, 0, VK_WHOLE_SIZE},
          {frame.gaussian_gpu_raster.records.handle, 0, VK_WHOLE_SIZE},
          {frame.gaussian_gpu_raster.draw_arguments.handle, 0,
              VK_WHOLE_SIZE},
      }};
  const std::array<VkDescriptorImageInfo, 4> images{{
      {VK_NULL_HANDLE, active_target_->gaussian_color_view,
          VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, active_target_->prim_id_view,
          VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, active_target_->instance_id_view,
          VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, active_target_->depth_view,
          VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
  }};
  static constexpr std::array buffer_bindings{
      shader_abi::kGaussianTileRasterPairs.binding,
      shader_abi::kGaussianTileRasterControl.binding,
      shader_abi::kGaussianTileRasterRecords.binding,
      shader_abi::kGaussianTileRasterDraw.binding,
  };
  static constexpr std::array image_bindings{
      shader_abi::kGaussianTileRasterColor.binding,
      shader_abi::kGaussianTileRasterPrimId.binding,
      shader_abi::kGaussianTileRasterInstanceId.binding,
      shader_abi::kGaussianTileRasterDepth.binding,
  };
  std::array<VkWriteDescriptorSet, buffers.size() + images.size()> writes{};
  for (std::size_t i = 0; i < buffers.size(); ++i) {
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[i].dstSet = frame.gaussian_gpu_tile_raster.descriptor_set;
    writes[i].dstBinding = buffer_bindings[i];
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = &buffers[i];
  }
  for (std::size_t i = 0; i < images.size(); ++i) {
    auto& write = writes[buffers.size() + i];
    write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = frame.gaussian_gpu_tile_raster.descriptor_set;
    write.dstBinding = image_bindings[i];
    write.descriptorCount = 1;
    write.descriptorType = i + 1U == images.size()
                               ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                               : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &images[i];
  }
  vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
      writes.data(), 0, nullptr);
  frame_counters_.descriptor_update_count += writes.size();
}

// Tile raster composites the verified tile ranges, so it depends on tile
// binning for the same frame and on a target created with storage color
// and ID images and a sampled depth image. Its dispatch plan is one
// selection thread and one workgroup per tile.
void Renderer::Impl::PrepareGaussianGpuTileRaster(FrameContext& frame,
    const RenderRequest& request, GaussianGpuStageSelection tiles) {
  auto& resources = frame.gaussian_gpu_tile_raster;
  resources.selected = false;
  if (tiles == GaussianGpuStageSelection::NoWork ||
      request.gpu_driven_gaussian_tile_raster ==
          GpuDrivenGaussianTileRasterMode::Disabled) {
    return;
  }
  const auto unavailable = [&](std::string detail) {
    if (request.gpu_driven_gaussian_tile_raster ==
        GpuDrivenGaussianTileRasterMode::Require) {
      throw RendererError(RendererErrorCode::Unsupported,
          "select GPU-driven Gaussian tile raster", std::move(detail));
    }
    ++frame_counters_.gaussian_gpu_tile_raster_fallback_count;
  };
  if (tiles == GaussianGpuStageSelection::NotRequested) {
    unavailable("GPU-driven Gaussian tile raster composites the tile "
                "ranges and requires GPU-driven Gaussian tile binning");
    return;
  }
  if (tiles == GaussianGpuStageSelection::Unavailable) {
    unavailable("GPU-driven Gaussian tile binning is unavailable");
    return;
  }
  if (!active_target_->gaussian_tile_raster_images) {
    unavailable("tile raster requires RGBA32F blend/blit/storage support, "
                "RGBA8 blit-destination support, storage ID images and "
                "sampled depth on this device");
    return;
  }
  const auto& binning = frame.gaussian_gpu_tiles.constants;
  if (binning.tile_count_x > max_compute_work_group_count_x_ ||
      binning.tile_count_y > max_compute_work_group_count_y_) {
    unavailable("the Gaussian tile grid exceeds maxComputeWorkGroupCount");
    return;
  }
  VkPipeline select_pipeline{};
  VkPipeline raster_pipeline{};
  try {
    const auto directory = GaussianTileRasterDirectory(request.shaders);
    select_pipeline = EnsureGaussianTileRasterPipeline(
        directory / "gaussian-tile-raster-select.comp.spv");
    raster_pipeline = EnsureGaussianTileRasterPipeline(
        directory / "gaussian-tile-raster.comp.spv");
  } catch (const RendererError& error) {
    if (request.gpu_driven_gaussian_tile_raster ==
            GpuDrivenGaussianTileRasterMode::Require ||
        error.code() == RendererErrorCode::DeviceLost ||
        error.code() == RendererErrorCode::ResourceExhausted ||
        error.code() == RendererErrorCode::Timeout) {
      throw;
    }
    unavailable("the packaged Gaussian tile raster artifacts are "
                "unavailable");
    return;
  }
  EnsureGaussianGpuTileRasterFrameResources(resources);
  UpdateGaussianGpuTileRasterDescriptors(frame);
  resources.select_pipeline = select_pipeline;
  resources.raster_pipeline = raster_pipeline;
  auto& constants = resources.constants;
  constants = {};
  constants.tile_count_x = binning.tile_count_x;
  constants.tile_count_y = binning.tile_count_y;
  constants.viewport_width = binning.viewport_width;
  constants.viewport_height = binning.viewport_height;
  constants.ranges_offset = binning.ranges_offset;
  constants.pair_capacity = binning.pair_capacity;
  resources.selected = true;
}

bool Renderer::Impl::GaussianTileRasterFormatsSupported() {
  if (!gaussian_tile_raster_formats_supported_) {
    const auto supports = [&](VkFormat format,
                              VkFormatFeatureFlags features) {
      VkFormatProperties properties{};
      vkGetPhysicalDeviceFormatProperties(physical_device_, format,
          &properties);
      return (properties.optimalTilingFeatures & features) == features;
    };
    gaussian_tile_raster_formats_supported_ =
        GaussianFloatColorSupported() &&
        supports(kGaussianColorFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) &&
        supports(kIdFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) &&
        supports(kDepthFormat, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
  }
  return *gaussian_tile_raster_formats_supported_;
}

// Runs after the gather, whose barrier already made the draw arguments
// visible to transfer reads.
void Renderer::Impl::RecordGaussianGpuTiles(VkCommandBuffer command,
    const FrameContext& frame) {
  const auto& resources = frame.gaussian_gpu_tiles;
  if (!resources.selected) {
    return;
  }
  const auto& constants = resources.constants;
  vkCmdFillBuffer(command, resources.control.handle, 0,
      sizeof(shader_abi::GaussianTileVerification), 0U);
  vkCmdFillBuffer(command, resources.control.handle,
      static_cast<VkDeviceSize>(constants.ranges_offset) *
          sizeof(std::uint32_t),
      static_cast<VkDeviceSize>(constants.tile_count_x) *
          constants.tile_count_y * 2U * sizeof(std::uint32_t),
      0U);
  const VkBufferCopy record_count_copy{
      offsetof(VkDrawIndirectCommand, instanceCount),
      shader_abi::kGaussianTileRecordCountWord * sizeof(std::uint32_t),
      sizeof(std::uint32_t)};
  // The verification clear includes the record-count word. Order that
  // transfer write before overwriting the word with the gathered count;
  // the compute input barrier below cannot order two preceding transfers.
  VkBufferMemoryBarrier count_copy_barrier{
      VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  count_copy_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  count_copy_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  count_copy_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  count_copy_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  count_copy_barrier.buffer = resources.control.handle;
  count_copy_barrier.offset = record_count_copy.dstOffset;
  count_copy_barrier.size = record_count_copy.size;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1,
      &count_copy_barrier, 0, nullptr);
  vkCmdCopyBuffer(command, frame.gaussian_gpu_raster.draw_arguments.handle,
      resources.control.handle, 1, &record_count_copy);
  // Gathered records come from the gather dispatch; control words from the
  // clears and the record-count copy above.
  VkMemoryBarrier input_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  input_barrier.srcAccessMask =
      VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  input_barrier.dstAccessMask =
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  vkCmdPipelineBarrier(command,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
          VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
      &input_barrier, 0, nullptr, 0, nullptr);
  const auto compute_barrier = [&] {
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
        &barrier, 0, nullptr, 0, nullptr);
  };
  const auto dispatch = [&](VkPipeline pipeline, std::size_t set,
                            const void* values, std::uint32_t size,
                            std::uint32_t workgroups) {
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
        gaussian_sort_pipeline_layout_, 0, 1,
        &resources.descriptor_sets[set], 0, nullptr);
    vkCmdPushConstants(command, gaussian_sort_pipeline_layout_,
        VK_SHADER_STAGE_COMPUTE_BIT, 0, size, values);
    vkCmdDispatch(command, workgroups, 1, 1);
    ++frame_counters_.gaussian_gpu_tile_dispatch_count;
  };
  const auto tile_dispatch = [&](GaussianTileKernel kernel, std::size_t set,
                                 std::uint32_t workgroups) {
    dispatch(resources.pipelines[kernel], set, &constants,
        sizeof(constants), workgroups);
  };
  const auto sort_dispatch = [&](GaussianSortKernel kernel, std::size_t set,
                                 const shader_abi::GaussianSortConstants&
                                     values,
                                 std::uint32_t workgroups) {
    dispatch(resources.sort_pipelines[kernel], set, &values,
        sizeof(values), workgroups);
  };
  // The top level fits one workgroup, so its scan is already global.
  const auto scan = [&](const std::vector<GaussianGpuSortScanLevel>& levels,
                        std::size_t set,
                        shader_abi::GaussianSortConstants values) {
    for (const auto& level : levels) {
      values.scan_offset = level.offset;
      values.scan_count = level.count;
      values.scan_sums_offset = level.sums_offset;
      sort_dispatch(kGaussianSortScanBlocks, set, values,
          shader_abi::GaussianSortScanWorkgroupCount(level.count));
      compute_barrier();
    }
    for (auto level = levels.rbegin() + 1; level != levels.rend(); ++level) {
      values.scan_offset = level->offset;
      values.scan_count = level->count;
      values.scan_sums_offset = level->sums_offset;
      sort_dispatch(kGaussianSortScanAdd, set, values,
          shader_abi::GaussianSortScanWorkgroupCount(level->count));
      compute_barrier();
    }
  };

  tile_dispatch(kGaussianTileCount, 0, resources.record_blocks);
  compute_barrier();
  scan(resources.offset_levels, 0, {});
  // Set 1 writes pairs[0], where the first pair-sort pass reads.
  tile_dispatch(kGaussianTileEmit, 1, resources.record_blocks);
  compute_barrier();
  for (std::uint32_t pass = 0; pass < resources.sort_pass_count; ++pass) {
    const std::size_t set = pass % 2U;
    shader_abi::GaussianSortConstants values;
    values.element_count = constants.pair_capacity;
    values.block_count = resources.pair_blocks;
    values.digit_shift = pass * shader_abi::kGaussianSortRadixBits;
    values.digit_word = 0U;
    values.count_word = shader_abi::kGaussianTileRequestedPairCountWord;
    values.flags = shader_abi::kGaussianSortDynamicCount;
    values.scan_offset = resources.histogram_levels.front().offset;
    sort_dispatch(kGaussianSortHistogram, set, values,
        resources.pair_blocks);
    compute_barrier();
    scan(resources.histogram_levels, set, values);
    values.scan_offset = resources.histogram_levels.front().offset;
    values.scan_count = 0;
    values.scan_sums_offset = 0;
    sort_dispatch(kGaussianSortScatter, set, values, resources.pair_blocks);
    compute_barrier();
  }
  const std::size_t grouped = resources.sort_pass_count % 2U;
  tile_dispatch(kGaussianTileRanges, grouped, resources.pair_blocks);
  compute_barrier();
  tile_dispatch(kGaussianTileVerify, grouped, resources.pair_blocks);
  frame_counters_.gaussian_gpu_tile_sort_pass_count +=
      resources.sort_pass_count;

  // Tile raster decides on the device, from the binning's final counts,
  // whether it replaces the sorted-stream draws; its later raster dispatch
  // reads the decision, the ranges, and the grouped pairs.
  const auto& tile_raster = frame.gaussian_gpu_tile_raster;
  VkPipelineStageFlags output_stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkAccessFlags output_access = VK_ACCESS_TRANSFER_READ_BIT;
  if (tile_raster.selected) {
    compute_barrier();
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
        tile_raster.select_pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
        gaussian_tile_raster_pipeline_layout_, 0, 1,
        &tile_raster.descriptor_set, 0, nullptr);
    vkCmdPushConstants(command, gaussian_tile_raster_pipeline_layout_,
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(tile_raster.constants),
        &tile_raster.constants);
    vkCmdDispatch(command, 1, 1, 1);
    ++frame_counters_.gaussian_gpu_tile_raster_dispatch_count;
    output_stages |= VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    output_access |= VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                     VK_ACCESS_SHADER_READ_BIT;
  }

  VkMemoryBarrier output_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  output_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  output_barrier.dstAccessMask = output_access;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      output_stages, 0, 1, &output_barrier, 0, nullptr, 0, nullptr);
  const VkBufferCopy copy{0, 0,
      sizeof(shader_abi::GaussianTileVerification)};
  vkCmdCopyBuffer(command, resources.control.handle,
      resources.verification_readback.handle, 1, &copy);
}

// Runs after the render pass, whose final layouts leave every attachment
// ready for transfer reads. Color and IDs move to GENERAL for storage
// access and depth to read-only for sampling; all return to the transfer-
// source layout that readback, presentation, and AOV export expect.
void Renderer::Impl::RecordGaussianGpuTileRaster(VkCommandBuffer command,
    const FrameContext& frame) {
  const auto& resources = frame.gaussian_gpu_tile_raster;
  const auto image_barrier = [](VkImage image, VkImageAspectFlags aspect,
                                 VkImageLayout old_layout,
                                 VkImageLayout new_layout,
                                 VkAccessFlags src_access,
                                 VkAccessFlags dst_access) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    return barrier;
  };
  constexpr auto kTransferSource = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  constexpr auto kDepthReadOnly =
      VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  const std::array to_compute{
      image_barrier(active_target_->gaussian_color, VK_IMAGE_ASPECT_COLOR_BIT,
          kTransferSource, VK_IMAGE_LAYOUT_GENERAL,
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT),
      image_barrier(active_target_->prim_id, VK_IMAGE_ASPECT_COLOR_BIT,
          kTransferSource, VK_IMAGE_LAYOUT_GENERAL,
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          VK_ACCESS_SHADER_WRITE_BIT),
      image_barrier(active_target_->instance_id, VK_IMAGE_ASPECT_COLOR_BIT,
          kTransferSource, VK_IMAGE_LAYOUT_GENERAL,
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          VK_ACCESS_SHADER_WRITE_BIT),
      image_barrier(active_target_->depth, VK_IMAGE_ASPECT_DEPTH_BIT,
          kTransferSource, kDepthReadOnly,
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
          VK_ACCESS_SHADER_READ_BIT),
  };
  // The transfer stage chains through the render pass's external
  // dependency, which orders its final layout transitions.
  vkCmdPipelineBarrier(command,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
          VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
      static_cast<std::uint32_t>(to_compute.size()), to_compute.data());
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
      resources.raster_pipeline);
  vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
      gaussian_tile_raster_pipeline_layout_, 0, 1,
      &resources.descriptor_set, 0, nullptr);
  vkCmdPushConstants(command, gaussian_tile_raster_pipeline_layout_,
      VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(resources.constants),
      &resources.constants);
  vkCmdDispatch(command, resources.constants.tile_count_x,
      resources.constants.tile_count_y, 1);
  ++frame_counters_.gaussian_gpu_tile_raster_dispatch_count;
  const std::array to_transfer{
      image_barrier(active_target_->gaussian_color, VK_IMAGE_ASPECT_COLOR_BIT,
          VK_IMAGE_LAYOUT_GENERAL, kTransferSource,
          VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
      image_barrier(active_target_->prim_id, VK_IMAGE_ASPECT_COLOR_BIT,
          VK_IMAGE_LAYOUT_GENERAL, kTransferSource,
          VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
      image_barrier(active_target_->instance_id, VK_IMAGE_ASPECT_COLOR_BIT,
          VK_IMAGE_LAYOUT_GENERAL, kTransferSource,
          VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
      image_barrier(active_target_->depth, VK_IMAGE_ASPECT_DEPTH_BIT,
          kDepthReadOnly, kTransferSource, 0, VK_ACCESS_TRANSFER_READ_BIT),
  };
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
      static_cast<std::uint32_t>(to_transfer.size()), to_transfer.data());
}

std::uint64_t Renderer::Impl::ReadGaussianGpuTileNanoseconds(
    const FrameContext& frame) const {
  return frame.gaussian_gpu_tiles.selected
             ? ReadGpuTimestampSpanNanoseconds(frame, 6, 7)
             : 0;
}

// Runs after the raster draw resolve, which has already checked that the
// gathered stream covers exactly the verified sorted records.
void Renderer::Impl::ResolveGaussianGpuTiles(FrameContext& frame) {
  const auto& resources = frame.gaussian_gpu_tiles;
  if (!resources.selected) {
    return;
  }
  shader_abi::GaussianTileVerification verification;
  void* mapped{};
  Check(vkMapMemory(device_, resources.verification_readback.memory, 0,
            sizeof(verification), 0, &mapped),
      "map Gaussian tile verification");
  std::memcpy(&verification, mapped, sizeof(verification));
  vkUnmapMemory(device_, resources.verification_readback.memory);
  if (verification.order_violation_count != 0U ||
      verification.pair_mismatch_count != 0U ||
      verification.range_violation_count != 0U) {
    throw RendererError(RendererErrorCode::BackendFailure,
        "resolve Gaussian tile verification",
        "tile pairs are out of order, disagree with their records, or "
        "escape their tile ranges");
  }
  const auto stored = std::min(verification.requested_pair_count,
      resources.constants.pair_capacity);
  if (verification.record_count !=
          frame_counters_.gaussian_gpu_raster_instance_count ||
      verification.verified_pair_count != stored) {
    throw RendererError(RendererErrorCode::BackendFailure,
        "resolve Gaussian tile verification",
        "tile binning did not retain every stored pair of the sorted "
        "stream exactly once");
  }
  frame_counters_.gaussian_gpu_tile_requested_pair_count =
      verification.requested_pair_count;
  frame_counters_.gaussian_gpu_tile_pair_count =
      verification.verified_pair_count;
  frame_counters_.gaussian_gpu_tile_pair_overflow_count =
      verification.requested_pair_count - stored;
  frame_counters_.gaussian_gpu_tile_clamped_record_count =
      verification.clamped_record_count;
  frame_counters_.gaussian_gpu_tile_occupied_count =
      verification.occupied_tile_count;
  frame_counters_.gaussian_gpu_tile_max_pair_count =
      verification.max_tile_pair_count;
  if (resources.reference_valid &&
      (verification.requested_pair_count !=
              resources.reference_requested_pair_count ||
          verification.identity_checksum != resources.reference_checksum)) {
    ++frame_counters_.gaussian_gpu_tile_reference_divergence_count;
  }
  if (!frame.gaussian_gpu_tile_raster.selected) {
    return;
  }
  // The device keeps the sorted-stream draws whenever a pair was lost to
  // the capacity or the per-record limit, so the image never drops one.
  const bool complete =
      verification.requested_pair_count <=
          resources.constants.pair_capacity &&
      verification.clamped_record_count == 0U;
  if (verification.raster_selected != (complete ? 1U : 0U)) {
    throw RendererError(RendererErrorCode::BackendFailure,
        "resolve Gaussian tile raster selection",
        "the device tile raster selection disagrees with the binning's "
        "stored and clamped pair counts");
  }
  if (complete) {
    ++frame_counters_.gaussian_gpu_tile_raster_frame_count;
  } else {
    ++frame_counters_.gaussian_gpu_tile_raster_overflow_fallback_count;
  }
}

} // namespace merlin::vulkan
