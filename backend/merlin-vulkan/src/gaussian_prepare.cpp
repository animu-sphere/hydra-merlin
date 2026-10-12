#include "renderer_impl.hpp"

namespace merlin::vulkan {

void Renderer::Impl::DestroyGaussianGpuPreparationFrameResources(
    GaussianGpuPreparationFrameResources& resources) noexcept {
  DestroyBuffer(resources.candidate_results);
  DestroyBuffer(resources.prepared_records);
  DestroyBuffer(resources.dispatch_counters);
  DestroyBuffer(resources.counter_readback);
  DestroyBuffer(resources.constants);
  if (resources.descriptor_pool != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(device_, resources.descriptor_pool, nullptr);
  }
  resources = {};
}

Renderer::Impl::GaussianGpuStageSelection Renderer::Impl::PrepareGaussianGpuPreparation(
    FrameContext& frame, const extraction::FrameSnapshot& snapshot,
    const RenderRequest& request) {
  auto& resources = frame.gaussian_gpu_preparation;
  resources.selected = false;
  resources.batches.clear();
  if (snapshot.gaussians.empty()) {
    return GaussianGpuStageSelection::NoWork;
  }
  if (request.gpu_driven_gaussian_preparation ==
      GpuDrivenGaussianPreparationMode::Disabled) {
    return GaussianGpuStageSelection::NotRequested;
  }
  const auto unavailable = [&](std::string detail) {
    if (request.gpu_driven_gaussian_preparation ==
        GpuDrivenGaussianPreparationMode::Require) {
      throw RendererError(RendererErrorCode::Unsupported,
          "select GPU-driven Gaussian preparation",
          std::move(detail));
    }
    ++frame_counters_.gaussian_gpu_preparation_fallback_count;
  };
  if (!capabilities_.compute_queue) {
    unavailable("the selected graphics queue does not support compute");
    return GaussianGpuStageSelection::Unavailable;
  }
  // Creating the pipeline layout below is a valid-usage violation, not a
  // recoverable error, on a device that guarantees only the Vulkan minimum
  // of four storage buffers. Preflight both limits so Prefer can still fall
  // back to the CPU reference.
  if (kGaussianPrepareStorageBufferCount >
          max_per_stage_descriptor_storage_buffers_ ||
      kGaussianPrepareStorageBufferCount >
          max_descriptor_set_storage_buffers_) {
    unavailable("the Gaussian preparation layout exceeds "
                "maxPerStageDescriptorStorageBuffers or "
                "maxDescriptorSetStorageBuffers");
    return GaussianGpuStageSelection::Unavailable;
  }

  struct Selection {
    const extraction::GaussianRecord* record{};
    VkDeviceSize candidate_offset{};
    VkDeviceSize prepared_offset{};
    VkDeviceSize counter_offset{};
    VkDeviceSize constants_offset{};
    std::uint32_t particle_count{};
  };
  std::vector<Selection> selections;
  VkDeviceSize candidate_bytes{};
  VkDeviceSize prepared_bytes{};
  VkDeviceSize counter_bytes{};
  VkDeviceSize constants_bytes{};
  for (const auto& record : snapshot.gaussians) {
    if (!record.visible || !record.positions || record.positions->empty()) {
      continue;
    }
    if (record.positions->size() >
        std::numeric_limits<std::uint32_t>::max()) {
      unavailable("a Gaussian resource exceeds the uint32 particle limit");
      return GaussianGpuStageSelection::Unavailable;
    }
    const auto particle_count =
        static_cast<std::uint32_t>(record.positions->size());
    if (shader_abi::GaussianPrepareWorkgroupCount(particle_count) >
        max_compute_work_group_count_x_) {
      unavailable(
          "a Gaussian resource exceeds maxComputeWorkGroupCount[0]");
      return GaussianGpuStageSelection::Unavailable;
    }
    const auto slot = gaussian_attribute_slots_.find(record.gaussian);
    if (slot == gaussian_attribute_slots_.end() ||
        !slot->second.positions.valid() ||
        !slot->second.covariances.valid() ||
        !slot->second.opacities.valid() ||
        !slot->second.radiance.valid()) {
      unavailable("persistent Gaussian attributes are unavailable");
      return GaussianGpuStageSelection::Unavailable;
    }
    const auto resource_candidate_bytes =
        static_cast<VkDeviceSize>(particle_count) * sizeof(std::uint32_t);
    const auto resource_prepared_bytes =
        static_cast<VkDeviceSize>(particle_count) *
        sizeof(shader_abi::GaussianPreparedRecord);
    if (slot->second.positions.size > max_storage_buffer_range_ ||
        slot->second.covariances.size > max_storage_buffer_range_ ||
        slot->second.opacities.size > max_storage_buffer_range_ ||
        slot->second.radiance.size > max_storage_buffer_range_ ||
        resource_candidate_bytes > max_storage_buffer_range_ ||
        resource_prepared_bytes > max_storage_buffer_range_) {
      unavailable("a Gaussian preparation binding exceeds "
                  "maxStorageBufferRange");
      return GaussianGpuStageSelection::Unavailable;
    }
    candidate_bytes =
        AlignUp(candidate_bytes, storage_buffer_alignment_);
    prepared_bytes = AlignUp(prepared_bytes, storage_buffer_alignment_);
    counter_bytes = AlignUp(counter_bytes, storage_buffer_alignment_);
    constants_bytes = AlignUp(constants_bytes, uniform_buffer_alignment_);
    selections.push_back({&record, candidate_bytes, prepared_bytes,
        counter_bytes, constants_bytes, particle_count});
    candidate_bytes += resource_candidate_bytes;
    prepared_bytes += resource_prepared_bytes;
    counter_bytes += sizeof(shader_abi::GaussianPrepareDispatchCounters);
    constants_bytes += sizeof(shader_abi::GaussianPrepareConstants);
  }
  if (selections.empty()) {
    return GaussianGpuStageSelection::NoWork;
  }

  VkPipeline compute_pipeline{};
  try {
    compute_pipeline = EnsureGaussianPrepareComputePipeline(request.shaders);
  } catch (const RendererError& error) {
    if (request.gpu_driven_gaussian_preparation ==
            GpuDrivenGaussianPreparationMode::Require ||
        error.code() == RendererErrorCode::DeviceLost ||
        error.code() == RendererErrorCode::ResourceExhausted ||
        error.code() == RendererErrorCode::Timeout) {
      throw;
    }
    unavailable("the packaged Gaussian preparation artifact is unavailable");
    return GaussianGpuStageSelection::Unavailable;
  }
  EnsureGaussianGpuPreparationFrameResources(
      resources, candidate_bytes, prepared_bytes, counter_bytes,
      constants_bytes, static_cast<std::uint32_t>(selections.size()));
  resources.compute_pipeline = compute_pipeline;
  resources.batches.resize(selections.size());
  // The dispatches feed one global sort, so every batch must key its records
  // in the same domain the CPU reference chose for this frame.
  const auto sorting_mode =
      detail::SelectGaussianSortingPolicy(snapshot).mode ==
              GaussianSortingMode::CameraDistance
          ? 1U
          : 0U;
  std::vector<shader_abi::GaussianPrepareConstants> constants(
      selections.size());
  for (std::size_t i = 0; i < selections.size(); ++i) {
    const auto& selection = selections[i];
    auto& batch = resources.batches[i];
    batch.descriptor_set = resources.descriptor_sets[i];
    batch.candidate_offset = selection.candidate_offset;
    batch.prepared_offset = selection.prepared_offset;
    batch.counter_offset = selection.counter_offset;
    batch.constants_offset = selection.constants_offset;
    batch.resource = selection.record->gaussian;
    batch.particle_count = selection.particle_count;

    auto& value = constants[i];
    value.local_to_camera = render::MultiplyForwardMatrices(snapshot.view,
        selection.record->transform);
    value.projection = snapshot.projection;
    value.viewport_size = {static_cast<float>(request.width),
        static_cast<float>(request.height)};
    value.resource_id_low =
        static_cast<std::uint32_t>(selection.record->gaussian);
    value.resource_id_high = static_cast<std::uint32_t>(
        selection.record->gaussian >> 32U);
    value.particle_count = selection.particle_count;
    value.coefficients_per_particle =
        (selection.record->spherical_harmonics_degree + 1U) *
        (selection.record->spherical_harmonics_degree + 1U);
    value.spherical_harmonics_degree =
        selection.record->spherical_harmonics_degree;
    value.projection_mode =
        selection.record->projection_mode ==
                GaussianProjectionMode::Tangential
            ? 1U
            : 0U;
    value.sorting_mode = sorting_mode;
  }
  void* mapped{};
  Check(vkMapMemory(device_, resources.constants.memory, 0,
            resources.constants.size, 0, &mapped),
      "map Gaussian preparation constants");
  for (std::size_t i = 0; i < selections.size(); ++i) {
    std::memcpy(static_cast<std::byte*>(mapped) +
                    selections[i].constants_offset,
        &constants[i], sizeof(constants[i]));
  }
  vkUnmapMemory(device_, resources.constants.memory);
  UpdateGaussianGpuPreparationDescriptors(resources);
  resources.selected = true;
  return GaussianGpuStageSelection::Selected;
}

void Renderer::Impl::EnsureGaussianGpuPreparationFrameResources(
    GaussianGpuPreparationFrameResources& resources,
    VkDeviceSize candidate_bytes, VkDeviceSize prepared_bytes,
    VkDeviceSize counter_bytes, VkDeviceSize constants_bytes,
    std::uint32_t batch_count) {
  if (resources.candidate_capacity_bytes >= candidate_bytes &&
      resources.prepared_capacity_bytes >= prepared_bytes &&
      resources.counter_capacity_bytes >= counter_bytes &&
      resources.constants_capacity_bytes >= constants_bytes &&
      resources.batch_capacity >= batch_count &&
      resources.descriptor_pool != VK_NULL_HANDLE) {
    return;
  }
  DestroyGaussianGpuPreparationFrameResources(resources);
  resources.candidate_capacity_bytes = candidate_bytes;
  resources.prepared_capacity_bytes = prepared_bytes;
  resources.counter_capacity_bytes = counter_bytes;
  resources.constants_capacity_bytes = constants_bytes;
  resources.batch_capacity = batch_count;
  resources.candidate_results = CreateBuffer(
      candidate_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  resources.prepared_records = CreateBuffer(
      prepared_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  resources.dispatch_counters = CreateBuffer(
      counter_bytes,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  resources.counter_readback = CreateBuffer(
      counter_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  resources.constants = CreateBuffer(
      constants_bytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  const std::array<VkDescriptorPoolSize, 2> pool_sizes{{
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, batch_count * 7U},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, batch_count},
  }};
  VkDescriptorPoolCreateInfo pool_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = batch_count;
  pool_info.poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size());
  pool_info.pPoolSizes = pool_sizes.data();
  Check(vkCreateDescriptorPool(device_, &pool_info, nullptr,
            &resources.descriptor_pool),
      "create Gaussian preparation descriptor pool");
  ++frame_counters_.descriptor_pool_creation_count;
  VkDescriptorSetAllocateInfo allocate{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorPool = resources.descriptor_pool;
  allocate.descriptorSetCount = batch_count;
  const std::vector layouts(batch_count,
      gaussian_prepare_descriptor_set_layout_);
  allocate.pSetLayouts = layouts.data();
  resources.descriptor_sets.resize(batch_count);
  Check(vkAllocateDescriptorSets(device_, &allocate,
            resources.descriptor_sets.data()),
      "allocate Gaussian preparation descriptor sets");
  frame_counters_.descriptor_allocation_count += batch_count;
}

void Renderer::Impl::UpdateGaussianGpuPreparationDescriptors(
    const GaussianGpuPreparationFrameResources& resources) {
  constexpr std::size_t kBindingCount = 8;
  std::vector<VkDescriptorBufferInfo> buffer_infos(
      resources.batches.size() * kBindingCount);
  std::vector<VkWriteDescriptorSet> writes(buffer_infos.size());
  for (std::size_t batch_index = 0;
      batch_index < resources.batches.size(); ++batch_index) {
    const auto& batch = resources.batches[batch_index];
    const auto& attributes = gaussian_attribute_slots_.at(batch.resource);
    const auto candidate_bytes =
        static_cast<VkDeviceSize>(batch.particle_count) *
        sizeof(std::uint32_t);
    const auto prepared_bytes =
        static_cast<VkDeviceSize>(batch.particle_count) *
        sizeof(shader_abi::GaussianPreparedRecord);
    const std::array<VkBuffer, kBindingCount> buffers{
        gaussian_position_arena_.buffer(attributes.positions.block),
        gaussian_covariance_arena_.buffer(attributes.covariances.block),
        gaussian_opacity_arena_.buffer(attributes.opacities.block),
        gaussian_radiance_arena_.buffer(attributes.radiance.block),
        resources.candidate_results.handle,
        resources.prepared_records.handle,
        resources.dispatch_counters.handle,
        resources.constants.handle,
    };
    const std::array<VkDeviceSize, kBindingCount> offsets{
        attributes.positions.offset,
        attributes.covariances.offset,
        attributes.opacities.offset,
        attributes.radiance.offset,
        batch.candidate_offset,
        batch.prepared_offset,
        batch.counter_offset,
        batch.constants_offset,
    };
    const std::array<VkDeviceSize, kBindingCount> ranges{
        attributes.positions.size,
        attributes.covariances.size,
        attributes.opacities.size,
        attributes.radiance.size,
        candidate_bytes,
        prepared_bytes,
        VkDeviceSize{sizeof(shader_abi::GaussianPrepareDispatchCounters)},
        VkDeviceSize{sizeof(shader_abi::GaussianPrepareConstants)},
    };
    const std::array<std::uint32_t, kBindingCount> bindings{
        shader_abi::kGaussianPositions.binding,
        shader_abi::kGaussianCovariances.binding,
        shader_abi::kGaussianOpacities.binding,
        shader_abi::kGaussianRadiance.binding,
        shader_abi::kGaussianCandidateResults.binding,
        shader_abi::kGaussianPreparedRecords.binding,
        shader_abi::kGaussianPrepareCounters.binding,
        shader_abi::kGaussianPrepareConstants.binding,
    };
    for (std::size_t binding_index = 0; binding_index < kBindingCount;
        ++binding_index) {
      const auto index = batch_index * kBindingCount + binding_index;
      buffer_infos[index] = {buffers[binding_index], offsets[binding_index],
          ranges[binding_index]};
      writes[index] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[index].dstSet = batch.descriptor_set;
      writes[index].dstBinding = bindings[binding_index];
      writes[index].descriptorCount = 1;
      writes[index].descriptorType =
          binding_index + 1U == kBindingCount
              ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
              : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[index].pBufferInfo = &buffer_infos[index];
    }
  }
  vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
      writes.data(), 0, nullptr);
  frame_counters_.descriptor_update_count += writes.size();
}

void Renderer::Impl::RecordGaussianGpuPreparation(VkCommandBuffer command,
    const FrameContext& frame) {
  const auto& resources = frame.gaussian_gpu_preparation;
  if (!resources.selected) {
    return;
  }
  for (const auto& batch : resources.batches) {
    vkCmdFillBuffer(command, resources.dispatch_counters.handle,
        batch.counter_offset,
        sizeof(shader_abi::GaussianPrepareDispatchCounters),
        0U);
  }
  VkMemoryBarrier counter_clear_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  counter_clear_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  counter_clear_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                        VK_ACCESS_SHADER_WRITE_BIT;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
      &counter_clear_barrier, 0, nullptr, 0, nullptr);
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
      resources.compute_pipeline);
  for (const auto& batch : resources.batches) {
    vkCmdBindDescriptorSets(
        command, VK_PIPELINE_BIND_POINT_COMPUTE,
        gaussian_prepare_pipeline_layout_, 3, 1, &batch.descriptor_set, 0,
        nullptr);
    vkCmdDispatch(command,
        shader_abi::GaussianPrepareWorkgroupCount(
            batch.particle_count),
        1, 1);
    ++frame_counters_.gaussian_gpu_preparation_dispatch_count;
    frame_counters_.gaussian_gpu_preparation_candidate_count +=
        batch.particle_count;
  }
  VkMemoryBarrier output_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  output_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  output_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
      &output_barrier, 0, nullptr, 0, nullptr);
  for (const auto& batch : resources.batches) {
    const VkBufferCopy copy{
        batch.counter_offset, batch.counter_offset,
        sizeof(shader_abi::GaussianPrepareDispatchCounters)};
    vkCmdCopyBuffer(command, resources.dispatch_counters.handle,
        resources.counter_readback.handle, 1, &copy);
  }
}

