#include "renderer_impl.hpp"

namespace merlin::vulkan {

std::string_view RendererErrorCodeName(RendererErrorCode code) noexcept {
  switch (code) {
  case RendererErrorCode::InvalidRequest:
    return "invalid-request";
  case RendererErrorCode::InvalidToken:
    return "invalid-token";
  case RendererErrorCode::ResourceBusy:
    return "resource-busy";
  case RendererErrorCode::Timeout:
    return "timeout";
  case RendererErrorCode::DeviceLost:
    return "device-lost";
  case RendererErrorCode::ResourceExhausted:
    return "resource-exhausted";
  case RendererErrorCode::Unsupported:
    return "unsupported";
  case RendererErrorCode::BackendFailure:
    return "backend-failure";
  }
  return "unknown";
}

RendererError::RendererError(RendererErrorCode code, std::string operation,
    std::string detail, std::int32_t native_code)
    : std::runtime_error(std::string(RendererErrorCodeName(code)) + ": " +
                         operation + ": " + detail),
      code_(code),
      operation_(std::move(operation)),
      detail_(std::move(detail)),
      native_code_(native_code) {
}

Renderer::Impl::Impl(RendererOptions options) {
  diagnostic_sink_ = options.diagnostic_sink;
  if (options.frames_in_flight < 2 || options.frames_in_flight > 8) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        "create renderer",
        "frames_in_flight must be between 2 and 8");
  }
  if (options.descriptor_backend !=
          DescriptorBackendRequest::Conventional &&
      options.bindless_sampler_capacity > (1U << 16U)) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        "create renderer",
        "bindless_sampler_capacity must not exceed 65536");
  }
  if (options.borrowed_context && options.presentation) {
    throw RendererError(
        RendererErrorCode::InvalidRequest, "create renderer",
        "a borrowed Vulkan context cannot own native presentation");
  }
  if (options.borrowed_context &&
      options.descriptor_backend == DescriptorBackendRequest::Bindless) {
    throw RendererError(
        RendererErrorCode::Unsupported, "create renderer",
        "a borrowed Vulkan context initially supports only conventional "
        "descriptors");
  }
  if (options.presentation) {
    if (options.presentation->create_surface == nullptr) {
      throw RendererError(RendererErrorCode::InvalidRequest,
          "create presentation target",
          "presentation surface callback is null");
    }
    presentation_vsync_ = options.presentation->vsync;
    presentation_overlay_user_data_ =
        options.presentation->overlay_user_data;
    presentation_overlay_ = options.presentation->render_overlay;
  }
  for (auto& artifact : options.generated_material_artifacts) {
    if (artifact.module_key.empty() || artifact.fragment.empty() ||
        artifact.fragment_entry_point.empty() ||
        artifact.parameter_buffer_size == 0U ||
        artifact.reflection.target.empty()) {
      throw RendererError(
          RendererErrorCode::InvalidRequest, "register material artifact",
          "generated material artifact metadata is incomplete");
    }
    if (!generated_material_artifacts_
            .emplace(artifact.module_key, std::move(artifact))
            .second) {
      throw RendererError(RendererErrorCode::InvalidRequest,
          "register material artifact",
          "generated material module key is duplicated");
    }
  }
  try {
    CreateDevice(options);
    capabilities_.generated_materials =
        !generated_material_artifacts_.empty() &&
        capabilities_.descriptor_indexing_selection.selected_backend ==
            DescriptorBackend::Conventional;
    memory_budget_.Initialize(physical_device_, device_,
        capabilities_.memory_budget_extension,
        options.vram_limit_bytes);
    const auto& initial_memory = memory_budget_.telemetry();
    capabilities_.device_local_heap_capacity_bytes =
        initial_memory.heap_capacity_bytes;
    capabilities_.device_local_heap_budget_bytes =
        initial_memory.heap_budget_bytes;
    capabilities_.device_local_heap_usage_bytes =
        initial_memory.heap_usage_bytes;
    capabilities_.configured_vram_limit_bytes = options.vram_limit_bytes;
    if (capabilities_.descriptor_indexing_selection.selected_backend ==
        DescriptorBackend::Bindless) {
      const auto& selection = capabilities_.descriptor_indexing_selection;
      bindless_texture_table_ =
          std::make_unique<BindlessTextureTable>(selection.texture_capacity);
      bindless_sampler_table_ =
          std::make_unique<BindlessSamplerTable>(selection.sampler_capacity);
      bindless_texture_views_.resize(selection.texture_capacity);
      bindless_samplers_.resize(selection.sampler_capacity);
      reserved_bindless_textures_.resize(kReservedBindlessTextureSlots);
      statistics_.bindless_resource_tables = true;
    }
    CreateFrameContexts(options.frames_in_flight);
    statistics_.frame_context_count = options.frames_in_flight;
    vertex_arena_.Initialize(
        device_, physical_device_, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        &memory_budget_, queue_family_, transfer_queue_family_);
    index_arena_.Initialize(
        device_, physical_device_, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        &memory_budget_, queue_family_, transfer_queue_family_);
    constexpr auto gaussian_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    gaussian_position_arena_.Initialize(device_, physical_device_,
        gaussian_usage, &memory_budget_, queue_family_, transfer_queue_family_,
        storage_buffer_alignment_);
    gaussian_covariance_arena_.Initialize(device_, physical_device_,
        gaussian_usage, &memory_budget_, queue_family_, transfer_queue_family_,
        storage_buffer_alignment_);
    gaussian_opacity_arena_.Initialize(device_, physical_device_,
        gaussian_usage, &memory_budget_, queue_family_, transfer_queue_family_,
        storage_buffer_alignment_);
    gaussian_radiance_arena_.Initialize(device_, physical_device_,
        gaussian_usage, &memory_budget_, queue_family_, transfer_queue_family_,
        storage_buffer_alignment_);
    staging_.Initialize(device_, physical_device_, &memory_budget_);
    gaussian_staging_.Initialize(device_, physical_device_, &memory_budget_);
    gpu_scene_staging_.Initialize(device_, physical_device_,
        &memory_budget_);
    gpu_driven_staging_.Initialize(device_, physical_device_,
        &memory_budget_);
    if (options.gpu_scene_capacities) {
      CreateGpuSceneBuffers(*options.gpu_scene_capacities);
    }
    CreateGaussianCornerBuffer();
  } catch (...) {
    Destroy(false);
    throw;
  }
}

Renderer::Impl::~Impl() {
  Destroy(true);
}

