#include "backend_impl.hpp"

namespace merlin::metal {

void Backend::Impl::PrepareGaussianExecution(const render::RenderRequest& request,
    id<MTLCommandBuffer> __strong& command, FrameContext& frame, FrameBuild& build) {
  const auto mode = request.gpu_driven_gaussian.mode;
  if (mode == render::GpuDrivenGaussianMode::Disabled) {
    PrepareGaussians(request, frame, build);
    return;
  }
  if (!capabilities_.gpu_driven_gaussian) {
    ++build.telemetry.gaussian_gpu_fallback_count;
    PrepareGaussians(request, frame, build);
    return;
  }
  const auto begin = Clock::now();
  try {
    auto attributes = gaussian_residency_->Prepare(*request.snapshot);
    gaussian_residency_->Encode(attributes, command);
    const auto uploaded = Clock::now();
    const bool wants_tiles = request.gpu_driven_gaussian.raster == render::GaussianRasterPath::Tiled;
    std::shared_ptr<const GaussianExecution::Frame> execution;
    try {
      execution = gaussian_execution_->Encode(attributes,
          request.snapshot->view, request.snapshot->projection,
          request.width, request.height, command, false, capabilities_.validation_enabled,
          frame.gaussian_timestamps, wants_tiles,
          request.gpu_driven_gaussian.tile_pair_capacity);
    } catch (const render::RendererError& error) {
      if (!wants_tiles || mode != render::GpuDrivenGaussianMode::Prefer ||
          (error.code() != render::RendererErrorCode::Unsupported &&
              error.code() != render::RendererErrorCode::ResourceExhausted))
        throw;
      // The tiled command is unsubmitted. Rebuild its attribute update in a
      // fresh command, then keep GPU sorted-stream execution.
      command = nil;
      command = [queue_ commandBuffer];
      if (!command)
        throw render::RendererError(render::RendererErrorCode::BackendFailure,
            "create Metal sorted fallback command", "commandBuffer returned nil");
      attributes = gaussian_residency_->Prepare(*request.snapshot);
      gaussian_residency_->Encode(attributes, command);
      execution = gaussian_execution_->Encode(attributes,
          request.snapshot->view, request.snapshot->projection,
          request.width, request.height, command, false, capabilities_.validation_enabled,
          frame.gaussian_timestamps);
      ++build.telemetry.gaussian_gpu_fallback_count;
    }
    // Read only bounded per-resource counters and draw arguments after all
    // compute stages. They never schedule GPU work from a CPU visible count.
    const auto tile_bytes = execution->tiled ? 44ULL : 0ULL;
    const auto bytes = 32ULL + tile_bytes + execution->resource_count * sizeof(gaussian_compute::PrepareCounters);
    if (!frame.gaussian_telemetry || frame.gaussian_telemetry.length < bytes) {
      auto buffer = [device_ newBufferWithLength:bytes options:MTLResourceStorageModeShared];
      if (!buffer)
        throw render::RendererError(render::RendererErrorCode::ResourceExhausted,
            "allocate Metal Gaussian telemetry", "newBuffer returned nil");
      frame.gaussian_telemetry = buffer;
      ++build.telemetry.allocation_count;
    }
    auto blit = [command blitCommandEncoder];
    if (!blit)
      throw render::RendererError(render::RendererErrorCode::BackendFailure,
          "copy Metal Gaussian telemetry", "blitCommandEncoder returned nil");
    [blit copyFromBuffer:execution->scratch->draw
             sourceOffset:0
                 toBuffer:frame.gaussian_telemetry
        destinationOffset:0
                     size:16];
    [blit copyFromBuffer:execution->scratch->control
             sourceOffset:0
                 toBuffer:frame.gaussian_telemetry
        destinationOffset:16
                     size:16];
    if (execution->tiled)
      [blit copyFromBuffer:execution->scratch->tile_control
               sourceOffset:0
                   toBuffer:frame.gaussian_telemetry
          destinationOffset:32
                       size:tile_bytes];
    if (execution->resource_count)
      [blit copyFromBuffer:execution->scratch->counters
               sourceOffset:0
                   toBuffer:frame.gaussian_telemetry
          destinationOffset:32 + tile_bytes
                       size:bytes - 32 - tile_bytes];
    [blit endEncoding];
    build.telemetry.readback_bytes += bytes;
    build.telemetry.allocation_count += attributes->allocation_count + execution->allocation_count;
    build.telemetry.gaussian_attribute_upload_bytes = attributes->upload_bytes;
    build.telemetry.gaussian_attribute_copy_range_count = attributes->upload_range_count;
    build.telemetry.gaussian_attribute_generation_count = attributes->generation_count;
    build.telemetry.upload_bytes += attributes->upload_bytes;
    build.telemetry.gaussian_candidate_count = execution->particle_count;
    build.telemetry.gaussian_sorting_policy_fallback_count = execution->sorting_policy.fallback_resource_count;
    for (const auto& resource : attributes->resources)
      if (!resource.record.visible)
        build.telemetry.gaussian_hidden_count += resource.record.positions->size();
    frame.gaussian_execution = std::move(execution);
    build.gaussian_attributes = std::move(attributes);
    build.gaussian_attribute_upload_ns = DurationNs(begin, uploaded);
    build.upload_ns += build.gaussian_attribute_upload_ns;
  } catch (const render::RendererError& error) {
    if (mode != render::GpuDrivenGaussianMode::Prefer ||
        (error.code() != render::RendererErrorCode::Unsupported &&
            error.code() != render::RendererErrorCode::ResourceExhausted))
      throw;
    // Discard all partially encoded uploads/compute, retaining the previous
    // committed scene. No other frame work has been encoded yet.
    command = nil;
    frame.gaussian_execution.reset();
    command = [queue_ commandBuffer];
    if (!command)
      throw render::RendererError(render::RendererErrorCode::BackendFailure,
          "create Metal fallback command", "commandBuffer returned nil");
    command.label = @"hdMerlin CPU Gaussian fallback frame";
    ++build.telemetry.gaussian_gpu_fallback_count;
    PrepareGaussians(request, frame, build);
  }
}

void Backend::Impl::ResolveGaussianTelemetry(const FrameContext& frame, render::RenderResult& result) {
  if (!frame.gaussian_execution)
    return;
  const auto* words = static_cast<const std::uint32_t*>(frame.gaussian_telemetry.contents);
  const auto& execution = *frame.gaussian_execution;
  auto& telemetry = result.telemetry;
  const auto* tile = execution.tiled ? words + 8 : nullptr;
  const auto* counters = reinterpret_cast<const gaussian_compute::PrepareCounters*>(
      words + 8 + (execution.tiled ? 11 : 0));
  for (std::uint32_t i = 0; i < execution.resource_count; ++i) {
    telemetry.gaussian_visible_count += counters[i].visible_count;
    telemetry.gaussian_opacity_culled_count += counters[i].opacity_culled_count;
    telemetry.gaussian_frustum_culled_count += counters[i].frustum_culled_count;
    telemetry.gaussian_invalid_culled_count += counters[i].invalid_culled_count;
  }
  const bool selected = tile && tile[10] != 0;
  if (words[0] != 6 || words[1] != (selected ? 0 : telemetry.gaussian_visible_count) ||
      words[1] > execution.particle_count || words[2] || words[3] ||
      (tile && (tile[0] != telemetry.gaussian_visible_count || tile[10] > 1 ||
                   (selected && (tile[1] > execution.tile_raster.pair_capacity || tile[2])) ||
                   (capabilities_.validation_enabled && (tile[3] != std::min(tile[1], execution.tile_raster.pair_capacity) ||
                                                            tile[4] || tile[5] || tile[6])))) ||
      (capabilities_.validation_enabled &&
          (words[4] != telemetry.gaussian_visible_count || words[5] || words[6]))) {
    ++statistics_.validation_messages;
    throw render::RendererError(render::RendererErrorCode::BackendFailure,
        "resolve Metal Gaussian telemetry", "GPU sort/count or indirect arguments are inconsistent: draw=" + std::to_string(words[1]) + " visible=" + std::to_string(telemetry.gaussian_visible_count) + " tile=" + std::to_string(tile ? tile[10] : 0) + " records=" + std::to_string(tile ? tile[0] : 0) + " requested=" + std::to_string(tile ? tile[1] : 0));
  }
  telemetry.gaussian_sorted_count = telemetry.gaussian_visible_count;
  telemetry.gaussian_gpu_sorted_count = telemetry.gaussian_visible_count;
  telemetry.gaussian_gpu_raster_instance_count = words[1];
  if (tile) {
    telemetry.gaussian_gpu_tile_pair_capacity = execution.tile_raster.pair_capacity;
    telemetry.gaussian_gpu_tile_requested_pair_count = tile[1];
    telemetry.gaussian_gpu_tile_clamped_record_count = tile[2];
    telemetry.gaussian_gpu_tile_raster_frame_count = selected ? 1 : 0;
    telemetry.gaussian_gpu_tile_raster_overflow_fallback_count =
        !selected && (tile[1] > execution.tile_raster.pair_capacity || tile[2]) ? 1 : 0;
  }
}

void Backend::Impl::ResolveGaussianTimestamps(const FrameContext& frame,
    render::RenderResult& result) const {
  if (!frame.gaussian_timestamps)
    return;
  result.timings.gaussian_raster_ns = 0;
  if (!frame.gaussian_execution && frame.gaussian_count == 0)
    return;
  NSData* data = [frame.gaussian_timestamps resolveCounterRange:
          NSMakeRange(0, kGaussianTimestampCount)];
  if (!data || data.length < kGaussianTimestampCount * sizeof(MTLCounterResultTimestamp))
    return;
  const auto* stamps = static_cast<const MTLCounterResultTimestamp*>(data.bytes);
  const auto duration = [&](NSUInteger begin, NSUInteger end) -> std::uint64_t {
    const auto first = stamps[begin].timestamp;
    const auto last = stamps[end].timestamp;
    if (first == MTLCounterErrorValue || last == MTLCounterErrorValue || last < first)
      return 0;
    return last - first;
  };
  if (frame.gaussian_execution) {
    result.timings.gaussian_gpu_preparation_ns =
        duration(kGaussianPrepareBegin, kGaussianPrepareEnd);
    result.timings.gaussian_gpu_sort_ns =
        duration(kGaussianSortBegin, kGaussianSortEnd);
  }
  result.timings.gaussian_raster_ns =
      duration(kGaussianRasterBegin, kGaussianRasterEnd);
  if (frame.gaussian_execution && frame.gaussian_execution->tiled) {
    result.timings.gaussian_gpu_tile_ns = duration(kGaussianTileBegin, kGaussianTileEnd);
    if (result.telemetry.gaussian_gpu_tile_raster_frame_count)
      result.timings.gaussian_raster_ns = duration(kGaussianTileRasterBegin, kGaussianTileRasterEnd);
  }
  if (result.telemetry.gaussian_float_color_frame_count)
    result.timings.gaussian_raster_ns += duration(kGaussianColorConvertBegin, kGaussianColorConvertEnd);
}

void Backend::Impl::PrepareGaussians(const render::RenderRequest& request,
    FrameContext& frame, FrameBuild& build) {
  const auto& snapshot = *request.snapshot;
  const bool cache_hit = gaussian_cache_valid_ &&
                         gaussian_source_.table_identity() == snapshot.gaussians.table_identity() &&
                         gaussian_view_.values == snapshot.view.values &&
                         gaussian_projection_.values == snapshot.projection.values &&
                         gaussian_width_ == request.width && gaussian_height_ == request.height;
  if (cache_hit) {
    ++build.telemetry.gaussian_preparation_cache_hits;
  } else {
    const auto begin = Clock::now();
    const auto prepared = extraction::PrepareGaussianFrame(
        snapshot, {request.width, request.height});
    build.gaussian_preparation_ns = DurationNs(begin, Clock::now());
    const auto count = prepared.gaussians.size();
    // ByteAddressBuffer loads use 32-bit byte offsets in the shader.
    if (count > std::numeric_limits<std::uint32_t>::max() / sizeof(GaussianInstance) ||
        count > device_.maxBufferLength / sizeof(GaussianInstance)) {
      throw render::RendererError(render::RendererErrorCode::Unsupported,
          "prepare Metal Gaussians", "prepared stream exceeds the Metal buffer or shader address limit");
    }
    const auto upload_begin = Clock::now();
    id<MTLBuffer> buffer = nil;
    const auto bytes = count * sizeof(GaussianInstance);
    if (bytes != 0) {
      buffer = [device_ newBufferWithLength:bytes options:MTLResourceStorageModeShared];
      if (buffer == nil) {
        throw render::RendererError(render::RendererErrorCode::BackendFailure,
            "upload Metal Gaussians", "could not allocate the prepared stream");
      }
      buffer.label = @"hdMerlin immutable Gaussian stream";
      auto* target = static_cast<GaussianInstance*>(buffer.contents);
      for (std::size_t i = 0; i < count; ++i) {
        const auto& g = prepared.gaussians[i];
        target[i] = {g.center_pixels, g.inverse_conic, g.radiance,
            g.opacity, g.radius_pixels, g.depth,
            static_cast<std::uint32_t>(g.resource), g.particle};
      }
      ++build.telemetry.allocation_count;
    }
    // Publish the cache only after preparation and upload both succeed.
    gaussian_buffer_ = buffer;
    gaussian_counters_ = prepared.counters;
    gaussian_source_ = snapshot.gaussians;
    gaussian_view_ = snapshot.view;
    gaussian_projection_ = snapshot.projection;
    gaussian_width_ = request.width;
    gaussian_height_ = request.height;
    gaussian_cache_valid_ = true;
    ++build.telemetry.gaussian_preparation_cache_misses;
    build.telemetry.gaussian_upload_bytes += bytes;
    build.telemetry.upload_bytes += bytes;
    build.gaussian_upload_ns = DurationNs(upload_begin, Clock::now());
    build.upload_ns += build.gaussian_upload_ns;
  }
  frame.gaussian_instances = gaussian_buffer_;
  frame.gaussian_count = gaussian_counters_.visible_count;
  build.telemetry.gaussian_candidate_count = gaussian_counters_.candidate_count;
  build.telemetry.gaussian_visible_count = gaussian_counters_.visible_count;
  build.telemetry.gaussian_hidden_count = gaussian_counters_.hidden_count;
  build.telemetry.gaussian_opacity_culled_count = gaussian_counters_.opacity_culled_count;
  build.telemetry.gaussian_frustum_culled_count = gaussian_counters_.frustum_culled_count;
  build.telemetry.gaussian_invalid_culled_count = gaussian_counters_.invalid_culled_count;
  build.telemetry.gaussian_sorted_count = gaussian_counters_.sorted_count;
  build.telemetry.gaussian_sorting_policy_fallback_count = gaussian_counters_.sorting_policy_fallback_count;
}

void Backend::Impl::CreateGaussianExecution() {
  try {
    gaussian_execution_ = std::make_unique<GaussianExecution>(device_, gaussian_library_,
        options_.gaussian_scratch_budget_bytes);
    gaussian_residency_ = std::make_unique<GaussianResidency>(device_,
        options_.gaussian_residency_budget_bytes);
    capabilities_.gpu_driven_gaussian = true;
  } catch (const render::RendererError& error) {
    if (error.code() != render::RendererErrorCode::Unsupported)
      throw;
    // Keep the CPU reference available on devices below the kernel limits.
    gaussian_execution_.reset();
  }
}

} // namespace merlin::metal
