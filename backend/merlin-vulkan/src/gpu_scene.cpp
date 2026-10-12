#include "renderer_impl.hpp"

namespace merlin::vulkan {

void Renderer::Impl::CreateGpuSceneBuffers(
    render::GpuScenePackingCapacities capacities) {
  gpu_scene_buffers_.capacities = capacities;
  gpu_scene_buffers_.geometries =
      CreateGpuSceneTable<render::GpuGeometry>(capacities.geometries);
  gpu_scene_buffers_.instances =
      CreateGpuSceneTable<render::GpuInstance>(capacities.instances);
  gpu_scene_buffers_.materials =
      CreateGpuSceneTable<render::GpuMaterial>(capacities.materials);
  gpu_scene_buffers_.draws =
      CreateGpuSceneTable<render::GpuDraw>(capacities.draws);
}

void Renderer::Impl::DestroyGpuSceneBuffers() noexcept {
  DestroyBuffer(gpu_scene_buffers_.geometries);
  DestroyBuffer(gpu_scene_buffers_.instances);
  DestroyBuffer(gpu_scene_buffers_.materials);
  DestroyBuffer(gpu_scene_buffers_.draws);
  gpu_scene_buffers_.capacities = {};
  gpu_scene_buffers_.source_id = 0;
  gpu_scene_buffers_.revision = 0;
  gpu_scene_buffers_.draw_slot_indices.reset();
  gpu_scene_buffers_.pending_draw_slot_indices.reset();
  gpu_scene_buffers_.has_resident_update = false;
  gpu_scene_buffers_.pending_update = false;
}

void Renderer::Impl::DestroyGpuDrivenFrameResources(
    GpuDrivenFrameResources& resources) noexcept {
  DestroyBuffer(resources.candidate_draw_slots);
  DestroyBuffer(resources.candidate_results);
  DestroyBuffer(resources.indirect_commands);
  DestroyBuffer(resources.dispatch_counters);
  DestroyBuffer(resources.counter_readback);
  if (resources.descriptor_pool != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(device_, resources.descriptor_pool, nullptr);
  }
  resources = {};
}

void Renderer::Impl::StageGpuSceneUpdate(
    const extraction::FrameSnapshot& snapshot,
    const render::GpuScenePackedFrameUpdate* update) {
  if (update == nullptr) {
    return;
  }
  if (!gpu_scene_buffers_.enabled()) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        "upload GPU Scene",
        "renderer has no GPU Scene table capacities");
  }
  const auto validate_plan = [&](const auto& plan,
                                 std::string_view table_name) {
    if (plan.source_id != snapshot.source_id ||
        plan.revision != snapshot.revision) {
      throw RendererError(RendererErrorCode::InvalidRequest,
          "upload GPU Scene",
          std::string(table_name) +
              " update was not packed from the request snapshot");
    }
  };
  validate_plan(update->geometry_plan, "geometry");
  validate_plan(update->instance_plan, "instance");
  validate_plan(update->material_plan, "material");
  validate_plan(update->draw_plan, "draw");
  const auto& reference_plan = update->geometry_plan;
  const auto same_plan_boundary = [&](const auto& plan) {
    return plan.source_id == reference_plan.source_id &&
           plan.base_revision == reference_plan.base_revision &&
           plan.revision == reference_plan.revision;
  };
  if (!same_plan_boundary(update->instance_plan) ||
      !same_plan_boundary(update->material_plan) ||
      !same_plan_boundary(update->draw_plan)) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        "upload GPU Scene",
        "table plans do not share one revision boundary");
  }
  if (update->geometry_plan.table !=
          render::GpuSceneResourceTable::Geometry ||
      update->instance_plan.table !=
          render::GpuSceneResourceTable::Instance ||
      update->material_plan.table !=
          render::GpuSceneResourceTable::Material) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        "upload GPU Scene",
        "resource update plans name the wrong tables");
  }

  const auto capacities = gpu_scene_buffers_.capacities;
  ValidateGpuSceneTableUpdate(update->geometries,
      update->geometry_plan.dirty_ranges,
      capacities.geometries, "geometry");
  ValidateGpuSceneTableUpdate(update->instances,
      update->instance_plan.dirty_ranges,
      capacities.instances, "instance");
  ValidateGpuSceneTableUpdate(update->materials,
      update->material_plan.dirty_ranges,
      capacities.materials, "material");
  ValidateGpuSceneTableUpdate(update->draws,
      update->draw_plan.dirty_ranges,
      capacities.draws, "draw");
  for (const auto& range : update->materials.ranges) {
    for (const auto& material : range.records) {
      const bool has_texture = material.base_color_texture_index !=
                               render::kInvalidGpuSceneTableIndex;
      const bool has_sampler = material.base_color_sampler_index !=
                               render::kInvalidGpuSceneTableIndex;
      if (has_texture != has_sampler) {
        throw RendererError(
            RendererErrorCode::InvalidRequest, "upload GPU Scene",
            "material texture and sampler table references are incomplete");
      }
      if (has_texture && bindless_texture_table_ &&
          (material.base_color_texture_index >=
                  bindless_texture_views_.size() ||
              material.base_color_sampler_index >= bindless_samplers_.size())) {
        throw RendererError(
            RendererErrorCode::InvalidRequest, "upload GPU Scene",
            "material texture or sampler reference exceeds bindless capacity");
      }
    }
  }
  for (const auto& range : update->draws.ranges) {
    for (const auto& draw : range.records) {
      if (draw.geometry_index >= capacities.geometries ||
          draw.instance_index >= capacities.instances ||
          draw.material_index >= capacities.materials) {
        throw RendererError(
            RendererErrorCode::InvalidRequest, "upload GPU Scene",
            "draw record contains an out-of-capacity table reference");
      }
    }
  }
  if (!update->draw_slot_indices ||
      update->draw_slot_indices->size() != snapshot.draws.size()) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        "upload GPU Scene",
        "draw slot map does not match the request snapshot");
  }
  if (gpu_scene_buffers_.draw_slot_indices != update->draw_slot_indices) {
    std::vector<bool> mapped_draw_slots(capacities.draws);
    for (const auto slot : *update->draw_slot_indices) {
      if (slot >= capacities.draws || mapped_draw_slots[slot]) {
        throw RendererError(
            RendererErrorCode::InvalidRequest, "upload GPU Scene",
            "draw slot map contains an invalid or duplicate slot");
      }
      mapped_draw_slots[slot] = true;
    }
  }
  const auto copy_bytes = update->geometries.copy_bytes +
                          update->instances.copy_bytes +
                          update->materials.copy_bytes +
                          update->draws.copy_bytes;
  if (update->copy_bytes != copy_bytes) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        "upload GPU Scene",
        "frame copy bytes do not match the table payloads");
  }
  const bool complete_reconciliation =
      update->geometry_plan.full_reconciliation &&
      update->instance_plan.full_reconciliation &&
      update->material_plan.full_reconciliation &&
      update->draw_plan.full_reconciliation &&
      update->geometries.record_count == snapshot.geometries.size() &&
      update->instances.record_count == snapshot.instances.size() &&
      update->materials.record_count == snapshot.materials.size() &&
      update->draws.record_count == snapshot.draws.size();
  const bool same_source =
      gpu_scene_buffers_.has_resident_update &&
      gpu_scene_buffers_.source_id == reference_plan.source_id;
  const bool unchanged =
      same_source && gpu_scene_buffers_.revision == reference_plan.revision;
  const bool continuous = same_source &&
                          gpu_scene_buffers_.revision ==
                              reference_plan.base_revision;
  if (!unchanged && !continuous && !complete_reconciliation) {
    throw RendererError(
        RendererErrorCode::InvalidRequest, "upload GPU Scene",
        "update neither continues nor completely rebuilds the resident "
        "table revision");
  }
  if (unchanged && copy_bytes != 0 && !complete_reconciliation) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        "upload GPU Scene",
        "unchanged resident revision contains copy payload");
  }
  gpu_scene_buffers_.pending_source_id = reference_plan.source_id;
  gpu_scene_buffers_.pending_revision = reference_plan.revision;
  gpu_scene_buffers_.pending_draw_slot_indices = update->draw_slot_indices;
  gpu_scene_buffers_.pending_update = true;
  if (copy_bytes == 0) {
    return;
  }

  Buffer retired_staging;
  VkDeviceSize growth_bytes{};
  const auto reservation = gpu_scene_staging_.Reserve(
      static_cast<VkDeviceSize>(copy_bytes), retired_staging, growth_bytes);
  frame_counters_.gpu_scene_upload_ring_reserved_bytes +=
      AlignUp(static_cast<VkDeviceSize>(copy_bytes), kArenaAlignment);
  if (growth_bytes != 0) {
    ++frame_counters_.allocation_count;
    ++frame_counters_.buffer_allocation_count;
    frame_counters_.buffer_allocation_bytes += copy_bytes;
    ++frame_counters_.gpu_scene_upload_ring_growth_count;
    frame_counters_.gpu_scene_upload_ring_growth_bytes += growth_bytes;
    Retire(retired_staging);
  }

  VkDeviceSize cursor{};
  const auto stage_table = [&]<typename Record>(
                               const render::GpuScenePackedUpdate<Record>& packed,
                               const Buffer& destination) {
    for (const auto& range : packed.ranges) {
      const auto bytes = static_cast<VkDeviceSize>(range.records.size()) *
                         sizeof(Record);
      std::memcpy(reservation.mapped + cursor, range.records.data(),
          static_cast<std::size_t>(bytes));
      pending_copies_.push_back(
          {reservation.buffer, reservation.offset + cursor,
              destination.handle,
              static_cast<VkDeviceSize>(range.first_slot) * sizeof(Record),
              bytes});
      cursor += bytes;
      ++frame_counters_.gpu_scene_copy_range_count;
    }
  };
  stage_table(update->geometries, gpu_scene_buffers_.geometries);
  stage_table(update->instances, gpu_scene_buffers_.instances);
  stage_table(update->materials, gpu_scene_buffers_.materials);
  stage_table(update->draws, gpu_scene_buffers_.draws);
  frame_counters_.gpu_scene_upload_bytes += copy_bytes;
  frame_counters_.upload_bytes += copy_bytes;
}