std::uint64_t Renderer::Impl::Submit(const RenderRequest& request) {
  ValidateRequest(request);
  const auto backend_start = CpuClock::now();
  frame_counters_ = {};
  ResetAbandonedUploads();
  EnsureEnvironmentLighting(request.shaders.environment);

  auto rendered_aovs = RenderedAovs(request);
  auto cpu_readback_aovs = CpuReadbackAovs(request);
  const auto aov_mask = [](const auto& aovs) {
    std::uint64_t mask{};
    for (const auto aov : aovs) {
      mask |= std::uint64_t{1} << static_cast<std::uint32_t>(aov);
    }
    return mask;
  };
  frame_counters_.requested_aov_count = request.products.size();
  for (const auto& product : request.products) {
    frame_counters_.requested_aov_mask |=
        std::uint64_t{1} << static_cast<std::uint32_t>(product.aov);
  }
  frame_counters_.rendered_aov_count = rendered_aovs.size();
  frame_counters_.rendered_aov_mask = aov_mask(rendered_aovs);
  frame_counters_.cpu_readback_aov_count = cpu_readback_aovs.size();
  frame_counters_.cpu_readback_aov_mask = aov_mask(cpu_readback_aovs);
  if (bindless_texture_table_) {
    EnsureBindlessDescriptorSet();
    EnsureBindlessMaterialDescriptorSetLayout();
  } else {
    EnsureDescriptorSetLayout();
    EnsureGeneratedDescriptorSetLayouts();
  }
  // Gaussian over-blending must not quantize the destination after every
  // splat. Keep a floating-point working target and convert once to the
  // public RGBA8 AOV for all execution policies, including the CPU reference.
  const bool gaussian_float_color = !request.snapshot->gaussians.empty() &&
                                    GaussianFloatColorSupported();
  frame_counters_.gaussian_float_color_frame_count =
      gaussian_float_color ? 1U : 0U;
  const bool gaussian_tile_raster_images =
      gaussian_float_color && request.gpu_driven_gaussian_tile_raster != GpuDrivenGaussianTileRasterMode::Disabled &&
      GaussianTileRasterFormatsSupported();
  auto& frame = AcquireFrame(request.width, request.height, request.shaders,
      cpu_readback_aovs, gaussian_tile_raster_images, gaussian_float_color);
  frame.scene_revision = request.snapshot->revision;
  frame.rendered_aovs = std::move(rendered_aovs);
  frame.cpu_readback_aovs = std::move(cpu_readback_aovs);
  active_target_ = &frame.target;
  struct ResetActiveTarget {
    RenderTarget*& target;
    ~ResetActiveTarget() {
      target = nullptr;
    }
  } reset_active_target{active_target_};
  EnsureTarget(frame.target, request.width, request.height, request.shaders,
      frame.cpu_readback_aovs, gaussian_tile_raster_images, gaussian_float_color);
  if (request.present) {
    PreparePresentation(frame, request.width, request.height);
  } else {
    frame.present_pending = false;
  }
  // The acquired image and its signaled acquire semaphore are only consumed
  // by a successful graphics submission; reclaim both on earlier failures.
  struct ReclaimPresentationOnError {
    Impl& impl;
    FrameContext& frame;
    bool armed;
    ~ReclaimPresentationOnError() {
      if (armed) {
        impl.ReclaimAcquiredImage(frame);
      }
    }
  } reclaim_presentation{*this, frame, frame.present_pending};

  const auto upload_start = CpuClock::now();
  std::uint64_t gaussian_preparation_ns{};
  std::uint64_t gaussian_attribute_upload_ns{};
  std::uint64_t gaussian_prepared_upload_ns{};
  geometry_records_.Sync(request.snapshot->geometries);
  texture_records_.Sync(request.snapshot->textures);
  sampler_records_.Sync(request.snapshot->samplers);
  material_records_.Sync(request.snapshot->materials);
  instance_records_.Sync(request.snapshot->instances);
  draw_records_.Sync(request.snapshot->draws);
  PrepareMeshDrawSummary(*request.snapshot);
  SelectGeneratedMaterials();
  PreflightGeneratedMaterialPipelines(*request.snapshot);
  frame_counters_.draw_count = draw_records_.size();
  frame_counters_.visible_primitive_count = draw_records_.size();
  frame_counters_.triangle_count = mesh_triangle_count_;
  const auto resource_sync_mode =
      SelectResourceSyncMode(*request.snapshot);
  // Resource sync mutates residency before queue submission. Leave this set
  // on every exceptional exit so the next valid request replaces the
  // possibly mixed state instead of selecting the unchanged fast path.
  resource_residency_dirty_ = true;
  if (resource_sync_mode != ResourceSyncMode::Unchanged &&
      (resource_sync_mode != ResourceSyncMode::Incremental ||
          !request.snapshot->delta->textures.upserts.empty() ||
          !request.snapshot->delta->textures.removals.empty() ||
          !request.snapshot->delta->samplers.upserts.empty() ||
          !request.snapshot->delta->samplers.removals.empty())) {
    // Native handles may change even when returning to a previously seen
    // immutable table after another scene or a failed resource update.
    ++material_resource_generation_;
  }
  SyncGeometry(*request.snapshot, resource_sync_mode);
  SyncTextures(*request.snapshot, resource_sync_mode);
  SyncSamplers(*request.snapshot, resource_sync_mode);
  PrepareMaterialDescriptors(frame, *request.snapshot,
      render::PackForwardOutputTransform(request.output_transform));
  PrepareBindlessDescriptors();
  const auto gaussian_attribute_upload_start = CpuClock::now();
  SyncGaussianAttributes(*request.snapshot, resource_sync_mode);
  gaussian_attribute_upload_ns =
      ElapsedNanoseconds(gaussian_attribute_upload_start);
  const auto gaussian_gpu_preparation =
      PrepareGaussianGpuPreparation(frame, *request.snapshot, request);
  const auto gaussian_gpu_sort =
      PrepareGaussianGpuSort(frame, request, gaussian_gpu_preparation);
  const auto gaussian_gpu_raster =
      PrepareGaussianGpuRaster(frame, request, gaussian_gpu_sort);
  const auto gaussian_gpu_tiles =
      PrepareGaussianGpuTiles(frame, request, gaussian_gpu_raster);
  PrepareGaussianGpuTileRaster(frame, request, gaussian_gpu_tiles);
  // Only the CPU-sorted draws and requested validation need the CPU
  // reference stream; GPU-sorted frames otherwise skip it entirely.
  frame.gaussian_cpu_reference =
      !frame.gaussian_gpu_raster.selected ||
      request.gaussian_cpu_reference_validation;
  gaussian_preparation_ns =
      PrepareGaussianCpuReference(request, frame.gaussian_cpu_reference);
  if (frame.gaussian_cpu_reference) {
    ComputeGaussianSortReference(frame.gaussian_gpu_sort);
    ComputeGaussianTileReference(frame.gaussian_gpu_tiles);
  }
  const auto gaussian_prepared_upload_start = CpuClock::now();
  // Sorted-stream raster draws the device-written order, so the frame
  // uploads no CPU-prepared stream.
  if (!frame.gaussian_gpu_raster.selected) {
    PrepareGaussianInstances(frame);
  }
  gaussian_prepared_upload_ns =
      ElapsedNanoseconds(gaussian_prepared_upload_start);
  StageGpuSceneUpdate(*request.snapshot, request.gpu_scene_update.get());
  PrepareGpuDrivenIndexed(frame, *request.snapshot, request);
  if (frame_counters_.upload_bytes != 0) {
    ++statistics_.scene_uploads;
  }
  const auto upload_ns = ElapsedNanoseconds(upload_start);

  const auto recording_start = CpuClock::now();
  Check(vkResetCommandPool(device_, frame.command_pool, 0),
      "reset frame command pool");
  const bool asynchronous_upload =
      capabilities_.async_transfer_queue &&
      (!pending_copies_.empty() || !pending_image_copies_.empty());
  if (asynchronous_upload) {
    Check(vkResetCommandPool(device_, frame.transfer_command_pool, 0),
        "reset transfer command pool");
    VkCommandBufferBeginInfo transfer_begin{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    transfer_begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(frame.transfer_command_buffer,
              &transfer_begin),
        "begin transfer command buffer");
    RecordUploads(frame.transfer_command_buffer, true);
    Check(vkEndCommandBuffer(frame.transfer_command_buffer),
        "end transfer command buffer");
  }
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  Check(vkBeginCommandBuffer(frame.command_buffer, &begin),
      "begin frame command buffer");
  if (frame.timestamp_pool != VK_NULL_HANDLE) {
    vkCmdResetQueryPool(frame.command_buffer, frame.timestamp_pool, 0,
        kFrameTimestampQueryCount);
    vkCmdWriteTimestamp(frame.command_buffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        frame.timestamp_pool, 0);
  }
  if (asynchronous_upload) {
    RecordUploadAcquires(frame.command_buffer);
  } else {
    RecordUploads(frame.command_buffer, false);
  }
  if (frame.timestamp_pool != VK_NULL_HANDLE &&
      frame.gaussian_gpu_preparation.selected) {
    vkCmdWriteTimestamp(frame.command_buffer,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.timestamp_pool, 8);
  }
  RecordGaussianGpuPreparation(frame.command_buffer, frame);
  if (frame.timestamp_pool != VK_NULL_HANDLE &&
      frame.gaussian_gpu_preparation.selected) {
    vkCmdWriteTimestamp(frame.command_buffer,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.timestamp_pool, 9);
  }
  if (frame.timestamp_pool != VK_NULL_HANDLE &&
      frame.gaussian_gpu_sort.selected) {
    vkCmdWriteTimestamp(frame.command_buffer,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        frame.timestamp_pool, 4);
  }
  RecordGaussianGpuSort(frame.command_buffer, frame);
  if (frame.timestamp_pool != VK_NULL_HANDLE &&
      frame.gaussian_gpu_sort.selected) {
    vkCmdWriteTimestamp(frame.command_buffer,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        frame.timestamp_pool, 5);
  }
  RecordGaussianGpuRasterGather(frame.command_buffer, frame);
  if (frame.timestamp_pool != VK_NULL_HANDLE &&
      frame.gaussian_gpu_tiles.selected) {
    vkCmdWriteTimestamp(frame.command_buffer,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        frame.timestamp_pool, 6);
  }
  RecordGaussianGpuTiles(frame.command_buffer, frame);
  if (frame.timestamp_pool != VK_NULL_HANDLE &&
      frame.gaussian_gpu_tiles.selected) {
    vkCmdWriteTimestamp(frame.command_buffer,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        frame.timestamp_pool, 7);
  }
  RecordGpuDrivenDispatch(frame.command_buffer, frame, *request.snapshot,
      request.gpu_driven_indexed);
  RecordFrame(frame.command_buffer, frame, *request.snapshot,
      request.clear_color,
      frame.cpu_readback_aovs);
  if (frame.timestamp_pool != VK_NULL_HANDLE) {
    vkCmdWriteTimestamp(frame.command_buffer,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        frame.timestamp_pool, 1);
  }
  Check(vkEndCommandBuffer(frame.command_buffer), "end frame command buffer");
  const auto recording_ns = ElapsedNanoseconds(recording_start);

  frame.cpu_timings = {};
  frame.cpu_timings.upload_ns = upload_ns;
  frame.cpu_timings.gaussian_preparation_ns = gaussian_preparation_ns;
  frame.cpu_timings.gaussian_attribute_upload_ns =
      gaussian_attribute_upload_ns;
  frame.cpu_timings.gaussian_prepared_upload_ns =
      gaussian_prepared_upload_ns;
  frame.cpu_timings.command_recording_ns = recording_ns;
  // Keep every operation after successful queue submission non-allocating.
  deferred_.reserve(deferred_.size() + frame_upload_buffers_.size());
  const auto submission_start = CpuClock::now();
  const auto transfer_completion =
      asynchronous_upload ? SubmitTransfers(frame) : 0;
  std::uint64_t completion{};
  try {
    completion = SubmitFrame(frame, transfer_completion);
  } catch (...) {
    if (transfer_completion != 0) {
      VkSemaphoreWaitInfo wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
      wait_info.semaphoreCount = 1;
      wait_info.pSemaphores = &transfer_timeline_semaphore_;
      wait_info.pValues = &transfer_completion;
      (void)vkWaitSemaphores(device_, &wait_info,
          std::numeric_limits<std::uint64_t>::max());
      if (frame_counters_.gpu_scene_copy_range_count != 0) {
        // The completed transfer may have replaced slots that the resident
        // CPU revision still names. Do not reuse that revision after the
        // graphics submission which would have committed it fails.
        InvalidateGpuSceneUpdate();
      }
      if (HasPendingGpuDrivenCandidateUpload(frame)) {
        // The transfer has already replaced the device-local candidate
        // list, but the failed graphics submission did not publish its
        // shadow. Force the next request to upload whichever sequence it
        // selects instead of comparing against stale CPU state.
        InvalidateGpuDrivenCandidates(frame);
      }
    }
    throw;
  }
  reclaim_presentation.armed = false;
  frame.cpu_timings.queue_submission_ns =
      ElapsedNanoseconds(submission_start);
  frame.counters = frame_counters_;
  frame.material_diagnostics = std::move(frame_material_diagnostics_);
  CommitTextureUploads();
  CommitResourceSnapshot(*request.snapshot);
  CommitGpuSceneUpdate();
  CommitGpuDrivenCandidates(frame);
  resource_residency_dirty_ = false;
  for (auto& buffer : frame_upload_buffers_) {
    deferred_.push_back({buffer, completion});
    buffer = {};
  }
  frame_upload_buffers_.clear();
  staging_.FinishFrame(completion);
  gaussian_staging_.FinishFrame(completion);
  gpu_scene_staging_.FinishFrame(completion);
  gpu_driven_staging_.FinishFrame(completion);
  frame.outstanding = true;
  ++statistics_.frames_submitted;
  if (frame.present_pending) {
    const auto presentation_start = CpuClock::now();
    try {
      PresentFrame(frame);
    } catch (...) {
      // Queue submission succeeded, so the staging reservations cannot be
      // abandoned or reused just because presentation failed. No completion
      // token can be returned on this path; wait and retire it here instead.
      WaitForFrame(frame, std::chrono::nanoseconds::max());
      latest_completed_value_ =
          std::max(latest_completed_value_, completion);
      staging_.Collect(completion);
      gaussian_staging_.Collect(completion);
      gpu_scene_staging_.Collect(completion);
      gpu_driven_staging_.Collect(completion);
      CollectDeferred(completion);
      frame.outstanding = false;
      frame.counters = {};
      frame.material_diagnostics.clear();
      throw;
    }
    frame.cpu_timings.presentation_ns =
        ElapsedNanoseconds(presentation_start);
  }
  frame.cpu_timings.backend_total_ns = ElapsedNanoseconds(backend_start);
  return completion;
}

