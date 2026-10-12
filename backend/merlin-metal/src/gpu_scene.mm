#include "backend_impl.hpp"

namespace merlin::metal {

void Backend::Impl::CreateGpuSceneBuffers(render::GpuScenePackingCapacities capacities) {
  gpu_scene_buffers_.capacities = capacities;
  gpu_scene_buffers_.geometries =
      CreateGpuSceneTable<render::GpuGeometry>(capacities.geometries,
          "geometry");
  gpu_scene_buffers_.instances =
      CreateGpuSceneTable<render::GpuInstance>(capacities.instances,
          "instance");
  gpu_scene_buffers_.materials =
      CreateGpuSceneTable<render::GpuMaterial>(capacities.materials,
          "material");
  gpu_scene_buffers_.draws =
      CreateGpuSceneTable<render::GpuDraw>(capacities.draws, "draw");
  metal_statistics_.gpu_scene_buffers = true;
  metal_statistics_.gpu_scene_capacity_bytes =
      gpu_scene_buffers_.geometries.length +
      gpu_scene_buffers_.instances.length +
      gpu_scene_buffers_.materials.length + gpu_scene_buffers_.draws.length;
}

void Backend::Impl::PrepareGpuSceneUpdate(
    const extraction::FrameSnapshot& snapshot,
    const render::GpuScenePackedFrameUpdate* update, FrameContext& frame,
    FrameBuild& build) {
  if (update == nullptr) {
    return;
  }
  const auto begin = Clock::now();
  if (!gpu_scene_buffers_.enabled()) {
    throw render::RendererError(
        render::RendererErrorCode::InvalidRequest, "upload Metal GPU Scene",
        "backend has no GPU Scene table capacities");
  }
  const auto validate_plan = [&](const auto& plan,
                                 std::string_view table_name) {
    if (plan.source_id != snapshot.source_id ||
        plan.revision != snapshot.revision) {
      throw render::RendererError(
          render::RendererErrorCode::InvalidRequest,
          "upload Metal GPU Scene",
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
    throw render::RendererError(
        render::RendererErrorCode::InvalidRequest, "upload Metal GPU Scene",
        "table plans do not share one revision boundary");
  }
  if (update->geometry_plan.table !=
          render::GpuSceneResourceTable::Geometry ||
      update->instance_plan.table !=
          render::GpuSceneResourceTable::Instance ||
      update->material_plan.table !=
          render::GpuSceneResourceTable::Material) {
    throw render::RendererError(
        render::RendererErrorCode::InvalidRequest, "upload Metal GPU Scene",
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
  if (!update->draw_slot_indices ||
      update->draw_slot_indices->size() != snapshot.draws.size()) {
    throw render::RendererError(
        render::RendererErrorCode::InvalidRequest, "upload Metal GPU Scene",
        "draw slot map does not match the request snapshot");
  }
  if (gpu_scene_buffers_.draw_slot_indices != update->draw_slot_indices) {
    std::vector<bool> mapped_draw_slots(capacities.draws);
    for (const auto slot : *update->draw_slot_indices) {
      if (slot >= capacities.draws || mapped_draw_slots[slot]) {
        throw render::RendererError(
            render::RendererErrorCode::InvalidRequest,
            "upload Metal GPU Scene",
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
    throw render::RendererError(
        render::RendererErrorCode::InvalidRequest, "upload Metal GPU Scene",
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
  const bool same_source = gpu_scene_buffers_.has_resident_update &&
                           gpu_scene_buffers_.source_id ==
                               reference_plan.source_id;
  const bool unchanged =
      same_source && gpu_scene_buffers_.revision == reference_plan.revision;
  const bool continuous = same_source &&
                          gpu_scene_buffers_.revision ==
                              reference_plan.base_revision;
  if (!unchanged && !continuous && !complete_reconciliation) {
    throw render::RendererError(
        render::RendererErrorCode::InvalidRequest, "upload Metal GPU Scene",
        "update neither continues nor completely rebuilds the resident "
        "table revision");
  }
  if (unchanged && copy_bytes != 0 && !complete_reconciliation) {
    throw render::RendererError(
        render::RendererErrorCode::InvalidRequest, "upload Metal GPU Scene",
        "unchanged resident revision contains copy payload");
  }

  build.has_gpu_scene_update = true;
  build.gpu_scene_source_id = reference_plan.source_id;
  build.gpu_scene_revision = reference_plan.revision;
  build.gpu_scene_draw_slot_indices = update->draw_slot_indices;
  if (copy_bytes == 0) {
    build.upload_ns += DurationNs(begin, Clock::now());
    return;
  }
  if (copy_bytes > std::numeric_limits<NSUInteger>::max()) {
    throw render::RendererError(
        render::RendererErrorCode::InvalidRequest, "upload Metal GPU Scene",
        "staging payload is not representable by Metal");
  }
  const auto required = static_cast<NSUInteger>(copy_bytes);
  if (frame.gpu_scene_staging == nil ||
      frame.gpu_scene_staging.length < required) {
    id<MTLBuffer> staging =
        [device_ newBufferWithLength:required
                             options:MTLResourceStorageModeShared |
                                     MTLResourceCPUCacheModeWriteCombined];
    if (staging == nil) {
      throw render::RendererError(
          render::RendererErrorCode::ResourceExhausted,
          "upload Metal GPU Scene",
          "newBuffer returned nil for the per-frame staging buffer");
    }
    staging.label = @"hdMerlin GPU Scene staging";
    frame.gpu_scene_staging = staging;
    ++build.telemetry.allocation_count;
    build.telemetry.buffer_allocation_bytes += required;
    ++build.telemetry.gpu_scene_staging_growth_count;
    build.telemetry.gpu_scene_staging_growth_bytes += required;
    ++metal_statistics_.gpu_scene_staging_growth_count;
    std::uint64_t staging_capacity{};
    for (const auto& candidate : frames_) {
      if (candidate.gpu_scene_staging != nil) {
        staging_capacity += candidate.gpu_scene_staging.length;
      }
    }
    metal_statistics_.gpu_scene_staging_peak_capacity_bytes =
        std::max(metal_statistics_.gpu_scene_staging_peak_capacity_bytes,
            staging_capacity);
  }

  NSUInteger cursor{};
  const auto stage_table = [&]<typename Record>(
                               const render::GpuScenePackedUpdate<Record>& packed,
                               id<MTLBuffer> destination) {
    for (const auto& range : packed.ranges) {
      const auto bytes = static_cast<NSUInteger>(range.records.size() *
                                                 sizeof(Record));
      std::memcpy(static_cast<std::byte*>(frame.gpu_scene_staging.contents) +
                      cursor,
          range.records.data(), bytes);
      build.gpu_scene_copies.push_back(
          {frame.gpu_scene_staging, cursor, destination,
              static_cast<NSUInteger>(range.first_slot) * sizeof(Record),
              bytes});
      cursor += bytes;
    }
  };
  stage_table(update->geometries, gpu_scene_buffers_.geometries);
  stage_table(update->instances, gpu_scene_buffers_.instances);
  stage_table(update->materials, gpu_scene_buffers_.materials);
  stage_table(update->draws, gpu_scene_buffers_.draws);
  build.telemetry.gpu_scene_upload_bytes += copy_bytes;
  build.telemetry.gpu_scene_copy_range_count +=
      build.gpu_scene_copies.size();
  build.telemetry.gpu_scene_staging_reserved_bytes += copy_bytes;
  build.telemetry.upload_bytes += copy_bytes;
  build.upload_ns += DurationNs(begin, Clock::now());
}

void Backend::Impl::EncodeGpuSceneUpdate(id<MTLCommandBuffer> command,
    const FrameBuild& build) {
  if (build.gpu_scene_copies.empty()) {
    return;
  }
  id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
  if (encoder == nil) {
    throw render::RendererError(
        render::RendererErrorCode::BackendFailure,
        "upload Metal GPU Scene", "blitCommandEncoder returned nil");
  }
  encoder.label = @"hdMerlin GPU Scene dirty ranges";
  for (const auto& copy : build.gpu_scene_copies) {
    [encoder copyFromBuffer:copy.source
               sourceOffset:copy.source_offset
                   toBuffer:copy.destination
          destinationOffset:copy.destination_offset
                       size:copy.size];
  }
  [encoder endEncoding];
}

void Backend::Impl::CommitGpuSceneUpdate(const FrameBuild& build) noexcept {
  if (!build.has_gpu_scene_update) {
    return;
  }
  gpu_scene_buffers_.source_id = build.gpu_scene_source_id;
  gpu_scene_buffers_.revision = build.gpu_scene_revision;
  gpu_scene_buffers_.draw_slot_indices =
      build.gpu_scene_draw_slot_indices;
  gpu_scene_buffers_.has_resident_update = true;
}

void Backend::Impl::InvalidateGpuSceneUpdate() noexcept {
  // A failed command buffer may have applied only part of its blit copies.
  // Reject incremental and zero-copy updates until a complete reconciliation
  // restores every GPU Scene table.
  gpu_scene_buffers_.source_id = 0;
  gpu_scene_buffers_.revision = 0;
  gpu_scene_buffers_.draw_slot_indices.reset();
  gpu_scene_buffers_.has_resident_update = false;
}

} // namespace merlin::metal
