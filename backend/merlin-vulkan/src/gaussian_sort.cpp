#include "renderer_impl.hpp"

namespace merlin::vulkan {

void Renderer::Impl::DestroyGaussianGpuSortFrameResources(
    GaussianGpuSortFrameResources& resources) noexcept {
  for (auto& keys : resources.keys) {
    DestroyBuffer(keys);
  }
  DestroyBuffer(resources.scan);
  DestroyBuffer(resources.verification_readback);
  if (resources.descriptor_pool != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(device_, resources.descriptor_pool, nullptr);
  }
  resources = {};
}

std::filesystem::path Renderer::Impl::GaussianSortDirectory(
    const ShaderPaths& shaders) {
  return shaders.gaussian_sort_directory.empty()
             ? GaussianPrepareComputePath(shaders).parent_path()
             : shaders.gaussian_sort_directory;
}

void Renderer::Impl::EnsureGaussianGpuSortFrameResources(
    GaussianGpuSortFrameResources& resources, VkDeviceSize key_bytes,
    VkDeviceSize scan_bytes) {
  if (resources.key_capacity_bytes >= key_bytes &&
      resources.scan_capacity_bytes >= scan_bytes &&
      resources.descriptor_pool != VK_NULL_HANDLE) {
    return;
  }
  DestroyGaussianGpuSortFrameResources(resources);
  resources.key_capacity_bytes = key_bytes;
  resources.scan_capacity_bytes = scan_bytes;
  for (auto& keys : resources.keys) {
    keys = CreateBuffer(key_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  }
  resources.scan = CreateBuffer(
      scan_bytes,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  resources.verification_readback = CreateBuffer(
      sizeof(shader_abi::GaussianSortVerification),
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
      "create Gaussian sort descriptor pool");
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
      "allocate Gaussian sort descriptor sets");
  frame_counters_.descriptor_allocation_count += kSetCount;
}

// Rewritten every selected frame, like the preparation descriptors: a
// recreated prepared-record buffer may reuse a destroyed handle's value.
void Renderer::Impl::UpdateGaussianGpuSortDescriptors(
    const GaussianGpuSortFrameResources& resources,
    VkBuffer prepared_records) {
  std::array<VkDescriptorBufferInfo,
      2 * kGaussianSortStorageBufferCount>
      buffer_infos{};
  std::array<VkWriteDescriptorSet, buffer_infos.size()> writes{};
  for (std::size_t set = 0; set < resources.descriptor_sets.size(); ++set) {
    const std::array<VkBuffer, kGaussianSortStorageBufferCount> buffers{
        resources.keys[set].handle,
        resources.keys[1U - set].handle,
        resources.scan.handle,
        prepared_records,
    };
    const std::array<std::uint32_t, kGaussianSortStorageBufferCount>
        bindings{
            shader_abi::kGaussianSortSource.binding,
            shader_abi::kGaussianSortDestination.binding,
            shader_abi::kGaussianSortScan.binding,
            shader_abi::kGaussianSortPreparedRecords.binding,
        };
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

// Appends the exclusive-scan levels of `count` words starting at word
// `offset`; each level's workgroup totals become the next level's input.
// Returns the first word past the top level's total, or nothing when a
// level would leave uint32 word addressing.
std::optional<std::uint64_t> Renderer::Impl::AppendGaussianScanLevels(
    std::uint64_t offset, std::uint64_t count,
    std::vector<GaussianGpuSortScanLevel>& levels) {
  while (true) {
    if (count > std::numeric_limits<std::uint32_t>::max()) {
      return std::nullopt;
    }
    const auto workgroups = shader_abi::GaussianSortScanWorkgroupCount(
        static_cast<std::uint32_t>(count));
    const auto sums_offset = offset + count;
    if (sums_offset + workgroups > std::numeric_limits<std::uint32_t>::max()) {
      return std::nullopt;
    }
    levels.push_back({static_cast<std::uint32_t>(offset),
        static_cast<std::uint32_t>(count),
        static_cast<std::uint32_t>(sums_offset)});
    if (workgroups <= 1U) {
      return sums_offset + 1U;
    }
    offset = sums_offset;
    count = workgroups;
  }
}

Renderer::Impl::GaussianGpuStageSelection Renderer::Impl::PrepareGaussianGpuSort(FrameContext& frame,
    const RenderRequest& request, GaussianGpuStageSelection preparation) {
  auto& resources = frame.gaussian_gpu_sort;
  resources.selected = false;
  if (preparation == GaussianGpuStageSelection::NoWork) {
    return GaussianGpuStageSelection::NoWork;
  }
  if (request.gpu_driven_gaussian_sort ==
      GpuDrivenGaussianSortMode::Disabled) {
    return GaussianGpuStageSelection::NotRequested;
  }
  const auto unavailable = [&](std::string detail) {
    if (request.gpu_driven_gaussian_sort ==
        GpuDrivenGaussianSortMode::Require) {
      throw RendererError(RendererErrorCode::Unsupported,
          "select GPU-driven Gaussian sorting",
          std::move(detail));
    }
    ++frame_counters_.gaussian_gpu_sort_fallback_count;
  };
  if (preparation == GaussianGpuStageSelection::NotRequested) {
    unavailable("GPU-driven Gaussian sorting consumes GPU-prepared records "
                "and requires GPU-driven Gaussian preparation");
    return GaussianGpuStageSelection::Unavailable;
  }
  if (preparation == GaussianGpuStageSelection::Unavailable) {
    unavailable("GPU-driven Gaussian preparation is unavailable");
    return GaussianGpuStageSelection::Unavailable;
  }
  const auto& prepared = frame.gaussian_gpu_preparation;

  // Candidate indices and segment bases follow ascending resource identity,
  // matching the CPU reference tie break, independent of table order.
  std::vector<std::size_t> order(prepared.batches.size());
  std::iota(order.begin(), order.end(), std::size_t{});
  std::sort(order.begin(), order.end(),
      [&](std::size_t lhs, std::size_t rhs) {
        return prepared.batches[lhs].resource <
               prepared.batches[rhs].resource;
      });
  std::uint64_t candidate_count{};
  for (const auto& batch : prepared.batches) {
    candidate_count += batch.particle_count;
  }
  // Keys pad to whole workgroups, and a real low word must stay below the
  // all-ones sentinel.
  if (candidate_count >
      std::numeric_limits<std::uint32_t>::max() -
          shader_abi::kGaussianSortWorkgroupSize) {
    unavailable("the frame exceeds the uint32 Gaussian sort key range");
    return GaussianGpuStageSelection::Unavailable;
  }
  const auto key_count = static_cast<std::uint32_t>(
      AlignUp(candidate_count, shader_abi::kGaussianSortWorkgroupSize));
  const auto block_count = shader_abi::GaussianSortWorkgroupCount(key_count);
  if (block_count > max_compute_work_group_count_x_) {
    unavailable("the Gaussian sort exceeds maxComputeWorkGroupCount[0]");
    return GaussianGpuStageSelection::Unavailable;
  }

  // Scan/control words: verification, one visible count per resource,
  // then the digit-major histogram and each scan level's block totals.
  std::vector<GaussianGpuSortScanLevel> levels;
  const auto scan_words = AppendGaussianScanLevels(
      shader_abi::kGaussianSortControlWordCount + prepared.batches.size(),
      key_count, levels);
  if (!scan_words) {
    unavailable("the Gaussian sort scan exceeds uint32 addressing");
    return GaussianGpuStageSelection::Unavailable;
  }
  const auto scan_bytes =
      static_cast<VkDeviceSize>(*scan_words) * sizeof(std::uint32_t);
  const auto key_bytes = static_cast<VkDeviceSize>(key_count) *
                         sizeof(shader_abi::GaussianSortElement);
  if (key_bytes > max_storage_buffer_range_ ||
      scan_bytes > max_storage_buffer_range_ ||
      prepared.prepared_records.size > max_storage_buffer_range_) {
    unavailable("a Gaussian sort binding exceeds maxStorageBufferRange");
    return GaussianGpuStageSelection::Unavailable;
  }

  std::vector<GaussianGpuSortKeyBatch> key_batches;
  key_batches.reserve(order.size());
  std::uint32_t candidate_base{};
  for (std::size_t rank = 0; rank < order.size(); ++rank) {
    const auto batch_index = order[rank];
    const auto& batch = prepared.batches[batch_index];
    // The last segment also writes the sentinel padding.
    const auto element_count =
        rank + 1U == order.size() ? key_count - candidate_base
                                  : batch.particle_count;
    GaussianGpuSortKeyBatch key_batch;
    key_batch.constants.element_count = element_count;
    key_batch.constants.candidate_base = candidate_base;
    key_batch.constants.prepared_base = static_cast<std::uint32_t>(
        batch.prepared_offset / sizeof(shader_abi::GaussianPreparedRecord));
    key_batch.constants.visible_count_offset = static_cast<std::uint32_t>(
        shader_abi::kGaussianSortControlWordCount + batch_index);
    key_batch.workgroup_count =
        shader_abi::GaussianSortWorkgroupCount(element_count);
    if (key_batch.workgroup_count > max_compute_work_group_count_x_) {
      unavailable("a Gaussian sort key segment exceeds "
                  "maxComputeWorkGroupCount[0]");
      return GaussianGpuStageSelection::Unavailable;
    }
    key_batches.push_back(key_batch);
    candidate_base += batch.particle_count;
  }

  std::array<VkPipeline, kGaussianSortKernelCount> pipelines{};
  try {
    pipelines = EnsureGaussianSortComputePipelines(request.shaders);
  } catch (const RendererError& error) {
    if (request.gpu_driven_gaussian_sort ==
            GpuDrivenGaussianSortMode::Require ||
        error.code() == RendererErrorCode::DeviceLost ||
        error.code() == RendererErrorCode::ResourceExhausted ||
        error.code() == RendererErrorCode::Timeout) {
      throw;
    }
    unavailable("the packaged Gaussian sort artifacts are unavailable");
    return GaussianGpuStageSelection::Unavailable;
  }
  EnsureGaussianGpuSortFrameResources(resources, key_bytes, scan_bytes);
  UpdateGaussianGpuSortDescriptors(resources,
      prepared.prepared_records.handle);
  resources.pipelines = pipelines;
  resources.key_batches = std::move(key_batches);
  resources.scan_levels = std::move(levels);
  resources.visible_count_copies.clear();
  for (std::size_t i = 0; i < prepared.batches.size(); ++i) {
    resources.visible_count_copies.push_back(
        {prepared.batches[i].counter_offset +
                offsetof(shader_abi::GaussianPrepareDispatchCounters,
                    visible_count),
            (shader_abi::kGaussianSortControlWordCount + i) *
                sizeof(std::uint32_t),
            sizeof(std::uint32_t)});
  }
  resources.key_count = key_count;
  resources.block_count = block_count;
  resources.low_word_pass_count =
      shader_abi::GaussianSortLowWordPassCount(candidate_count);
  resources.pass_count = resources.low_word_pass_count +
                         32U / shader_abi::kGaussianSortRadixBits;
  resources.reference_valid = false;
  resources.selected = true;
  return GaussianGpuStageSelection::Selected;
}

// The GPU checksum weights each sorted record by its position, so it
// matches only when both streams hold the same records in the same order.
void Renderer::Impl::ComputeGaussianSortReference(GaussianGpuSortFrameResources& resources) {
  if (!resources.selected) {
    return;
  }
  std::uint32_t checksum{};
  for (std::size_t i = 0; i < prepared_gaussians_.gaussians.size(); ++i) {
    const auto& gaussian = prepared_gaussians_.gaussians[i];
    checksum += static_cast<std::uint32_t>(i + 1U) *
                shader_abi::GaussianSortIdentityHash(
                    static_cast<std::uint32_t>(gaussian.resource),
                    static_cast<std::uint32_t>(gaussian.resource >> 32U),
                    gaussian.particle);
  }
  resources.reference_checksum = checksum;
  resources.reference_valid = true;
}

void Renderer::Impl::RecordGaussianGpuSort(VkCommandBuffer command,
    const FrameContext& frame) {
  const auto& resources = frame.gaussian_gpu_sort;
  if (!resources.selected) {
    return;
  }
  const auto& prepared = frame.gaussian_gpu_preparation;
  vkCmdFillBuffer(command, resources.scan.handle, 0,
      sizeof(shader_abi::GaussianSortVerification), 0U);
  vkCmdCopyBuffer(
      command, prepared.dispatch_counters.handle, resources.scan.handle,
      static_cast<std::uint32_t>(resources.visible_count_copies.size()),
      resources.visible_count_copies.data());
  // Prepared records come from the preparation dispatch; the control words
  // from the clear and visible-count copies above.
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
  const auto dispatch = [&](GaussianSortKernel kernel, std::size_t set,
                            const shader_abi::GaussianSortConstants& values,
                            std::uint32_t workgroups) {
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
        resources.pipelines[kernel]);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
        gaussian_sort_pipeline_layout_, 0, 1,
        &resources.descriptor_sets[set], 0, nullptr);
    vkCmdPushConstants(command, gaussian_sort_pipeline_layout_,
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(values),
        &values);
    vkCmdDispatch(command, workgroups, 1, 1);
    ++frame_counters_.gaussian_gpu_sort_dispatch_count;
  };

  // Set 1 writes keys[0], where the first radix pass reads.
  for (const auto& batch : resources.key_batches) {
    dispatch(kGaussianSortKeys, 1, batch.constants, batch.workgroup_count);
  }
  compute_barrier();
  const auto& levels = resources.scan_levels;
  for (std::uint32_t pass = 0; pass < resources.pass_count; ++pass) {
    const bool low_word = pass < resources.low_word_pass_count;
    const auto digit = low_word ? pass : pass - resources.low_word_pass_count;
    const std::size_t set = pass % 2U;
    shader_abi::GaussianSortConstants values;
    values.element_count = resources.key_count;
    values.block_count = resources.block_count;
    values.digit_shift = digit * shader_abi::kGaussianSortRadixBits;
    values.digit_word = low_word ? 0U : 1U;
    values.scan_offset = levels.front().offset;
    dispatch(kGaussianSortHistogram, set, values, resources.block_count);
    compute_barrier();
    for (const auto& level : levels) {
      values.scan_offset = level.offset;
      values.scan_count = level.count;
      values.scan_sums_offset = level.sums_offset;
      dispatch(kGaussianSortScanBlocks, set, values,
          shader_abi::GaussianSortScanWorkgroupCount(level.count));
      compute_barrier();
    }
    // The top level fits one workgroup, so its scan is already global.
    for (auto level = levels.rbegin() + 1; level != levels.rend(); ++level) {
      values.scan_offset = level->offset;
      values.scan_count = level->count;
      values.scan_sums_offset = level->sums_offset;
      dispatch(kGaussianSortScanAdd, set, values,
          shader_abi::GaussianSortScanWorkgroupCount(level->count));
      compute_barrier();
    }
    values.scan_offset = levels.front().offset;
    values.scan_count = 0;
    values.scan_sums_offset = 0;
    dispatch(kGaussianSortScatter, set, values, resources.block_count);
    compute_barrier();
  }
  shader_abi::GaussianSortConstants verify;
  verify.element_count = resources.key_count;
  dispatch(kGaussianSortVerify, resources.pass_count % 2U, verify,
      resources.block_count);
  frame_counters_.gaussian_gpu_sort_pass_count += resources.pass_count;
  frame_counters_.gaussian_gpu_sort_key_count += resources.key_count;

  VkMemoryBarrier output_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  output_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  output_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
      &output_barrier, 0, nullptr, 0, nullptr);
  const VkBufferCopy copy{0, 0,
      sizeof(shader_abi::GaussianSortVerification)};
  vkCmdCopyBuffer(command, resources.scan.handle,
      resources.verification_readback.handle, 1, &copy);
}

std::uint64_t Renderer::Impl::ReadGaussianGpuSortNanoseconds(
    const FrameContext& frame) const {
  return frame.gaussian_gpu_sort.selected
             ? ReadGpuTimestampSpanNanoseconds(frame, 4, 5)
             : 0;
}

void Renderer::Impl::ResolveGaussianGpuSortVerification(FrameContext& frame) {
  const auto& resources = frame.gaussian_gpu_sort;
  if (!resources.selected) {
    return;
  }
  shader_abi::GaussianSortVerification verification;
  void* mapped{};
  Check(vkMapMemory(device_, resources.verification_readback.memory, 0,
            sizeof(verification), 0, &mapped),
      "map Gaussian sort verification");
  std::memcpy(&verification, mapped, sizeof(verification));
  vkUnmapMemory(device_, resources.verification_readback.memory);
  if (verification.order_violation_count != 0U ||
      verification.key_mismatch_count != 0U) {
    throw RendererError(
        RendererErrorCode::BackendFailure,
        "resolve Gaussian sort verification",
        "sorted keys are out of order or disagree with their prepared "
        "records");
  }
  if (verification.sorted_count !=
      frame_counters_.gaussian_gpu_preparation_visible_count) {
    throw RendererError(RendererErrorCode::BackendFailure,
        "resolve Gaussian sort verification",
        "the sort did not retain every visible prepared "
        "record exactly once");
  }
  frame_counters_.gaussian_gpu_sorted_count = verification.sorted_count;
  if (!frame.gaussian_cpu_reference) {
    frame_counters_.gaussian_sorted_count = verification.sorted_count;
  }
  if (resources.reference_valid &&
      verification.identity_checksum != resources.reference_checksum) {
    ++frame_counters_.gaussian_gpu_sort_reference_divergence_count;
  }
}

} // namespace merlin::vulkan