bool Renderer::Impl::IsComplete(std::uint64_t completion) const {
  const auto& frame = FindFrame(completion);
  if (timeline_semaphore_ != VK_NULL_HANDLE) {
    std::uint64_t value{};
    Check(vkGetSemaphoreCounterValue(device_, timeline_semaphore_, &value),
        "query frame completion");
    return value >= completion;
  }
  const auto status = vkGetFenceStatus(device_, frame.fence);
  if (status == VK_NOT_READY) {
    return false;
  }
  Check(status, "query frame fence");
  return true;
}

AovImageExport Renderer::Impl::AcquireAovImage(std::uint64_t completion, Aov aov) {
  auto& frame = FindFrame(completion);
  if (!HasAov(frame.rendered_aovs, aov)) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        "acquire AOV image",
        "AOV was not selected for this submission");
  }
  const auto mask = std::uint64_t{1} << static_cast<std::uint32_t>(aov);
  if ((frame.exported_aov_mask & mask) != 0) {
    throw RendererError(RendererErrorCode::ResourceBusy,
        "acquire AOV image",
        "AOV image already has an active export lease");
  }

  VkImage image{};
  VkFormat format{VK_FORMAT_UNDEFINED};
  VkImageAspectFlags aspect{};
  const auto usage = aov == Aov::Color
                         ? TargetImageUsage(aov, false) |
                               (frame.target.gaussian_float_color ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0U)
                         : TargetImageUsage(aov, frame.target.gaussian_tile_raster_images);
  switch (aov) {
  case Aov::Color:
    image = frame.target.color;
    format = kColorFormat;
    aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    break;
  case Aov::Depth:
    image = frame.target.depth;
    format = kDepthFormat;
    aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    break;
  case Aov::PrimId:
    image = frame.target.prim_id;
    format = kIdFormat;
    aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    break;
  case Aov::InstanceId:
    image = frame.target.instance_id;
    format = kIdFormat;
    aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    break;
  default:
    throw RendererError(RendererErrorCode::Unsupported,
        "acquire AOV image",
        "AOV has no Vulkan export image");
  }
  if (image == VK_NULL_HANDLE || format == VK_FORMAT_UNDEFINED) {
    throw RendererError(RendererErrorCode::BackendFailure,
        "acquire AOV image",
        "selected AOV image is unavailable");
  }

  frame.exported_aov_mask |= mask;
  ++frame.counters.aov_image_export_count;
  ++aov_image_export_count_;
  ++active_aov_image_leases_;

  AovImageExport result;
  result.product =
      MakeRenderProduct(frame.target.width, frame.target.height, aov);
  result.physical_device = EncodeHandle(physical_device_);
  result.device = EncodeHandle(device_);
  result.image = EncodeHandle(image);
  result.native_format = static_cast<std::uint32_t>(format);
  result.native_layout =
      static_cast<std::uint32_t>(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  result.native_stage_mask =
      static_cast<std::uint32_t>(VK_PIPELINE_STAGE_TRANSFER_BIT);
  result.native_access_mask =
      static_cast<std::uint32_t>(VK_ACCESS_TRANSFER_READ_BIT);
  result.native_aspect_mask = static_cast<std::uint32_t>(aspect);
  result.native_usage_mask = static_cast<std::uint32_t>(usage);
  result.native_tiling = static_cast<std::uint32_t>(VK_IMAGE_TILING_OPTIMAL);
  result.native_memory_property_mask =
      static_cast<std::uint32_t>(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  result.native_sharing_mode =
      static_cast<std::uint32_t>(VK_SHARING_MODE_EXCLUSIVE);
  result.queue_family = queue_family_;
  result.renderer_completion = completion;
  return result;
}

void Renderer::Impl::ReleaseAovImage(std::uint64_t completion, Aov aov) {
  const auto found = std::find_if(
      frames_.begin(), frames_.end(), [&](const FrameContext& frame) {
        return frame.completion_value == completion;
      });
  if (found == frames_.end()) {
    throw RendererError(RendererErrorCode::InvalidToken,
        "release AOV image",
        "lease completion is unknown");
  }
  const auto mask =
      std::uint64_t{1} << static_cast<std::uint32_t>(aov);
  if ((found->exported_aov_mask & mask) == 0) {
    throw RendererError(RendererErrorCode::InvalidToken,
        "release AOV image",
        "lease is unknown or already released");
  }
  found->exported_aov_mask &= ~mask;
  if (active_aov_image_leases_ != 0) {
    --active_aov_image_leases_;
  }
}

RenderResult Renderer::Impl::Resolve(std::uint64_t completion,
    std::chrono::nanoseconds timeout) {
  auto& frame = FindFrame(completion);
  const auto resolve_start = CpuClock::now();
  const auto wait_start = CpuClock::now();
  WaitForFrame(frame, timeout);
  const auto wait_ns = ElapsedNanoseconds(wait_start);
  frame_counters_ = frame.counters;
  ResolveGaussianGpuPreparationCounters(frame);
  ResolveGaussianGpuSortVerification(frame);
  ResolveGaussianGpuRasterDraw(frame);
  ResolveGaussianGpuTiles(frame);
  ResolveGpuDrivenCounters(frame);
  latest_completed_value_ = std::max(latest_completed_value_, completion);
  staging_.Collect(completion);
  gaussian_staging_.Collect(completion);
  gpu_scene_staging_.Collect(completion);
  gpu_driven_staging_.Collect(completion);
  CollectDeferred(completion);
  active_target_ = &frame.target;
  struct ResetActiveTarget {
    RenderTarget*& target;
    ~ResetActiveTarget() {
      target = nullptr;
    }
  } reset_active_target{active_target_};
  RenderResult result;
  const auto readback_start = CpuClock::now();
  if (HasAov(frame.cpu_readback_aovs, Aov::Color)) {
    result.color = ReadColor(frame.target.width, frame.target.height);
  }
  if (HasAov(frame.cpu_readback_aovs, Aov::Depth)) {
    result.depth = ReadDepth(frame.target.width, frame.target.height);
  }
  if (HasAov(frame.cpu_readback_aovs, Aov::PrimId)) {
    result.prim_id = ReadId(frame.target.width, frame.target.height,
        Aov::PrimId, frame.target.prim_id_readback);
  }
  if (HasAov(frame.cpu_readback_aovs, Aov::InstanceId)) {
    result.instance_id = ReadId(frame.target.width, frame.target.height,
        Aov::InstanceId,
        frame.target.instance_id_readback);
  }
  const auto readback_ns = ElapsedNanoseconds(readback_start);
  result.rendered_aovs = frame.rendered_aovs;
  result.cpu_readback_aovs = frame.cpu_readback_aovs;
  result.scene_revision = frame.scene_revision;
  result.completion_value = completion;
  result.cpu_timings = frame.cpu_timings;
  result.cpu_timings.completion_wait_ns = wait_ns;
  result.cpu_timings.readback_ns = readback_ns;
  result.cpu_timings.gpu_execution_ns = ReadGpuExecutionNanoseconds(frame);
  result.cpu_timings.gaussian_gpu_preparation_ns =
      frame.gaussian_gpu_preparation.selected
          ? ReadGpuTimestampSpanNanoseconds(frame, 8, 9)
          : 0;
  result.cpu_timings.gaussian_gpu_sort_ns =
      ReadGaussianGpuSortNanoseconds(frame);
  result.cpu_timings.gaussian_gpu_tile_ns =
      ReadGaussianGpuTileNanoseconds(frame);
  result.cpu_timings.gaussian_raster_ns =
      ReadGaussianRasterNanoseconds(frame);
  result.cpu_timings.backend_total_ns += ElapsedNanoseconds(resolve_start);
  ++frame_counters_.wait_count;
  ++frame_counters_.resolve_count;
  result.counters = frame_counters_;
  result.material_diagnostics = frame.material_diagnostics;
  frame.outstanding = false;
  frame.counters = {};
  frame.material_diagnostics.clear();
  return result;
}

void Renderer::Impl::CreateFrameContexts(std::uint32_t count) {
  frames_.resize(count);
  for (auto& frame : frames_) {
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                      VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family_;
    Check(vkCreateCommandPool(device_, &pool_info, nullptr,
              &frame.command_pool),
        "create frame command pool");
    VkCommandBufferAllocateInfo command_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool = frame.command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    Check(vkAllocateCommandBuffers(device_, &command_info,
              &frame.command_buffer),
        "allocate frame command buffer");
    if (surface_ != VK_NULL_HANDLE) {
      VkSemaphoreCreateInfo semaphore_info{
          VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      Check(vkCreateSemaphore(device_, &semaphore_info, nullptr,
                &frame.image_available),
          "create presentation acquire semaphore");
    }
    if (capabilities_.async_transfer_queue) {
      pool_info.queueFamilyIndex = transfer_queue_family_;
      Check(vkCreateCommandPool(device_, &pool_info, nullptr,
                &frame.transfer_command_pool),
          "create transfer command pool");
      command_info.commandPool = frame.transfer_command_pool;
      Check(vkAllocateCommandBuffers(device_, &command_info,
                &frame.transfer_command_buffer),
          "allocate transfer command buffer");
    }
    if (timeline_semaphore_ == VK_NULL_HANDLE) {
      VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      Check(vkCreateFence(device_, &fence_info, nullptr, &frame.fence),
          "create frame fence");
    }
    if (capabilities_.timestamp_queries) {
      VkQueryPoolCreateInfo query_info{
          VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
      query_info.queryCount = kFrameTimestampQueryCount;
      Check(vkCreateQueryPool(device_, &query_info, nullptr,
                &frame.timestamp_pool),
          "create frame timestamp query pool");
    }
  }
}

void Renderer::Impl::PrepareMeshDrawSummary(const extraction::FrameSnapshot& snapshot) {
  if (mesh_summary_draws_.table_identity() == snapshot.draws.table_identity() &&
      mesh_summary_geometries_.table_identity() ==
          snapshot.geometries.table_identity() &&
      mesh_material_is_drawn_.size() == material_records_.size()) {
    return;
  }
  std::vector<bool> material_is_drawn(material_records_.size(), false);
  std::uint64_t triangle_count{};
  for (std::size_t draw_index = 0; draw_index < draw_records_.size();
      ++draw_index) {
    ++frame_counters_.mesh_cpu_draw_visit_count;
    const auto& draw = draw_records_[draw_index];
    triangle_count +=
        geometry_records_[draw.geometry_index].indices->size() / 3U;
    if (draw.material_index < material_is_drawn.size()) {
      material_is_drawn[draw.material_index] = true;
    }
  }
  mesh_summary_draws_ = snapshot.draws;
  mesh_summary_geometries_ = snapshot.geometries;
  mesh_material_is_drawn_ = std::move(material_is_drawn);
  mesh_triangle_count_ = triangle_count;
}

void Renderer::Impl::ValidateExtent(std::uint32_t width, std::uint32_t height) const {
  if (width == 0 || height == 0 ||
      width > capabilities_.max_image_dimension_2d ||
      height > capabilities_.max_image_dimension_2d) {
    throw RendererError(RendererErrorCode::Unsupported,
        "validate render request",
        "offscreen extent is unsupported");
  }
}

bool Renderer::Impl::IsSupportedAov(Aov aov) noexcept {
  return aov == Aov::Color || aov == Aov::Depth ||
         aov == Aov::PrimId || aov == Aov::InstanceId;
}

void Renderer::Impl::ValidateRequest(const RenderRequest& request) const {
  if (!request.snapshot) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        "validate render request", "snapshot is null");
  }
  switch (request.gpu_driven_indexed.mode) {
  case GpuDrivenIndexedMode::Disabled:
  case GpuDrivenIndexedMode::Prefer:
  case GpuDrivenIndexedMode::Require:
    break;
  default:
    throw RendererError(RendererErrorCode::InvalidRequest,
        "validate render request",
        "GPU-driven indexed mode is invalid");
  }
  try {
    (void)render::PackForwardOutputTransform(request.output_transform);
  } catch (const render::RendererError& error) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        "validate render request", error.detail());
  }
  ValidateExtent(request.width, request.height);
  if (!std::isfinite(request.clear_color.x) ||
      !std::isfinite(request.clear_color.y) ||
      !std::isfinite(request.clear_color.z) ||
      !std::isfinite(request.clear_color.w)) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        "validate render request",
        "clear color components must be finite");
  }
  if (request.shaders.vertex.empty() || request.shaders.fragment.empty()) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        "validate render request",
        "vertex and fragment shader paths are required");
  }
  if (request.shaders.environment.empty()) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        "validate render request",
        "environment HDR path is required");
  }
  if (bindless_texture_table_ &&
      (request.shaders.bindless_vertex.empty() ||
          request.shaders.bindless_fragment.empty())) {
    throw RendererError(
        RendererErrorCode::InvalidRequest, "validate render request",
        "bindless vertex and fragment shader paths are required");
  }
  if (request.present && surface_ == VK_NULL_HANDLE) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        "validate render request",
        "presentation was requested without a presentation target");
  }
  if (request.products.empty()) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        "validate render request",
        "at least one render product is required");
  }
  std::vector<Aov> seen;
  for (const auto& product : request.products) {
    if (!IsSupportedAov(product.aov)) {
      throw RendererError(RendererErrorCode::Unsupported,
          "validate render request",
          "AOV " + std::string(AovName(product.aov)) +
              " is unsupported");
    }
    if (HasAov(seen, product.aov)) {
      throw RendererError(RendererErrorCode::InvalidRequest,
          "validate render request",
          "duplicate AOV " +
              std::string(AovName(product.aov)));
    }
    seen.push_back(product.aov);
  }
  if (request.present && !HasAov(seen, Aov::Color)) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        "validate render request",
        "presentation requires the color AOV");
  }
}

