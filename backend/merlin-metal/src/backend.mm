#include "backend_impl.hpp"

#include <merlin/render/surface_geometry.hpp>

namespace merlin::metal {

Backend::Impl::Impl(const render::BackendCreateInfo& info, BackendOptions options)
    : owner_(++g_owner), options_(options),
      texture_slots_(options.texture_capacity),
      sampler_slots_(options.sampler_capacity) {
  @autoreleasepool {
    if (info.frames_in_flight == 0 || info.frames_in_flight > 8) {
      throw render::RendererError(render::RendererErrorCode::InvalidRequest,
          "create Metal backend",
          "frames_in_flight must be between 1 and 8");
    }
    if (options_.texture_capacity > kShaderTextureCapacity ||
        options_.sampler_capacity > kShaderSamplerCapacity) {
      throw render::RendererError(
          render::RendererErrorCode::InvalidRequest, "create Metal backend",
          "resource table capacity exceeds the compiled argument-buffer ABI");
    }
    if (options_.heap_capacity_bytes < 1024U * 1024U) {
      throw render::RendererError(render::RendererErrorCode::InvalidRequest,
          "create Metal backend",
          "heap capacity must be at least 1 MiB");
    }
    if (options_.presentation &&
        (options_.presentation->layer == 0 ||
            options_.presentation->drawable_count < 2 ||
            options_.presentation->drawable_count > 3)) {
      throw render::RendererError(
          render::RendererErrorCode::InvalidRequest,
          "create Metal presentation",
          "presentation requires a CAMetalLayer and 2 or 3 drawables");
    }
    if (options_.presentation &&
        options_.presentation->dynamic_range !=
            PresentationDynamicRange::Standard) {
      throw render::RendererError(
          render::RendererErrorCode::Unsupported,
          "create Metal presentation",
          "extended-range presentation is reserved for the future HDR "
          "contract");
    }

    device_ = MTLCreateSystemDefaultDevice();
    if (device_ == nil) {
      throw render::RendererError(
          render::RendererErrorCode::BackendUnavailable,
          "create Metal device", "no Metal device is available");
    }
    queue_ = [device_ newCommandQueue];
    if (queue_ == nil) {
      throw render::RendererError(render::RendererErrorCode::BackendFailure,
          "create Metal command queue",
          "newCommandQueue returned nil");
    }

    bindless_ = device_.argumentBuffersSupport == MTLArgumentBuffersTier2;
    CreateLibraryAndPipeline();
    CreateDepthState();
    CreateHeap();
    CreateGaussianExecution();
    if (options_.gpu_scene_capacities) {
      CreateGpuSceneBuffers(*options_.gpu_scene_capacities);
    }
    if (options_.presentation) {
      ConfigurePresentation(*options_.presentation);
    }
    completion_event_ = [device_ newSharedEvent];
    if (completion_event_ == nil) {
      throw render::RendererError(
          render::RendererErrorCode::BackendFailure,
          "create Metal completion event", "newSharedEvent returned nil");
    }

    frames_.resize(info.frames_in_flight);
    bool gaussian_stage_timestamps =
        [device_ supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary];
    id<MTLCounterSet> timestamp_set = nil;
    if (gaussian_stage_timestamps) {
      for (id<MTLCounterSet> candidate in device_.counterSets) {
        if ([candidate.name isEqualToString:MTLCommonCounterSetTimestamp]) {
          timestamp_set = candidate;
          break;
        }
      }
      gaussian_stage_timestamps = timestamp_set != nil;
    }
    for (auto& frame : frames_) {
      if (gaussian_stage_timestamps) {
        auto descriptor = [[MTLCounterSampleBufferDescriptor alloc] init];
        descriptor.counterSet = timestamp_set;
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.sampleCount = kGaussianTimestampCount;
        NSError* error = nil;
        frame.gaussian_timestamps =
            [device_ newCounterSampleBufferWithDescriptor:descriptor
                                                    error:&error];
        gaussian_stage_timestamps = frame.gaussian_timestamps != nil;
      }
      frame.encoded_textures.resize(options_.texture_capacity);
      frame.encoded_texture_revisions.resize(options_.texture_capacity);
      frame.encoded_samplers.resize(options_.sampler_capacity);
      frame.encoded_sampler_revisions.resize(options_.sampler_capacity);
      if (bindless_) {
        frame.argument_buffer =
            [device_ newBufferWithLength:argument_encoder_.encodedLength
                                 options:MTLResourceStorageModeShared];
        if (frame.argument_buffer == nil) {
          throw render::RendererError(
              render::RendererErrorCode::ResourceExhausted,
              "allocate Metal argument buffer", "newBuffer returned nil");
        }
      }
    }
    if (!gaussian_stage_timestamps)
      for (auto& frame : frames_)
        frame.gaussian_timestamps = nil;

    capabilities_.backend = render::BackendKind::Metal;
    capabilities_.backend_name = "metal";
    capabilities_.device_name = String(device_.name);
    capabilities_.bindless_textures = bindless_;
    capabilities_.asynchronous_upload = false;
    capabilities_.timestamp_queries = true;
    capabilities_.gaussian_gpu_stage_timestamps = gaussian_stage_timestamps;
    capabilities_.external_presentation = layer_ != nil;
    capabilities_.cpu_readback = true;
    capabilities_.validation_enabled = info.enable_validation;
    capabilities_.generated_materials = false;
    capabilities_.limits.max_image_dimension_2d = 16384;
    capabilities_.limits.max_frames_in_flight = info.frames_in_flight;
    capabilities_.limits.sampled_image_slots = options_.texture_capacity;
    capabilities_.limits.sampler_slots = options_.sampler_capacity;

    metal_statistics_.frame_context_count = info.frames_in_flight;
    metal_statistics_.heap_capacity_bytes = options_.heap_capacity_bytes;
  }
}

Backend::Impl::~Impl() {
  @autoreleasepool {
    for (auto& [value, pending] : pending_) {
      (void)value;
      [pending.command waitUntilCompleted];
    }
    NotifyOverlay(PresentationOverlayPhase::Shutdown);
  }
}

const render::RendererCapabilities& Backend::Impl::capabilities() const noexcept {
  return capabilities_;
}

render::RendererStatistics Backend::Impl::statistics() const noexcept {
  auto result = statistics_;
  const auto metal = metal_statistics();
  result.uploaded_bytes = uploaded_bytes_;
  result.readback_bytes = readback_bytes_;
  result.presentation_copy_bytes = presentation_copy_bytes_;
  result.aov_image_export_count = aov_image_export_count_;
  result.active_aov_image_leases = active_aov_image_leases_;
  result.residency.bindless_tables = bindless_;
  result.residency.vram_heap_capacity_bytes = metal.heap_capacity_bytes;
  result.residency.vram_heap_budget_bytes = metal.heap_capacity_bytes;
  result.residency.vram_heap_usage_bytes = metal.heap_resident_bytes;
  result.residency.vram_heap_available_bytes =
      metal.heap_capacity_bytes > metal.heap_resident_bytes
          ? metal.heap_capacity_bytes - metal.heap_resident_bytes
          : 0;
  result.residency.configured_vram_limit_bytes = options_.heap_capacity_bytes;
  result.residency.effective_vram_limit_bytes = options_.heap_capacity_bytes;
  result.residency.renderer_allocated_bytes = metal.heap_resident_bytes;
  result.residency.renderer_peak_allocated_bytes =
      metal.heap_peak_resident_bytes;
  result.residency.geometry_resident_bytes = GeometryResidentBytes();
  result.residency.geometry_peak_resident_bytes =
      geometry_peak_resident_bytes_;
  result.residency.geometry_retiring_bytes = RetiringBytes();
  result.residency.texture_slots_capacity = metal.texture_slots.capacity;
  result.residency.texture_slots_in_use = metal.texture_slots.in_use;
  result.residency.texture_slots_retiring = metal.texture_slots.retiring;
  result.residency.sampler_slots_capacity = metal.sampler_slots.capacity;
  result.residency.sampler_slots_in_use = metal.sampler_slots.in_use;
  result.residency.sampler_slots_retiring = metal.sampler_slots.retiring;
  result.residency.unique_sampler_count = metal.sampler_slots.in_use;
  return result;
}

MetalStatistics Backend::Impl::metal_statistics() const noexcept {
  auto result = metal_statistics_;
  for (const auto& frame : frames_) {
    if (frame.gpu_scene_staging != nil) {
      result.gpu_scene_staging_capacity_bytes +=
          frame.gpu_scene_staging.length;
    }
  }
  result.texture_slots = texture_slots_.telemetry();
  result.sampler_slots = sampler_slots_.telemetry();
  result.gaussian_resident_live_bytes = gaussian_residency_ ? gaussian_residency_->live_bytes() : 0;
  result.gaussian_scratch_live_bytes = gaussian_execution_ ? gaussian_execution_->live_bytes() : 0;
  return result;
}

std::optional<render::PresentationTarget>
Backend::Impl::default_presentation_target() const noexcept {
  return presentation_;
}

render::CompletionToken Backend::Impl::Submit(const render::RenderRequest& request) {
  // @autoreleasepool does not drain on C++ exception unwinding. Catch inside
  // the pool so abandoned commands release their residency/scratch leases
  // before reporting a failed submission to the caller.
  std::exception_ptr failure;
  render::CompletionToken token;
  @autoreleasepool {
    try {
      token = SubmitFrame(request);
    } catch (...) {
      failure = std::current_exception();
    }
  }
  if (failure)
    std::rethrow_exception(failure);
  return token;
}

render::CompletionToken Backend::Impl::SubmitFrame(const render::RenderRequest& request) {
  const auto submit_begin = Clock::now();
  (void)render::PackForwardOutputTransform(request.output_transform);
  if (!render::IsForwardSurfaceDebugView(request.debug_view)) {
    throw render::RendererError(render::RendererErrorCode::InvalidRequest,
        "submit Metal frame",
        "debug view " + std::string(render::DebugViewName(request.debug_view)) +
            " is not executed by the Forward surface context");
  }
  if (!request.snapshot) {
    throw render::RendererError(render::RendererErrorCode::InvalidRequest,
        "submit Metal frame", "snapshot is null");
  }
  if (request.width == 0 || request.height == 0 ||
      request.width > capabilities_.limits.max_image_dimension_2d ||
      request.height > capabilities_.limits.max_image_dimension_2d) {
    throw render::RendererError(render::RendererErrorCode::InvalidRequest,
        "submit Metal frame",
        "render extent is invalid");
  }
  if (render::GpuDrivenIndexedModeName(
          request.gpu_driven_indexed.mode) == "unknown") {
    throw render::RendererError(
        render::RendererErrorCode::InvalidRequest, "submit Metal frame",
        "GPU-driven indexed submission mode is invalid");
  }
  if (request.gpu_driven_indexed.mode ==
      render::GpuDrivenIndexedMode::Require) {
    throw render::RendererError(
        render::RendererErrorCode::Unsupported, "submit Metal frame",
        "required GPU-driven indexed Forward execution is unavailable");
  }
  if (render::GpuDrivenGaussianModeName(
          request.gpu_driven_gaussian.mode) == "unknown" ||
      render::GaussianRasterPathName(
          request.gpu_driven_gaussian.raster) == "unknown") {
    throw render::RendererError(
        render::RendererErrorCode::InvalidRequest, "submit Metal frame",
        "GPU-driven Gaussian execution settings are invalid");
  }
  if (request.gpu_driven_gaussian.mode ==
          render::GpuDrivenGaussianMode::Require &&
      !capabilities_.gpu_driven_gaussian) {
    throw render::RendererError(
        render::RendererErrorCode::Unsupported, "submit Metal frame",
        "required Metal Gaussian path is unavailable");
  }
  if (request.presentation) {
    ValidatePresentation(request.presentation, "submit Metal frame");
    UpdatePresentationExtent(request.width, request.height);
  }

  std::vector<Aov> readbacks;
  auto rendered = ValidateProducts(request.products, &readbacks);
  CollectRetirements();
  if (RecordResidentSceneFailureThrough(submitted_value_)) {
    ++statistics_.validation_messages;
    throw render::RendererError(
        render::RendererErrorCode::BackendFailure,
        "submit Metal frame", gpu_scene_failure_detail_,
        gpu_scene_failure_native_code_);
  }

  auto context_index = next_context_;
  std::size_t searched{};
  while (searched < frames_.size() && frames_[context_index].busy) {
    context_index = (context_index + 1U) % frames_.size();
    ++searched;
  }
  if (searched == frames_.size()) {
    throw render::RendererError(render::RendererErrorCode::ResourceBusy,
        "submit Metal frame",
        "all frames-in-flight are unresolved");
  }
  auto& frame = frames_[context_index];
  next_context_ = (context_index + 1U) % frames_.size();

  FrameBuild build;
  Reconcile(*request.snapshot, build);
  EnsureTargets(frame, request.width, request.height,
      !request.snapshot->gaussians.empty() ||
          request.gpu_driven_gaussian.mode != render::GpuDrivenGaussianMode::Disabled,
      build);
  if (bindless_) {
    EncodeArgumentBuffer(frame, build);
  }
  frame.gaussian_execution.reset();
  frame.gaussian_instances = nil;
  frame.gaussian_count = 0;
  struct UnsubmittedGaussianFrame {
    FrameContext& frame;
    bool submitted{};
    ~UnsubmittedGaussianFrame() {
      if (!submitted)
        frame.gaussian_execution.reset();
    }
  } gaussian_frame_guard{frame};
  PrepareGpuSceneUpdate(*request.snapshot,
      request.gpu_scene_update.get(), frame, build);

  id<MTLCommandBuffer> command = [queue_ commandBuffer];
  if (command == nil) {
    throw render::RendererError(render::RendererErrorCode::BackendFailure,
        "create Metal command buffer",
        "commandBuffer returned nil");
  }
  command.label = @"hdMerlin offscreen frame";

  const auto record_begin = Clock::now();
  PrepareGaussianExecution(request, command, frame, build);
  EncodeGpuSceneUpdate(command, build);
  EncodeRender(command, frame, request, build);
  id<CAMetalDrawable> drawable;
  if (request.presentation) {
    const auto presentation_begin = Clock::now();
    drawable = [layer_ nextDrawable];
    if (drawable == nil) {
      throw render::RendererError(
          render::RendererErrorCode::ResourceBusy,
          "acquire Metal drawable",
          "CAMetalLayer returned no drawable before its timeout");
    }
    EncodePresentation(command, frame, drawable, build);
    [command presentDrawable:drawable];
    build.presentation_ns =
        DurationNs(presentation_begin, Clock::now());
  }
  EncodeReadback(command, frame, readbacks);
  const auto record_end = Clock::now();

  const auto value = ++submitted_value_;
  dispatch_semaphore_t completion = dispatch_semaphore_create(0);
  auto* completed = &completed_value_;
  [command addCompletedHandler:^(id<MTLCommandBuffer>) {
    completed->store(value, std::memory_order_release);
    dispatch_semaphore_signal(completion);
  }];
  if (completion_event_ != nil) {
    [command encodeSignalEvent:completion_event_ value:value];
  }

  const auto queue_begin = Clock::now();
  [command commit];
  gaussian_frame_guard.submitted = true;
  const auto queue_end = Clock::now();
  CommitGpuSceneUpdate(build);
  if (build.gaussian_attributes) {
    gaussian_residency_->Commit(build.gaussian_attributes);
    metal_statistics_.gaussian_compute_dispatch_count += frame.gaussian_execution->dispatch_count;
    metal_statistics_.gaussian_attribute_device_copy_bytes += build.gaussian_attributes->device_copy_bytes;
  }

  frame.busy = true;
  frame.completion_value = value;

  Pending pending;
  pending.command = command;
  pending.completion = completion;
  pending.context_index = context_index;
  pending.snapshot = request.snapshot;
  pending.width = request.width;
  pending.height = request.height;
  pending.rendered_aovs = std::move(rendered);
  pending.readback_aovs = std::move(readbacks);
  pending.drawable = drawable;
  pending.has_gpu_scene_copies = !build.gpu_scene_copies.empty();
  pending.has_gaussian_uploads = build.gaussian_attributes &&
                                 build.gaussian_attributes->upload_bytes != 0;
  pending.result.scene_revision = request.snapshot->revision;
  pending.result.completion_value = value;
  pending.result.telemetry = build.telemetry;
  if (request.gpu_driven_indexed.mode ==
      render::GpuDrivenIndexedMode::Prefer) {
    ++pending.result.telemetry.gpu_driven_fallback_count;
  }
  pending.result.timings.upload_ns = build.upload_ns;
  pending.result.timings.gaussian_preparation_ns = build.gaussian_preparation_ns;
  pending.result.timings.gaussian_prepared_upload_ns = build.gaussian_upload_ns;
  pending.result.timings.gaussian_attribute_upload_ns = build.gaussian_attribute_upload_ns;
  pending.result.timings.gaussian_raster_ns = build.gaussian_raster_ns;
  pending.result.timings.command_recording_ns =
      DurationNs(record_begin, record_end) - build.gaussian_preparation_ns -
      build.gaussian_upload_ns - build.gaussian_attribute_upload_ns;
  pending.result.timings.presentation_ns = build.presentation_ns;
  pending.result.timings.queue_submission_ns =
      DurationNs(queue_begin, queue_end);
  pending.result.timings.backend_total_ns =
      DurationNs(submit_begin, queue_end);
  pending.result.material_diagnostics =
      std::move(build.material_diagnostics);
  pending_.emplace(value, std::move(pending));

  ++statistics_.frames_submitted;
  uploaded_bytes_ += build.telemetry.upload_bytes;
  return render::CompletionToken(owner_, value);
}

bool Backend::Impl::IsComplete(render::CompletionToken token) const {
  const auto& pending = ValidateToken(token, "query Metal completion");
  return pending.command.status == MTLCommandBufferStatusCompleted ||
         pending.command.status == MTLCommandBufferStatusError;
}

AovImageExport Backend::Impl::AcquireAovImage(render::CompletionToken token, Aov aov) {
  @autoreleasepool {
    auto& pending = ValidateToken(token, "acquire Metal AOV image");
    const auto mask = std::uint64_t{1} << static_cast<std::uint32_t>(aov);
    auto& frame = frames_[pending.context_index];
    if (frame.exported_aov_mask & mask) {
      throw render::RendererError(
          render::RendererErrorCode::ResourceBusy,
          "acquire Metal AOV image", "AOV image already has an active export lease");
    }
    if (!HasAov(pending.rendered_aovs, aov)) {
      throw render::RendererError(
          render::RendererErrorCode::InvalidRequest,
          "acquire Metal AOV image", "AOV was not selected for this submission");
    }
    id<MTLTexture> texture = nil;
    MTLPixelFormat format = MTLPixelFormatInvalid;
    MTLTextureUsage usage = MTLTextureUsageUnknown;
    switch (aov) {
    case Aov::Color:
      texture = frame.color;
      format = MTLPixelFormatRGBA8Unorm;
      usage = MTLTextureUsageRenderTarget;
      break;
    case Aov::Depth:
      texture = frame.depth;
      format = MTLPixelFormatDepth32Float;
      usage = MTLTextureUsageRenderTarget;
      break;
    case Aov::PrimId:
      texture = frame.prim_id;
      format = MTLPixelFormatR32Uint;
      usage = MTLTextureUsageRenderTarget;
      break;
    case Aov::InstanceId:
      texture = frame.instance_id;
      format = MTLPixelFormatR32Uint;
      usage = MTLTextureUsageRenderTarget;
      break;
    default:
      throw render::RendererError(
          render::RendererErrorCode::Unsupported,
          "acquire Metal AOV image", "AOV has no Metal export image");
    }
    if (texture == nil) {
      throw render::RendererError(
          render::RendererErrorCode::BackendFailure,
          "acquire Metal AOV image", "selected AOV image is unavailable");
    }
    usage = texture.usage;
    frame.exported_aov_mask |= mask;
    ++aov_image_export_count_;
    ++active_aov_image_leases_;
    ++pending.result.telemetry.aov_image_export_count;
    return AovImageExport{
        AovImageLease(owner_, pending.result.completion_value, aov),
        MakeRenderProduct(pending.width, pending.height, aov),
        reinterpret_cast<std::uintptr_t>((__bridge void*)device_),
        reinterpret_cast<std::uintptr_t>((__bridge void*)queue_),
        reinterpret_cast<std::uintptr_t>((__bridge void*)texture),
        reinterpret_cast<std::uintptr_t>((__bridge void*)completion_event_),
        static_cast<std::uint32_t>(format),
        static_cast<std::uint32_t>(usage),
        static_cast<std::uint32_t>(texture.storageMode),
        pending.result.completion_value};
  }
}

void Backend::Impl::ReleaseAovImage(AovImageLease&& lease) {
  @autoreleasepool {
    if (!lease) {
      return;
    }
    for (auto& frame : frames_) {
      if (frame.completion_value != lease.completion_value()) {
        continue;
      }
      const auto mask = std::uint64_t{1} << static_cast<std::uint32_t>(lease.aov());
      if ((frame.exported_aov_mask & mask) == 0) {
        return;
      }
      frame.exported_aov_mask &= ~mask;
      if (active_aov_image_leases_ != 0) {
        --active_aov_image_leases_;
      }
      if (frame.exported_aov_mask == 0 &&
          pending_.find(frame.completion_value) == pending_.end()) {
        frame.busy = false;
        frame.completion_value = 0;
      }
      lease.Reset();
      return;
    }
  }
}

render::RenderResult Backend::Impl::Resolve(render::CompletionToken token,
    std::chrono::nanoseconds timeout) {
  @autoreleasepool {
    auto& pending = ValidateToken(token, "resolve Metal frame");
    const auto wait_begin = Clock::now();
    if (pending.command.status != MTLCommandBufferStatusCompleted &&
        pending.command.status != MTLCommandBufferStatusError) {
      if (timeout == std::chrono::nanoseconds::max()) {
        [pending.command waitUntilCompleted];
      } else {
        const auto count = std::max<std::int64_t>(timeout.count(), 0);
        const auto deadline = dispatch_time(DISPATCH_TIME_NOW, count);
        if (dispatch_semaphore_wait(pending.completion, deadline) != 0) {
          throw render::RendererError(render::RendererErrorCode::Timeout,
              "resolve Metal frame",
              "completion wait timed out");
        }
      }
    }
    pending.result.timings.completion_wait_ns =
        DurationNs(wait_begin, Clock::now());

    RecordResidentSceneFailureThrough(token.value());

    const bool command_failed =
        pending.command.status == MTLCommandBufferStatusError;
    const bool gpu_scene_dependency_failed =
        token.value() <= gpu_scene_invalid_through_value_;
    if (command_failed || gpu_scene_dependency_failed) {
      ++statistics_.validation_messages;
      const auto detail = command_failed
                              ? (pending.command.error == nil
                                        ? std::string(
                                              "Metal command buffer failed")
                                        : String(pending.command.error
                                                  .localizedDescription))
                              : gpu_scene_failure_detail_;
      auto& failed_frame = frames_[pending.context_index];
      failed_frame.gaussian_execution.reset();
      // A bridge blit may still be waiting on this frame's completion event.
      // Keep the frame identified until that lease callback releases it.
      failed_frame.busy = failed_frame.exported_aov_mask != 0;
      if (!failed_frame.busy) {
        failed_frame.completion_value = 0;
      }
      const auto native_code = command_failed
                                   ? static_cast<std::int32_t>(
                                         pending.command.error.code)
                                   : gpu_scene_failure_native_code_;
      pending_.erase(token.value());
      CollectRetirements();
      throw render::RendererError(render::RendererErrorCode::BackendFailure,
          "execute Metal frame", detail, native_code);
    }

    const auto readback_begin = Clock::now();
    auto& frame = frames_[pending.context_index];
    pending.result.rendered_aovs = pending.rendered_aovs;
    pending.result.cpu_readback_aovs = pending.readback_aovs;
    CopyReadbacks(frame, pending);
    try {
      ResolveGaussianTelemetry(frame, pending.result);
      ResolveGaussianTimestamps(frame, pending.result);
    } catch (...) {
      frame.gaussian_execution.reset();
      frame.busy = frame.exported_aov_mask != 0;
      if (!frame.busy)
        frame.completion_value = 0;
      pending_.erase(token.value());
      CollectRetirements();
      throw;
    }
    frame.gaussian_execution.reset();
    pending.result.timings.readback_ns =
        DurationNs(readback_begin, Clock::now());
    if (pending.command.GPUEndTime >= pending.command.GPUStartTime) {
      pending.result.timings.gpu_execution_ns = static_cast<std::uint64_t>(
          (pending.command.GPUEndTime - pending.command.GPUStartTime) *
          1.0e9);
    }
    pending.result.timings.backend_total_ns +=
        pending.result.timings.completion_wait_ns +
        pending.result.timings.readback_ns;

    auto result = std::move(pending.result);
    // An exported texture is retained until the bridge command buffer
    // completes. Resolve may return before that callback, so the frame
    // context remains unavailable for reuse while any lease is active.
    frame.busy = frame.exported_aov_mask != 0;
    if (!frame.busy) {
      frame.completion_value = 0;
    }
    readback_bytes_ += result.telemetry.readback_bytes;
    if (pending.drawable != nil) {
      ++statistics_.frames_presented;
      presentation_copy_bytes_ +=
          result.telemetry.presentation_copy_bytes;
    }
    pending_.erase(token.value());
    CollectRetirements();
    return result;
  }
}

bool Backend::Impl::RecordResidentSceneFailureThrough(std::uint64_t completion_value) {
  // One Metal command queue preserves submission order. A failed table or
  // Gaussian attribute upload invalidates later submissions that may depend
  // on partially resident contents or immutable version copies.
  std::uint64_t failed_value{};
  Pending* failed_pending{};
  for (auto& [value, candidate] : pending_) {
    if (value <= completion_value && (candidate.has_gpu_scene_copies || candidate.has_gaussian_uploads) &&
        candidate.command.status == MTLCommandBufferStatusError &&
        value > failed_value) {
      failed_value = value;
      failed_pending = &candidate;
    }
  }
  if (failed_pending == nullptr ||
      failed_value <= gpu_scene_last_failure_value_) {
    return false;
  }
  gpu_scene_last_failure_value_ = failed_value;
  gpu_scene_invalid_through_value_ =
      std::max(gpu_scene_invalid_through_value_, submitted_value_);
  gpu_scene_failure_detail_ =
      failed_pending->command.error == nil
          ? std::string("Metal resident scene upload failed")
          : String(failed_pending->command.error.localizedDescription);
  gpu_scene_failure_native_code_ =
      static_cast<std::int32_t>(failed_pending->command.error.code);
  InvalidateGpuSceneUpdate();
  if (gaussian_residency_)
    gaussian_residency_->Reset();
  if (gaussian_execution_)
    gaussian_execution_->Reset();
  return true;
}

Backend::Impl::Pending& Backend::Impl::ValidateToken(render::CompletionToken token, const char* operation) {
  if (token.owner() != owner_ || token.value() == 0) {
    throw render::RendererError(render::RendererErrorCode::InvalidToken,
        operation,
        "token belongs to another backend");
  }
  const auto found = pending_.find(token.value());
  if (found == pending_.end()) {
    throw render::RendererError(render::RendererErrorCode::InvalidToken,
        operation,
        "token is unknown or already resolved");
  }
  return found->second;
}

const Backend::Impl::Pending& Backend::Impl::ValidateToken(render::CompletionToken token,
    const char* operation) const {
  if (token.owner() != owner_ || token.value() == 0) {
    throw render::RendererError(render::RendererErrorCode::InvalidToken,
        operation,
        "token belongs to another backend");
  }
  const auto found = pending_.find(token.value());
  if (found == pending_.end()) {
    throw render::RendererError(render::RendererErrorCode::InvalidToken,
        operation,
        "token is unknown or already resolved");
  }
  return found->second;
}

AovImageLease::AovImageLease(AovImageLease&& other) noexcept
    : owner_(other.owner_), completion_(other.completion_), aov_(other.aov_) {
  other.owner_ = 0;
  other.completion_ = 0;
}

Backend::Backend(const render::BackendCreateInfo& info, BackendOptions options)
    : impl_(std::make_unique<Impl>(info, options)) {
}

Backend::~Backend() = default;
Backend::Backend(Backend&&) noexcept = default;
Backend& Backend::operator=(Backend&&) noexcept = default;

const render::RendererCapabilities& Backend::capabilities() const noexcept {
  return impl_->capabilities();
}

render::RendererStatistics Backend::statistics() const noexcept {
  std::scoped_lock lock(impl_->state_mutex);
  return impl_->statistics();
}

MetalStatistics Backend::metal_statistics() const noexcept {
  std::scoped_lock lock(impl_->state_mutex);
  return impl_->metal_statistics();
}

std::optional<render::PresentationTarget>
Backend::default_presentation_target() const noexcept {
  std::scoped_lock lock(impl_->state_mutex);
  return impl_->default_presentation_target();
}

void Backend::ResizePresentationTarget(render::PresentationTarget target,
    std::uint32_t width,
    std::uint32_t height) {
  std::scoped_lock lock(impl_->state_mutex);
  impl_->ResizePresentationTarget(target, width, height);
}

render::CompletionToken Backend::Submit(const render::RenderRequest& request) {
  std::scoped_lock lock(impl_->state_mutex);
  return impl_->Submit(request);
}

bool Backend::IsComplete(render::CompletionToken token) const {
  std::scoped_lock lock(impl_->state_mutex);
  return impl_->IsComplete(token);
}

AovImageExport Backend::AcquireAovImage(render::CompletionToken token,
    Aov aov) {
  std::scoped_lock lock(impl_->state_mutex);
  return impl_->AcquireAovImage(token, aov);
}

void Backend::ReleaseAovImage(AovImageLease&& lease) {
  std::scoped_lock lock(impl_->state_mutex);
  impl_->ReleaseAovImage(std::move(lease));
}

render::RenderResult Backend::Resolve(render::CompletionToken token,
    std::chrono::nanoseconds timeout) {
  std::scoped_lock lock(impl_->state_mutex);
  return impl_->Resolve(token, timeout);
}

BackendFactory::BackendFactory(BackendOptions options)
    : options_(options) {
}

render::BackendKind BackendFactory::kind() const noexcept {
  return render::BackendKind::Metal;
}

render::BackendAvailability BackendFactory::availability() const {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
      return {false, "no Metal device is available"};
    }
    std::uint64_t gpu_scene_reservation{};
    if (options_.gpu_scene_capacities) {
      gpu_scene_reservation =
          GpuSceneReservation(device, *options_.gpu_scene_capacities);
    }
    if (gpu_scene_reservation >= options_.heap_capacity_bytes) {
      return {false,
          "Metal GPU Scene tables require " +
              std::to_string(gpu_scene_reservation) +
              " bytes of the configured " +
              std::to_string(options_.heap_capacity_bytes) +
              "-byte scene residency budget"};
    }
    id<MTLHeap> heap = [device
        newHeapWithDescriptor:SceneHeapDescriptor(
                                  options_.heap_capacity_bytes -
                                  gpu_scene_reservation)];
    if (heap == nil) {
      return {false,
          "Metal resource heaps are unavailable for the configured " +
              std::to_string(options_.heap_capacity_bytes) +
              "-byte scene residency budget"};
    }
    return {true, String(device.name)};
  }
}

std::unique_ptr<render::Backend>
BackendFactory::Create(const render::BackendCreateInfo& info) const {
  if (info.backend == render::BackendRequest::Vulkan) {
    throw render::RendererError(render::RendererErrorCode::InvalidRequest,
        "create Metal backend",
        "the Metal factory received a Vulkan request");
  }
  return std::make_unique<Backend>(info, options_);
}

} // namespace merlin::metal