void Renderer::Impl::CommitGpuSceneUpdate() noexcept {
  if (!gpu_scene_buffers_.pending_update) {
    return;
  }
  gpu_scene_buffers_.source_id = gpu_scene_buffers_.pending_source_id;
  gpu_scene_buffers_.revision = gpu_scene_buffers_.pending_revision;
  gpu_scene_buffers_.draw_slot_indices =
      std::move(gpu_scene_buffers_.pending_draw_slot_indices);
  gpu_scene_buffers_.has_resident_update = true;
  gpu_scene_buffers_.pending_update = false;
}

void Renderer::Impl::PrepareGpuDrivenIndexed(FrameContext& frame,
    const extraction::FrameSnapshot& snapshot,
    const RenderRequest& request) {
  auto& resources = frame.gpu_driven;
  resources.selected = false;
  resources.candidate_count = 0;
  resources.pending_batch_snapshot.reset();
  resources.pending_batch_draw_slots.reset();
  for (auto& batch : resources.batches) {
    batch.candidate_upload_pending = false;
  }
  if (request.gpu_driven_indexed.mode == GpuDrivenIndexedMode::Disabled ||
      draw_records_.size() == 0) {
    return;
  }
  const auto unavailable = [&](std::string detail) {
    if (request.gpu_driven_indexed.mode == GpuDrivenIndexedMode::Require) {
      throw RendererError(RendererErrorCode::Unsupported,
          "select GPU-driven indexed Forward",
          std::move(detail));
    }
    ++frame_counters_.gpu_driven_fallback_count;
  };
  if (!capabilities_.draw_indirect_first_instance) {
    unavailable("drawIndirectFirstInstance was not enabled");
    return;
  }
  if (!capabilities_.draw_indirect_count) {
    unavailable("drawIndirectCount was not enabled");
    return;
  }
  if (!capabilities_.shader_draw_parameters) {
    unavailable("shaderDrawParameters was not enabled");
    return;
  }
  if (!capabilities_.compute_queue) {
    unavailable("the selected graphics queue does not support compute");
    return;
  }
  const auto draw_slots =
      gpu_scene_buffers_.pending_update
          ? gpu_scene_buffers_.pending_draw_slot_indices
          : (gpu_scene_buffers_.has_resident_update &&
                        gpu_scene_buffers_.source_id == snapshot.source_id &&
                        gpu_scene_buffers_.revision == snapshot.revision
                    ? gpu_scene_buffers_.draw_slot_indices
                    : nullptr);
  if (!bindless_texture_table_ || !draw_slots ||
      draw_slots->size() != draw_records_.size()) {
    unavailable("persistent bindless GPU Scene state is unavailable");
    return;
  }
  if (std::any_of(selected_material_artifacts_.begin(),
          selected_material_artifacts_.end(),
          [](const auto* artifact) { return artifact != nullptr; })) {
    unavailable("generated material pipelines require conventional submission");
    return;
  }

  const auto& cached = resources.batch_snapshot;
  if (cached && cached->source_id == snapshot.source_id &&
      cached->draws.table_identity() == snapshot.draws.table_identity() &&
      cached->geometries.table_identity() ==
          snapshot.geometries.table_identity() &&
      cached->materials.table_identity() ==
          snapshot.materials.table_identity() &&
      cached->front_face == snapshot.front_face &&
      resources.batch_draw_slots == draw_slots &&
      resources.geometry_generation == geometry_residency_generation_) {
    const auto compute_pipeline =
        EnsureGpuDrivenComputePipeline(request.shaders);
    for (auto& batch : resources.batches) {
      batch.compute_pipeline = compute_pipeline;
    }
    frame_counters_.gpu_driven_candidate_draw_count = draw_slots->size();
    resources.candidate_count =
        static_cast<std::uint32_t>(draw_slots->size());
    resources.selected = true;
    return;
  }
  // Any failure while replacing batches must leave the old cache unusable.
  resources.batch_snapshot.reset();
  resources.batch_draw_slots.reset();

  constexpr auto pipeline_state_mask =
      kMaskedAlphaFlag | kDoubleSidedFlag |
      kCounterClockwiseFrontFaceFlag;

  struct BatchSelection {
    std::uint32_t vertex_block{};
    std::uint32_t index_block{};
    std::uint32_t pipeline_variant{};
    VkDeviceSize candidate_offset{};
    VkDeviceSize command_offset{};
    VkDeviceSize counter_offset{};
    std::vector<std::uint32_t> draw_slots;
  };
  std::vector<BatchSelection> selections;
  selections.reserve(draw_records_.size());
  for (std::size_t i = 0; i < draw_records_.size(); ++i) {
    ++frame_counters_.mesh_cpu_draw_visit_count;
    const auto& draw = draw_records_[i];
    const auto& geometry = geometry_records_[draw.geometry_index];
    const auto& slot = geometry_slots_.at(geometry.mesh);
    const auto variant = MakeDrawPipelineVariant(draw, snapshot);
    const auto masked_variant = variant.variant_key & pipeline_state_mask;
    if (selections.empty() ||
        selections.back().vertex_block != slot.vertices.block ||
        selections.back().index_block != slot.indices.block ||
        (selections.back().pipeline_variant & pipeline_state_mask) !=
            masked_variant) {
      selections.push_back({slot.vertices.block, slot.indices.block,
          variant.variant_key, 0, 0, 0, {}});
    }
    selections.back().draw_slots.push_back((*draw_slots)[i]);
  }
  const auto oversized_batch =
      std::any_of(selections.begin(), selections.end(),
          [&](const auto& batch) {
            return batch.draw_slots.size() >
                   max_draw_indirect_count_;
          });
  if (oversized_batch) {
    unavailable("an arena/pipeline batch exceeds maxDrawIndirectCount");
    return;
  }
  const auto excessive_compute_batch =
      std::any_of(selections.begin(), selections.end(),
          [&](const auto& batch) {
            return shader_abi::GpuDrivenIndexedWorkgroupCount(
                       static_cast<std::uint32_t>(
                           batch.draw_slots.size())) >
                   max_compute_work_group_count_x_;
          });
  if (excessive_compute_batch) {
    unavailable(
        "an arena/pipeline batch exceeds maxComputeWorkGroupCount[0]");
    return;
  }

  VkDeviceSize candidate_buffer_bytes{};
  VkDeviceSize command_buffer_bytes{};
  VkDeviceSize counter_buffer_bytes{};
  bool unsupported_batch_range{};
  for (auto& selection : selections) {
    const auto candidate_bytes =
        static_cast<VkDeviceSize>(selection.draw_slots.size()) *
        sizeof(std::uint32_t);
    const auto command_bytes =
        static_cast<VkDeviceSize>(selection.draw_slots.size()) *
        sizeof(render::GpuIndexedIndirectCommand);
    unsupported_batch_range =
        unsupported_batch_range ||
        candidate_bytes > max_storage_buffer_range_ ||
        command_bytes > max_storage_buffer_range_;
    candidate_buffer_bytes =
        AlignUp(candidate_buffer_bytes, storage_buffer_alignment_);
    command_buffer_bytes =
        AlignUp(command_buffer_bytes, storage_buffer_alignment_);
    counter_buffer_bytes =
        AlignUp(counter_buffer_bytes, storage_buffer_alignment_);
    selection.candidate_offset = candidate_buffer_bytes;
    selection.command_offset = command_buffer_bytes;
    selection.counter_offset = counter_buffer_bytes;
    candidate_buffer_bytes += candidate_bytes;
    command_buffer_bytes += command_bytes;
    counter_buffer_bytes +=
        sizeof(shader_abi::GpuDrivenIndexedDispatchCounters);
  }
  if (unsupported_batch_range) {
    unavailable("an arena/pipeline batch exceeds maxStorageBufferRange");
    return;
  }

  const auto compute_pipeline =
      EnsureGpuDrivenComputePipeline(request.shaders);
  EnsureGpuDrivenFrameResources(
      resources, candidate_buffer_bytes, command_buffer_bytes,
      counter_buffer_bytes,
      static_cast<std::uint32_t>(selections.size()));
  resources.batches.resize(selections.size());
  VkDeviceSize candidate_upload_bytes{};
  bool descriptor_update_required{};
  for (std::size_t i = 0; i < selections.size(); ++i) {
    auto& batch = resources.batches[i];
    const auto& selection = selections[i];
    if (batch.candidate_draw_slot_shadow != selection.draw_slots ||
        batch.candidate_draw_slot_shadow_offset !=
            selection.candidate_offset) {
      candidate_upload_bytes +=
          static_cast<VkDeviceSize>(selection.draw_slots.size()) *
          sizeof(std::uint32_t);
      batch.candidate_upload_pending = true;
    }
    descriptor_update_required =
        descriptor_update_required ||
        batch.descriptor_set != resources.descriptor_sets[i] ||
        batch.candidate_offset != selection.candidate_offset ||
        batch.command_offset != selection.command_offset ||
        batch.counter_offset != selection.counter_offset ||
        batch.candidate_count !=
            static_cast<std::uint32_t>(selection.draw_slots.size());
    batch.candidate_count =
        static_cast<std::uint32_t>(selection.draw_slots.size());
    batch.descriptor_set = resources.descriptor_sets[i];
    batch.candidate_offset = selection.candidate_offset;
    batch.command_offset = selection.command_offset;
    batch.counter_offset = selection.counter_offset;
    batch.vertex_block = selection.vertex_block;
    batch.index_block = selection.index_block;
    batch.pipeline_variant = selection.pipeline_variant;
    batch.compute_pipeline = compute_pipeline;
  }
  if (descriptor_update_required) {
    UpdateGpuDrivenBatchDescriptors(resources);
  }
  if (candidate_upload_bytes != 0) {
    Buffer retired_staging;
    VkDeviceSize growth_bytes{};
    const auto reservation = gpu_driven_staging_.Reserve(
        candidate_upload_bytes, retired_staging, growth_bytes);
    if (growth_bytes != 0) {
      ++frame_counters_.allocation_count;
      ++frame_counters_.buffer_allocation_count;
      frame_counters_.buffer_allocation_bytes += growth_bytes;
      Retire(retired_staging);
    }
    VkDeviceSize source_offset{};
    for (std::size_t i = 0; i < selections.size(); ++i) {
      auto& batch = resources.batches[i];
      const auto& draw_slot_selection = selections[i].draw_slots;
      if (!batch.candidate_upload_pending) {
        continue;
      }
      const auto bytes =
          static_cast<VkDeviceSize>(draw_slot_selection.size()) *
          sizeof(std::uint32_t);
      std::memcpy(reservation.mapped + source_offset,
          draw_slot_selection.data(),
          static_cast<std::size_t>(bytes));
      pending_copies_.push_back(
          {reservation.buffer, reservation.offset + source_offset,
              resources.candidate_draw_slots.handle, batch.candidate_offset,
              bytes});
      batch.pending_candidate_draw_slot_shadow = draw_slot_selection;
      batch.pending_candidate_draw_slot_shadow_offset =
          batch.candidate_offset;
      source_offset += bytes;
    }
    frame_counters_.gpu_driven_candidate_upload_bytes =
        candidate_upload_bytes;
    frame_counters_.upload_bytes += candidate_upload_bytes;
  }
  frame_counters_.gpu_driven_candidate_draw_count = draw_slots->size();
  resources.candidate_count =
      static_cast<std::uint32_t>(draw_slots->size());
  resources.pending_batch_snapshot = std::make_shared<GpuDrivenBatchSource>(
      snapshot.source_id, snapshot.draws, snapshot.geometries,
      snapshot.materials, snapshot.front_face);
  resources.pending_batch_draw_slots = draw_slots;
  resources.geometry_generation = geometry_residency_generation_;
  resources.selected = true;
}