std::vector<Aov> Renderer::Impl::RenderedAovs(const RenderRequest& request) {
  std::vector<Aov> result;
  result.reserve(request.products.size());
  for (const auto& product : request.products) {
    result.push_back(product.aov);
  }
  return result;
}

std::vector<Aov> Renderer::Impl::CpuReadbackAovs(const RenderRequest& request) {
  std::vector<Aov> result;
  for (const auto& product : request.products) {
    if (product.cpu_readback) {
      result.push_back(product.aov);
    }
  }
  return result;
}

Renderer::Impl::FrameContext& Renderer::Impl::AcquireFrame(std::uint32_t width, std::uint32_t height,
    const ShaderPaths& shaders,
    const std::vector<Aov>& cpu_readback_aovs,
    bool gaussian_tile_raster_images, bool gaussian_float_color) {
  auto reusable = [&](FrameContext& frame) {
    return !frame.outstanding && frame.exported_aov_mask == 0 &&
           frame.target.width == width &&
           frame.target.height == height &&
           frame.target.shaders == shaders &&
           frame.target.cpu_readback_aovs == cpu_readback_aovs &&
           frame.target.gaussian_tile_raster_images ==
               gaussian_tile_raster_images &&
           frame.target.gaussian_float_color == gaussian_float_color;
  };
  auto found = std::find_if(frames_.begin(), frames_.end(), reusable);
  if (found == frames_.end()) {
    found = std::find_if(frames_.begin(), frames_.end(),
        [](const FrameContext& frame) {
          return !frame.outstanding &&
                 frame.exported_aov_mask == 0;
        });
  }
  if (found == frames_.end()) {
    throw RendererError(
        RendererErrorCode::ResourceBusy, "acquire frame context",
        "all frame contexts have unresolved completion tokens");
  }
  if (timeline_semaphore_ == VK_NULL_HANDLE &&
      found->completion_value != 0) {
    Check(vkResetFences(device_, 1, &found->fence), "reset frame fence");
  }
  return *found;
}