void Renderer::Impl::ResolveGaussianGpuPreparationCounters(FrameContext& frame) {
  const auto& resources = frame.gaussian_gpu_preparation;
  if (!resources.selected) {
    return;
  }
  std::uint64_t candidate_count{};
  std::uint64_t visible_count{};
  std::uint64_t opacity_culled_count{};
  std::uint64_t frustum_culled_count{};
  std::uint64_t invalid_culled_count{};
  void* mapped{};
  Check(vkMapMemory(device_, resources.counter_readback.memory, 0,
            resources.counter_readback.size, 0, &mapped),
      "map Gaussian preparation counters");
  for (const auto& batch : resources.batches) {
    shader_abi::GaussianPrepareDispatchCounters counters;
    std::memcpy(&counters,
        static_cast<const std::byte*>(mapped) +
            batch.counter_offset,
        sizeof(counters));
    if (counters.candidate_count != batch.particle_count ||
        static_cast<std::uint64_t>(counters.visible_count) +
                counters.opacity_culled_count +
                counters.frustum_culled_count +
                counters.invalid_culled_count !=
            counters.candidate_count) {
      vkUnmapMemory(device_, resources.counter_readback.memory);
      throw RendererError(
          RendererErrorCode::BackendFailure,
          "resolve Gaussian preparation counters",
          "compute counters violate a resource particle partition");
    }
    candidate_count += counters.candidate_count;
    visible_count += counters.visible_count;
    opacity_culled_count += counters.opacity_culled_count;
    frustum_culled_count += counters.frustum_culled_count;
    invalid_culled_count += counters.invalid_culled_count;
  }
  vkUnmapMemory(device_, resources.counter_readback.memory);
  if (candidate_count !=
      frame_counters_.gaussian_gpu_preparation_candidate_count) {
    throw RendererError(RendererErrorCode::BackendFailure,
        "resolve Gaussian preparation counters",
        "resource candidate counts do not match the frame");
  }
  frame_counters_.gaussian_gpu_preparation_visible_count = visible_count;
  frame_counters_.gaussian_gpu_preparation_opacity_culled_count =
      opacity_culled_count;
  frame_counters_.gaussian_gpu_preparation_frustum_culled_count =
      frustum_culled_count;
  frame_counters_.gaussian_gpu_preparation_invalid_culled_count =
      invalid_culled_count;
  // Without the CPU reference the frame's particle counters come from the
  // GPU partition; hidden resources are not dispatched, so their particles
  // join the candidates here.
  if (!frame.gaussian_cpu_reference) {
    frame_counters_.gaussian_candidate_count =
        candidate_count + frame_counters_.gaussian_hidden_count;
    frame_counters_.gaussian_visible_count = visible_count;
    frame_counters_.gaussian_opacity_culled_count = opacity_culled_count;
    frame_counters_.gaussian_frustum_culled_count = frustum_culled_count;
    frame_counters_.gaussian_invalid_culled_count = invalid_culled_count;
  }
}

} // namespace merlin::vulkan