void Renderer::Impl::CommitGpuDrivenCandidates(FrameContext& frame) noexcept {
  auto& resources = frame.gpu_driven;
  for (auto& batch : resources.batches) {
    if (!batch.candidate_upload_pending) {
      continue;
    }
    batch.candidate_draw_slot_shadow =
        std::move(batch.pending_candidate_draw_slot_shadow);
    batch.candidate_draw_slot_shadow_offset =
        batch.pending_candidate_draw_slot_shadow_offset;
    batch.candidate_upload_pending = false;
  }
  if (resources.pending_batch_snapshot) {
    resources.batch_snapshot = std::move(resources.pending_batch_snapshot);
    resources.batch_draw_slots = std::move(resources.pending_batch_draw_slots);
  }
}

void Renderer::Impl::InvalidateGpuDrivenCandidates(FrameContext& frame) noexcept {
  auto& resources = frame.gpu_driven;
  resources.batch_snapshot.reset();
  resources.pending_batch_snapshot.reset();
  resources.batch_draw_slots.reset();
  resources.pending_batch_draw_slots.reset();
  for (auto& batch : resources.batches) {
    batch.candidate_draw_slot_shadow.clear();
    batch.pending_candidate_draw_slot_shadow.clear();
    batch.candidate_draw_slot_shadow_offset = 0;
    batch.pending_candidate_draw_slot_shadow_offset = 0;
    batch.candidate_upload_pending = false;
  }
}