Renderer::Impl::FrameContext& Renderer::Impl::FindFrame(std::uint64_t completion) {
  const auto found = std::find_if(
      frames_.begin(), frames_.end(), [&](const FrameContext& frame) {
        return frame.outstanding && frame.completion_value == completion;
      });
  if (found == frames_.end()) {
    throw RendererError(RendererErrorCode::InvalidToken,
        "resolve completion token",
        "token is unknown or already resolved");
  }
  return *found;
}

const Renderer::Impl::FrameContext& Renderer::Impl::FindFrame(std::uint64_t completion) const {
  const auto found = std::find_if(
      frames_.begin(), frames_.end(), [&](const FrameContext& frame) {
        return frame.outstanding && frame.completion_value == completion;
      });
  if (found == frames_.end()) {
    throw RendererError(RendererErrorCode::InvalidToken,
        "query completion token",
        "token is unknown or already resolved");
  }
  return *found;
}

std::uint64_t Renderer::Impl::SubmitTransfers(FrameContext& frame) {
  const auto completion = ++transfer_timeline_value_;
  VkTimelineSemaphoreSubmitInfo timeline{
      VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
  timeline.signalSemaphoreValueCount = 1;
  timeline.pSignalSemaphoreValues = &completion;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.pNext = &timeline;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &frame.transfer_command_buffer;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &transfer_timeline_semaphore_;
  Check(vkQueueSubmit(transfer_queue_, 1, &submit, VK_NULL_HANDLE),
      "submit asynchronous resource uploads");
  ++frame_counters_.transfer_submission_count;
  ++transfer_submission_count_;
  transfer_uploaded_bytes_ += frame_counters_.upload_bytes;
  transfer_ownership_count_ +=
      frame_counters_.queue_ownership_transfer_count;
  return completion;
}

std::uint64_t Renderer::Impl::SubmitFrame(FrameContext& frame,
    std::uint64_t transfer_completion) {
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &frame.command_buffer;

  std::array<VkSemaphore, 2> wait_semaphores{};
  std::array<VkPipelineStageFlags, 2> wait_stages{};
  std::array<std::uint64_t, 2> wait_values{};
  std::uint32_t wait_count{};
  if (transfer_completion != 0) {
    wait_semaphores[wait_count] = transfer_timeline_semaphore_;
    wait_stages[wait_count] = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                              VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    wait_values[wait_count] = transfer_completion;
    ++wait_count;
  }
  if (frame.present_pending) {
    wait_semaphores[wait_count] = frame.image_available;
    wait_stages[wait_count] = VK_PIPELINE_STAGE_TRANSFER_BIT;
    wait_values[wait_count] = 0;
    ++wait_count;
  }
  submit.waitSemaphoreCount = wait_count;
  submit.pWaitSemaphores = wait_semaphores.data();
  submit.pWaitDstStageMask = wait_stages.data();

  std::array<VkSemaphore, 2> signal_semaphores{};
  std::array<std::uint64_t, 2> signal_values{};
  std::uint32_t signal_count{};
  std::uint64_t completion{};
  VkTimelineSemaphoreSubmitInfo timeline{
      VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
  if (timeline_semaphore_ != VK_NULL_HANDLE) {
    completion = ++timeline_value_;
    timeline.waitSemaphoreValueCount = wait_count;
    timeline.pWaitSemaphoreValues = wait_values.data();
    signal_semaphores[signal_count] = timeline_semaphore_;
    signal_values[signal_count] = completion;
    ++signal_count;
    if (frame.present_pending) {
      signal_semaphores[signal_count] =
          swapchain_.render_finished.at(frame.present_image_index);
      signal_values[signal_count] = 0;
      ++signal_count;
    }
    timeline.signalSemaphoreValueCount = signal_count;
    timeline.pSignalSemaphoreValues = signal_values.data();
    submit.pNext = &timeline;
    submit.signalSemaphoreCount = signal_count;
    submit.pSignalSemaphores = signal_semaphores.data();
    Check(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE),
        "submit extracted scene frame");
  } else {
    completion = ++timeline_value_;
    if (frame.present_pending) {
      signal_semaphores[0] =
          swapchain_.render_finished.at(frame.present_image_index);
      submit.signalSemaphoreCount = 1;
      submit.pSignalSemaphores = signal_semaphores.data();
    }
    Check(vkQueueSubmit(queue_, 1, &submit, frame.fence),
        "submit extracted scene frame");
  }
  frame.completion_value = completion;
  return completion;
}

void Renderer::Impl::WaitForFrame(FrameContext& frame, std::chrono::nanoseconds timeout) {
  const auto timeout_ns = timeout == std::chrono::nanoseconds::max()
                              ? std::numeric_limits<std::uint64_t>::max()
                              : static_cast<std::uint64_t>(timeout.count());
  if (timeline_semaphore_ != VK_NULL_HANDLE) {
    VkSemaphoreWaitInfo wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wait_info.semaphoreCount = 1;
    wait_info.pSemaphores = &timeline_semaphore_;
    wait_info.pValues = &frame.completion_value;
    Check(vkWaitSemaphores(device_, &wait_info, timeout_ns),
        "wait for render completion");
  } else {
    Check(vkWaitForFences(device_, 1, &frame.fence, VK_TRUE, timeout_ns),
        "wait for render completion");
  }
}

Renderer::Renderer(RendererOptions options)
    : impl_(std::make_unique<Impl>(options)) {
}
Renderer::~Renderer() = default;
Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;

const RendererCapabilities& Renderer::capabilities() const noexcept {
  return impl_->capabilities_;
}

RendererStatistics Renderer::statistics() const noexcept {
  impl_->memory_budget_.Refresh();
  auto result = impl_->statistics_;
  result.validation_messages =
      impl_->validation_messages_.load(std::memory_order_relaxed);
  result.aov_image_export_count = impl_->aov_image_export_count_;
  result.active_aov_image_leases = impl_->active_aov_image_leases_;
  result.pending_geometry_retirements = 0;
  result.pending_gaussian_attribute_retirements = 0;
  result.geometry_arena_blocks =
      impl_->vertex_arena_.block_count() + impl_->index_arena_.block_count();
  result.vertex_arena = impl_->vertex_arena_.telemetry();
  result.index_arena = impl_->index_arena_.telemetry();
  const auto add_gaussian_arena = [&](const ArenaTelemetry& source) {
    auto& destination = result.gaussian_attribute_arena;
    destination.capacity_bytes += source.capacity_bytes;
    destination.resident_bytes += source.resident_bytes;
    destination.peak_resident_bytes += source.peak_resident_bytes;
    destination.free_bytes += source.free_bytes;
    destination.largest_free_span_bytes = std::max(
        destination.largest_free_span_bytes, source.largest_free_span_bytes);
    destination.retiring_bytes += source.retiring_bytes;
    destination.allocation_count += source.allocation_count;
    destination.release_count += source.release_count;
    destination.active_ranges += source.active_ranges;
    destination.peak_active_ranges += source.peak_active_ranges;
    destination.retiring_ranges += source.retiring_ranges;
    destination.free_spans += source.free_spans;
    destination.blocks += source.blocks;
    destination.growth_count += source.growth_count;
  };
  add_gaussian_arena(impl_->gaussian_position_arena_.telemetry());
  add_gaussian_arena(impl_->gaussian_covariance_arena_.telemetry());
  add_gaussian_arena(impl_->gaussian_opacity_arena_.telemetry());
  add_gaussian_arena(impl_->gaussian_radiance_arena_.telemetry());
  result.upload_ring = impl_->staging_.telemetry();
  result.gaussian_attribute_upload_ring =
      impl_->gaussian_staging_.telemetry();
  result.gpu_scene_upload_ring = impl_->gpu_scene_staging_.telemetry();
  result.gpu_scene_buffers = impl_->gpu_scene_buffers_.enabled();
  result.gpu_scene_capacity_bytes =
      impl_->gpu_scene_buffers_.geometries.size +
      impl_->gpu_scene_buffers_.instances.size +
      impl_->gpu_scene_buffers_.materials.size +
      impl_->gpu_scene_buffers_.draws.size;
  result.gaussian_attribute_resources =
      static_cast<std::uint32_t>(impl_->gaussian_attribute_slots_.size());
  result.memory_budget = impl_->memory_budget_.telemetry();
  result.transfer_queue = {
      impl_->capabilities_.async_transfer_queue,
      impl_->queue_family_,
      impl_->transfer_queue_family_,
      impl_->transfer_submission_count_,
      impl_->transfer_uploaded_bytes_,
      impl_->transfer_ownership_count_,
      impl_->transfer_timeline_value_,
  };
  for (const auto& retired : impl_->retired_ranges_) {
    auto* arena = &result.gaussian_attribute_arena;
    if (retired.arena == &impl_->vertex_arena_) {
      arena = &result.vertex_arena;
      ++result.pending_geometry_retirements;
    } else if (retired.arena == &impl_->index_arena_) {
      arena = &result.index_arena;
      ++result.pending_geometry_retirements;
    } else {
      ++result.pending_gaussian_attribute_retirements;
    }
    ++arena->retiring_ranges;
    arena->retiring_bytes += retired.range.size;
  }
  if (impl_->bindless_texture_table_) {
    result.bindless_texture_slots =
        impl_->bindless_texture_table_->telemetry();
    result.bindless_samplers = impl_->bindless_sampler_table_->telemetry();
  }
  return result;
}

bool HasAov(const std::vector<Aov>& aovs, Aov aov) noexcept {
  return std::find(aovs.begin(), aovs.end(), aov) != aovs.end();
}

bool HasCpuReadback(const RenderResult& result, Aov aov) noexcept {
  return HasAov(result.cpu_readback_aovs, aov);
}

void ValidateRenderResult(const RenderResult& result) {
  if (result.rendered_aovs.empty()) {
    throw std::invalid_argument("render result has no rendered AOVs");
  }
  for (const auto aov : result.cpu_readback_aovs) {
    if (!HasAov(result.rendered_aovs, aov)) {
      throw std::invalid_argument("CPU readback AOV was not rendered");
    }
  }

  std::uint32_t width{};
  std::uint32_t height{};
  auto validate_metadata = [&](const RenderProduct& product, Aov aov,
                               std::uint32_t row_pitch) {
    if (!IsCanonicalRenderProduct(product) || product.aov != aov) {
      throw std::invalid_argument("invalid " + std::string(AovName(aov)) +
                                  " render product metadata");
    }
    if (row_pitch != TightRowPitchBytes(product)) {
      throw std::invalid_argument("render product row pitch is not tight");
    }
    if (width == 0) {
      width = product.width;
      height = product.height;
    } else if (width != product.width || height != product.height) {
      throw std::invalid_argument("render product extents do not match");
    }
  };
  if (HasCpuReadback(result, Aov::Color)) {
    validate_metadata(result.color.product, Aov::Color,
        result.color.row_pitch_bytes);
    const auto bytes = static_cast<std::uint64_t>(result.color.row_pitch_bytes) *
                       result.color.product.height;
    if (bytes != result.color.pixels.size()) {
      throw std::invalid_argument("color render product payload size is invalid");
    }
  }
  if (HasCpuReadback(result, Aov::Depth)) {
    validate_metadata(result.depth.product, Aov::Depth,
        result.depth.row_pitch_bytes);
    const auto values = static_cast<std::uint64_t>(result.depth.product.width) *
                        result.depth.product.height;
    if (values != result.depth.pixels.size()) {
      throw std::invalid_argument("depth render product payload size is invalid");
    }
    if (std::any_of(result.depth.pixels.begin(), result.depth.pixels.end(),
            [](float value) {
              return !std::isfinite(value) || value < 0.0F ||
                     value > 1.0F;
            })) {
      throw std::invalid_argument(
          "depth render product contains invalid values");
    }
  }
  auto validate_id = [&](const ImageUint32& image, Aov aov) {
    validate_metadata(image.product, aov, image.row_pitch_bytes);
    const auto values = static_cast<std::uint64_t>(image.product.width) *
                        image.product.height;
    if (values != image.pixels.size()) {
      throw std::invalid_argument("ID render product payload size is invalid");
    }
  };
  if (HasCpuReadback(result, Aov::PrimId)) {
    validate_id(result.prim_id, Aov::PrimId);
  }
  if (HasCpuReadback(result, Aov::InstanceId)) {
    validate_id(result.instance_id, Aov::InstanceId);
  }
  if (result.completion_value == 0) {
    throw std::invalid_argument("render result has no completion value");
  }
}

CompletionToken Renderer::Submit(const RenderRequest& request) {
  return CompletionToken(impl_->owner_id_, impl_->Submit(request));
}

bool Renderer::IsComplete(CompletionToken token) const {
  if (!token || token.owner_ != impl_->owner_id_) {
    throw RendererError(RendererErrorCode::InvalidToken,
        "query completion token",
        "token belongs to a different renderer");
  }
  return impl_->IsComplete(token.value_);
}

AovImageExport Renderer::AcquireAovImage(CompletionToken token, Aov aov) {
  if (!token || token.owner_ != impl_->owner_id_) {
    throw RendererError(RendererErrorCode::InvalidToken,
        "acquire AOV image",
        "token belongs to another renderer");
  }
  auto result = impl_->AcquireAovImage(token.value_, aov);
  result.lease.owner_ = impl_->owner_id_;
  result.lease.completion_ = token.value_;
  result.lease.aov_ = aov;
  return result;
}

void Renderer::ReleaseAovImage(AovImageLease&& lease) {
  if (!lease || lease.owner_ != impl_->owner_id_) {
    throw RendererError(RendererErrorCode::InvalidToken,
        "release AOV image",
        "lease belongs to another renderer");
  }
  impl_->ReleaseAovImage(lease.completion_, lease.aov_);
  lease.owner_ = 0;
  lease.completion_ = 0;
}

RenderResult Renderer::Resolve(CompletionToken token,
    std::chrono::nanoseconds timeout) {
  if (!token || token.owner_ != impl_->owner_id_) {
    throw RendererError(RendererErrorCode::InvalidToken,
        "resolve completion token",
        "token belongs to a different renderer");
  }
  if (timeout < std::chrono::nanoseconds::zero()) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        "resolve completion token",
        "timeout must not be negative");
  }
  auto result = impl_->Resolve(token.value_, timeout);
  ValidateRenderResult(result);
  return result;
}

RenderResult Renderer::Render(const extraction::FrameSnapshot& snapshot,
    std::uint32_t width, std::uint32_t height,
    const ShaderPaths& shaders) {
  RenderRequest request;
  // Submit consumes all snapshot CPU storage before returning. An empty-owner
  // alias keeps the synchronous compatibility path allocation-free without
  // weakening the owning boundary required by the asynchronous public API.
  request.snapshot = std::shared_ptr<const extraction::FrameSnapshot>(
      std::shared_ptr<const extraction::FrameSnapshot>{}, &snapshot);
  request.width = width;
  request.height = height;
  request.shaders = shaders;
  request.products = {{Aov::Color, true}, {Aov::Depth, true},
      {Aov::PrimId, true}, {Aov::InstanceId, true}};
  return Resolve(Submit(request));
}

} // namespace merlin::vulkan