bool Renderer::Impl::HasPendingGpuDrivenCandidateUpload(
    const FrameContext& frame) noexcept {
  return std::any_of(
      frame.gpu_driven.batches.begin(), frame.gpu_driven.batches.end(),
      [](const auto& batch) { return batch.candidate_upload_pending; });
}

void Renderer::Impl::InvalidateGpuSceneUpdate() noexcept {
  gpu_scene_buffers_.source_id = 0;
  gpu_scene_buffers_.revision = 0;
  gpu_scene_buffers_.pending_source_id = 0;
  gpu_scene_buffers_.pending_revision = 0;
  gpu_scene_buffers_.draw_slot_indices.reset();
  gpu_scene_buffers_.pending_draw_slot_indices.reset();
  gpu_scene_buffers_.has_resident_update = false;
  gpu_scene_buffers_.pending_update = false;
}

void Renderer::Impl::EnsureGpuDrivenFrameResources(GpuDrivenFrameResources& resources,
    VkDeviceSize candidate_bytes,
    VkDeviceSize command_bytes,
    VkDeviceSize counter_bytes,
    std::uint32_t batch_count) {
  if (resources.candidate_capacity_bytes >= candidate_bytes &&
      resources.command_capacity_bytes >= command_bytes &&
      resources.counter_capacity_bytes >= counter_bytes &&
      resources.batch_capacity >= batch_count &&
      resources.descriptor_pool != VK_NULL_HANDLE) {
    return;
  }
  DestroyGpuDrivenFrameResources(resources);
  resources.candidate_capacity_bytes =
      std::max<VkDeviceSize>(candidate_bytes, 1U);
  resources.command_capacity_bytes =
      std::max<VkDeviceSize>(command_bytes, 1U);
  resources.counter_capacity_bytes =
      std::max<VkDeviceSize>(counter_bytes, 1U);
  resources.batch_capacity = std::max(batch_count, 1U);
  resources.candidate_draw_slots = CreateBuffer(
      resources.candidate_capacity_bytes,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, queue_family_,
      transfer_queue_family_);
  resources.candidate_results = CreateBuffer(
      resources.candidate_capacity_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  resources.indirect_commands = CreateBuffer(
      resources.command_capacity_bytes,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  resources.dispatch_counters = CreateBuffer(
      resources.counter_capacity_bytes,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  resources.counter_readback = CreateBuffer(
      resources.counter_capacity_bytes,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  const VkDescriptorPoolSize pool_size{
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, resources.batch_capacity * 4U};
  VkDescriptorPoolCreateInfo pool_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = resources.batch_capacity;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  Check(vkCreateDescriptorPool(device_, &pool_info, nullptr,
            &resources.descriptor_pool),
      "create GPU-driven descriptor pool");
  ++frame_counters_.descriptor_pool_creation_count;
  VkDescriptorSetAllocateInfo allocate{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorPool = resources.descriptor_pool;
  allocate.descriptorSetCount = resources.batch_capacity;
  const std::vector layouts(resources.batch_capacity,
      gpu_driven_descriptor_set_layout_);
  allocate.pSetLayouts = layouts.data();
  resources.descriptor_sets.resize(resources.batch_capacity);
  Check(vkAllocateDescriptorSets(device_, &allocate,
            resources.descriptor_sets.data()),
      "allocate GPU-driven descriptor sets");
  frame_counters_.descriptor_allocation_count += resources.batch_capacity;
}

void Renderer::Impl::UpdateGpuDrivenBatchDescriptors(
    const GpuDrivenFrameResources& resources) {
  const std::array descriptor_buffers{
      &resources.candidate_draw_slots,
      &resources.candidate_results,
      &resources.indirect_commands,
      &resources.dispatch_counters,
  };
  const std::array descriptor_bindings{
      shader_abi::kGpuDrivenCandidateDrawSlots.binding,
      shader_abi::kGpuDrivenCandidateResults.binding,
      shader_abi::kGpuDrivenIndirectCommands.binding,
      shader_abi::kGpuDrivenDispatchCounters.binding,
  };
  std::vector<VkDescriptorBufferInfo> buffer_infos(
      resources.batches.size() * descriptor_buffers.size());
  std::vector<VkWriteDescriptorSet> writes(buffer_infos.size());
  for (std::size_t batch_index = 0;
      batch_index < resources.batches.size(); ++batch_index) {
    const auto& batch = resources.batches[batch_index];
    const auto slot_bytes = static_cast<VkDeviceSize>(batch.candidate_count) *
                            sizeof(std::uint32_t);
    const auto command_bytes =
        static_cast<VkDeviceSize>(batch.candidate_count) *
        sizeof(render::GpuIndexedIndirectCommand);
    const std::array offsets{batch.candidate_offset, batch.candidate_offset,
        batch.command_offset, batch.counter_offset};
    const std::array ranges{
        slot_bytes, slot_bytes, command_bytes,
        VkDeviceSize{sizeof(shader_abi::GpuDrivenIndexedDispatchCounters)}};
    for (std::size_t binding_index = 0;
        binding_index < descriptor_buffers.size(); ++binding_index) {
      const auto index =
          batch_index * descriptor_buffers.size() + binding_index;
      buffer_infos[index] = {descriptor_buffers[binding_index]->handle,
          offsets[binding_index], ranges[binding_index]};
      writes[index] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[index].dstSet = batch.descriptor_set;
      writes[index].dstBinding = descriptor_bindings[binding_index];
      writes[index].descriptorCount = 1;
      writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[index].pBufferInfo = &buffer_infos[index];
    }
  }
  vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
      writes.data(), 0, nullptr);
  frame_counters_.descriptor_update_count += writes.size();
}

void Renderer::Impl::RecordGpuDrivenDispatch(VkCommandBuffer command,
    const FrameContext& frame,
    const extraction::FrameSnapshot& snapshot,
    const GpuDrivenIndexedRequest& request) {
  if (!frame.gpu_driven.selected) {
    return;
  }
  for (const auto& batch : frame.gpu_driven.batches) {
    vkCmdFillBuffer(command, frame.gpu_driven.dispatch_counters.handle,
        batch.counter_offset,
        sizeof(shader_abi::GpuDrivenIndexedDispatchCounters),
        0U);
  }
  VkMemoryBarrier counter_clear_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  counter_clear_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  counter_clear_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                        VK_ACCESS_SHADER_WRITE_BIT;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
      &counter_clear_barrier, 0, nullptr, 0, nullptr);
  for (const auto& batch : frame.gpu_driven.batches) {
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
        batch.compute_pipeline);
    const std::array descriptor_sets{
        bindless_descriptor_set_, frame.bindless_material_descriptor_set,
        batch.descriptor_set};
    const std::uint32_t dynamic_offset{};
    vkCmdBindDescriptorSets(
        command, VK_PIPELINE_BIND_POINT_COMPUTE,
        gpu_driven_pipeline_layout_, 0,
        static_cast<std::uint32_t>(descriptor_sets.size()),
        descriptor_sets.data(), 1, &dynamic_offset);
    shader_abi::GpuDrivenIndexedConstants constants;
    constants.view_projection = render::MultiplyForwardMatrices(snapshot.projection, snapshot.view);
    constants.visibility_mask = request.visibility_mask;
    constants.candidate_count = batch.candidate_count;
    constants.flags = 0;
    if (request.enable_visibility_mask_culling) {
      constants.flags |= shader_abi::kGpuDrivenVisibilityMaskCulling;
    }
    if (request.enable_frustum_culling) {
      constants.flags |= shader_abi::kGpuDrivenFrustumCulling;
    }
    vkCmdPushConstants(command, gpu_driven_pipeline_layout_,
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
        &constants);
    const auto workgroup_count =
        shader_abi::GpuDrivenIndexedWorkgroupCount(batch.candidate_count);
    vkCmdDispatch(command, workgroup_count, 1, 1);
  }
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                          VK_ACCESS_SHADER_READ_BIT |
                          VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(
      command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
          VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
          VK_PIPELINE_STAGE_TRANSFER_BIT,
      0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void Renderer::Impl::ResolveGpuDrivenCounters(FrameContext& frame) {
  if (!frame.gpu_driven.selected) {
    return;
  }
  std::uint64_t candidate_count{};
  std::uint64_t visible_count{};
  std::uint64_t visibility_mask_culled_count{};
  std::uint64_t frustum_culled_count{};
  std::vector<shader_abi::GpuDrivenIndexedDispatchCounters> batch_counters(
      frame.gpu_driven.batches.size());
  void* mapped{};
  Check(vkMapMemory(device_, frame.gpu_driven.counter_readback.memory, 0,
            frame.gpu_driven.counter_readback.size, 0, &mapped),
      "map GPU-driven dispatch counters");
  for (std::size_t i = 0; i < frame.gpu_driven.batches.size(); ++i) {
    std::memcpy(&batch_counters[i],
        static_cast<const std::byte*>(mapped) +
            frame.gpu_driven.batches[i].counter_offset,
        sizeof(batch_counters[i]));
  }
  vkUnmapMemory(device_, frame.gpu_driven.counter_readback.memory);
  for (std::size_t i = 0; i < frame.gpu_driven.batches.size(); ++i) {
    const auto& batch = frame.gpu_driven.batches[i];
    const auto& counters = batch_counters[i];
    if (counters.candidate_count != batch.candidate_count ||
        static_cast<std::uint64_t>(counters.visible_count) +
                counters.visibility_mask_culled_count +
                counters.frustum_culled_count !=
            counters.candidate_count) {
      throw RendererError(
          RendererErrorCode::BackendFailure,
          "resolve GPU-driven dispatch counters",
          "compute counters violate an arena/pipeline batch partition");
    }
    candidate_count += counters.candidate_count;
    visible_count += counters.visible_count;
    visibility_mask_culled_count +=
        counters.visibility_mask_culled_count;
    frustum_culled_count += counters.frustum_culled_count;
  }
  if (candidate_count != frame.gpu_driven.candidate_count) {
    throw RendererError(RendererErrorCode::BackendFailure,
        "resolve GPU-driven dispatch counters",
        "batch candidate counts do not match the frame");
  }
  frame_counters_.gpu_driven_visible_draw_count = visible_count;
  frame_counters_.gpu_driven_visibility_mask_culled_count =
      visibility_mask_culled_count;
  frame_counters_.gpu_driven_frustum_culled_count =
      frustum_culled_count;
  frame_counters_.gpu_scene_draw_count = visible_count;
}

} // namespace merlin::vulkan
