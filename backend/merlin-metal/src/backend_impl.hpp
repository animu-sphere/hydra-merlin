#pragma once

#include "backend_internal.hpp"

namespace merlin::metal {
using namespace detail;

class Backend::Impl {
public:
  // Hgi completion handlers release export leases while the execution thread
  // submits/resolves frames. Serialize their shared frame/pending state.
  mutable std::mutex state_mutex;

  Impl(const render::BackendCreateInfo& info, BackendOptions options);

  ~Impl();

  const render::RendererCapabilities& capabilities() const noexcept;

  render::RendererStatistics statistics() const noexcept;

  MetalStatistics metal_statistics() const noexcept;

  std::optional<render::PresentationTarget>
  default_presentation_target() const noexcept;

  void ResizePresentationTarget(render::PresentationTarget target,
      std::uint32_t width, std::uint32_t height);

  render::CompletionToken Submit(const render::RenderRequest& request);

  render::CompletionToken SubmitFrame(const render::RenderRequest& request);

  bool IsComplete(render::CompletionToken token) const;

  AovImageExport AcquireAovImage(render::CompletionToken token, Aov aov);

  void ReleaseAovImage(AovImageLease&& lease);

  render::RenderResult Resolve(render::CompletionToken token,
      std::chrono::nanoseconds timeout);

private:
  struct Geometry {
    std::uint64_t vertex_revision{};
    std::uint64_t index_revision{};
    id<MTLBuffer> vertices;
    id<MTLBuffer> indices;
    std::uint32_t index_count{};
    std::uint64_t bytes{};
  };

  struct Texture {
    std::uint64_t revision{};
    id<MTLTexture> texture;
    std::uint64_t bytes{};
  };

  struct Sampler {
    std::uint64_t revision{};
    id<MTLSamplerState> sampler;
  };

  struct Retirement {
    std::uint64_t completion_value{};
    std::uint64_t bytes{};
    id<MTLResource> first;
    id<MTLResource> second;
  };

  struct GpuSceneBuffers {
    render::GpuScenePackingCapacities capacities;
    id<MTLBuffer> geometries = nil;
    id<MTLBuffer> instances = nil;
    id<MTLBuffer> materials = nil;
    id<MTLBuffer> draws = nil;
    std::uint64_t source_id{};
    std::uint64_t revision{};
    std::shared_ptr<const std::vector<std::uint32_t>> draw_slot_indices;
    bool has_resident_update{};

    [[nodiscard]] bool enabled() const noexcept {
      return geometries != nil && instances != nil && materials != nil &&
             draws != nil;
    }
  };

  struct GpuSceneCopy {
    id<MTLBuffer> source = nil;
    NSUInteger source_offset{};
    id<MTLBuffer> destination = nil;
    NSUInteger destination_offset{};
    NSUInteger size{};
  };

  struct FrameContext {
    bool busy{};
    std::uint64_t completion_value{};
    std::uint64_t exported_aov_mask{};
    std::uint32_t width{};
    std::uint32_t height{};
    id<MTLTexture> color;
    id<MTLTexture> gaussian_color = nil;
    id<MTLTexture> depth;
    id<MTLTexture> prim_id;
    id<MTLTexture> instance_id;
    id<MTLBuffer> color_readback;
    id<MTLBuffer> depth_readback;
    id<MTLBuffer> prim_readback;
    id<MTLBuffer> instance_readback;
    id<MTLBuffer> argument_buffer;
    id<MTLBuffer> gpu_scene_staging = nil;
    // Retain immutable prepared data until this frame completes. A camera or
    // scene edit allocates a new stream without overwriting in-flight frames.
    id<MTLBuffer> gaussian_instances = nil;
    NSUInteger gaussian_count{};
    std::shared_ptr<const GaussianExecution::Frame> gaussian_execution;
    id<MTLBuffer> gaussian_telemetry = nil;
    id<MTLCounterSampleBuffer> gaussian_timestamps = nil;
    std::vector<std::uint64_t> encoded_textures;
    std::vector<std::uint64_t> encoded_texture_revisions;
    std::vector<std::uint64_t> encoded_samplers;
    std::vector<std::uint64_t> encoded_sampler_revisions;
  };

  struct Pending {
    id<MTLCommandBuffer> command;
    id<CAMetalDrawable> drawable;
    dispatch_semaphore_t completion;
    std::size_t context_index{};
    std::shared_ptr<const extraction::FrameSnapshot> snapshot;
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<Aov> rendered_aovs;
    std::vector<Aov> readback_aovs;
    bool has_gpu_scene_copies{};
    bool has_gaussian_uploads{};
    render::RenderResult result;
  };

  struct FrameBuild {
    render::FrameTelemetry telemetry;
    std::uint64_t upload_ns{};
    std::uint64_t presentation_ns{};
    std::uint64_t gaussian_preparation_ns{};
    std::uint64_t gaussian_upload_ns{};
    std::uint64_t gaussian_raster_ns{};
    std::uint64_t gaussian_attribute_upload_ns{};
    std::shared_ptr<GaussianResidency::Update> gaussian_attributes;
    bool has_gpu_scene_update{};
    std::uint64_t gpu_scene_source_id{};
    std::uint64_t gpu_scene_revision{};
    std::shared_ptr<const std::vector<std::uint32_t>>
        gpu_scene_draw_slot_indices;
    std::vector<GpuSceneCopy> gpu_scene_copies;
    std::vector<MaterialDiagnostic> material_diagnostics;
  };

  bool RecordResidentSceneFailureThrough(std::uint64_t completion_value);

  template <typename Record>
  id<MTLBuffer> CreateGpuSceneTable(std::uint32_t capacity,
      std::string_view table_name) {
    if (capacity == 0) {
      throw render::RendererError(
          render::RendererErrorCode::InvalidRequest,
          "create Metal GPU Scene buffers",
          "every GPU Scene table capacity must be non-zero");
    }
    const auto bytes = static_cast<std::uint64_t>(capacity) * sizeof(Record);
    if (bytes > std::numeric_limits<NSUInteger>::max()) {
      throw render::RendererError(
          render::RendererErrorCode::InvalidRequest,
          "create Metal GPU Scene buffers",
          std::string(table_name) + " table capacity is not representable");
    }
    const auto charged = GpuSceneTableReservation<Record>(device_, capacity);
    id<MTLBuffer> buffer =
        [device_ newBufferWithLength:static_cast<NSUInteger>(bytes)
                             options:MTLResourceStorageModePrivate];
    if (buffer == nil) {
      throw render::RendererError(
          render::RendererErrorCode::ResourceExhausted,
          "create Metal GPU Scene buffers",
          "newBuffer returned nil for the " + std::string(table_name) +
              " table");
    }
    buffer.label = [NSString
        stringWithFormat:@"hdMerlin GPU Scene %s", table_name.data()];
    metal_statistics_.heap_resident_bytes += charged;
    metal_statistics_.heap_peak_resident_bytes =
        std::max(metal_statistics_.heap_peak_resident_bytes,
            metal_statistics_.heap_resident_bytes);
    ++metal_statistics_.heap_allocation_count;
    return buffer;
  }

  void CreateGpuSceneBuffers(render::GpuScenePackingCapacities capacities);

  template <typename Record>
  void ValidateGpuSceneTableUpdate(
      const render::GpuScenePackedUpdate<Record>& packed,
      const std::vector<render::GpuSceneDirtyRange>& dirty_ranges,
      std::uint32_t capacity, std::string_view table_name) const {
    if (packed.ranges.size() != dirty_ranges.size()) {
      throw render::RendererError(
          render::RendererErrorCode::InvalidRequest, "upload Metal GPU Scene",
          std::string(table_name) +
              " packed ranges do not match the update plan");
    }
    std::uint64_t record_count{};
    std::uint64_t copy_bytes{};
    std::uint64_t previous_end{};
    for (std::size_t i = 0; i < packed.ranges.size(); ++i) {
      const auto& range = packed.ranges[i];
      const auto& dirty = dirty_ranges[i];
      if (range.records.empty() || range.first_slot != dirty.first_slot ||
          range.records.size() != dirty.slot_count) {
        throw render::RendererError(
            render::RendererErrorCode::InvalidRequest,
            "upload Metal GPU Scene",
            std::string(table_name) +
                " packed range does not match its dirty range");
      }
      const auto end = static_cast<std::uint64_t>(range.first_slot) +
                       range.records.size();
      if (range.first_slot < previous_end || end > capacity) {
        throw render::RendererError(
            render::RendererErrorCode::InvalidRequest,
            "upload Metal GPU Scene",
            std::string(table_name) +
                " packed range is unordered or exceeds capacity");
      }
      previous_end = end;
      record_count += range.records.size();
      copy_bytes += range.records.size() * sizeof(Record);
    }
    if (packed.record_count != record_count || packed.copy_bytes != copy_bytes) {
      throw render::RendererError(
          render::RendererErrorCode::InvalidRequest, "upload Metal GPU Scene",
          std::string(table_name) +
              " packed telemetry does not match its payload");
    }
  }

  void PrepareGpuSceneUpdate(
      const extraction::FrameSnapshot& snapshot,
      const render::GpuScenePackedFrameUpdate* update, FrameContext& frame,
      FrameBuild& build);

  void EncodeGpuSceneUpdate(id<MTLCommandBuffer> command,
      const FrameBuild& build);

  void CommitGpuSceneUpdate(const FrameBuild& build) noexcept;

  void InvalidateGpuSceneUpdate() noexcept;

  void CreateLibraryAndPipeline();
  void CreateGaussianExecution();

  void ConfigurePresentation(const PresentationOptions& options);

  void NotifyOverlay(PresentationOverlayPhase phase,
      id<MTLCommandBuffer> command = nil,
      id<MTLRenderCommandEncoder> encoder = nil,
      MTLRenderPassDescriptor* pass = nil);

  void ValidatePresentation(render::PresentationTarget target,
      const char* operation) const;

  void ValidatePresentationExtent(std::uint32_t width, std::uint32_t height,
      const char* operation) const;

  void UpdatePresentationExtent(std::uint32_t width, std::uint32_t height);

  void CreateDepthState();

  void CreateHeap();

  id<MTLBuffer> HeapBuffer(std::uint64_t length, std::uint64_t* charged,
      FrameBuild& build);

  id<MTLTexture> HeapTexture(MTLTextureDescriptor* descriptor,
      std::uint64_t* charged, FrameBuild& build);

  void ChargeAllocation(std::uint64_t bytes, FrameBuild& build);

  void Retire(id<MTLResource> first, id<MTLResource> second,
      std::uint64_t bytes);

  void CollectRetirements();

  void Reconcile(const extraction::FrameSnapshot& snapshot, FrameBuild& build);

  ResourceSlot AcquireSlot(StableResourceTable& table, std::uint64_t resource,
      const char* operation);

  std::uint64_t CompletedValue() const noexcept;

  void EnsureTargets(FrameContext& frame, std::uint32_t width,
      std::uint32_t height, bool gaussian_color, FrameBuild& build);

  void EncodeArgumentBuffer(FrameContext& frame, FrameBuild& build);

  static MaterialConstants
  MakeMaterial(const extraction::MaterialRecord& material,
      const render::ForwardDirectionalLighting& lighting);

  void PrepareGaussianExecution(const render::RenderRequest& request,
      id<MTLCommandBuffer> __strong& command, FrameContext& frame, FrameBuild& build);

  void ResolveGaussianTelemetry(const FrameContext& frame, render::RenderResult& result);

  void ResolveGaussianTimestamps(const FrameContext& frame,
      render::RenderResult& result) const;

  void PrepareGaussians(const render::RenderRequest& request,
      FrameContext& frame, FrameBuild& build);

  void EncodeRender(id<MTLCommandBuffer> command, FrameContext& frame,
      const render::RenderRequest& request, FrameBuild& build);

  void EncodePresentation(id<MTLCommandBuffer> command, FrameContext& frame,
      id<CAMetalDrawable> drawable, FrameBuild& build);

  void EncodeReadback(id<MTLCommandBuffer> command, FrameContext& frame,
      const std::vector<Aov>& readbacks);

  template <typename T>
  static std::vector<T> TightCopy(id<MTLBuffer> buffer, std::uint32_t width,
      std::uint32_t height) {
    std::vector<T> result(static_cast<std::size_t>(width) * height);
    const auto source_pitch = AlignedRowPitch(width);
    const auto row_bytes = static_cast<std::size_t>(width) * sizeof(T);
    const auto* source = static_cast<const std::byte*>(buffer.contents);
    auto* destination = reinterpret_cast<std::byte*>(result.data());
    for (std::uint32_t row = 0; row < height; ++row) {
      std::memcpy(destination + row * row_bytes,
          source + static_cast<std::size_t>(row) * source_pitch,
          row_bytes);
    }
    return result;
  }

  static std::vector<std::uint8_t> TightColorCopy(id<MTLBuffer> buffer,
      std::uint32_t width,
      std::uint32_t height);

  void CopyReadbacks(FrameContext& frame, Pending& pending);

  Pending& ValidateToken(render::CompletionToken token, const char* operation);

  const Pending& ValidateToken(render::CompletionToken token,
      const char* operation) const;

  std::uint64_t GeometryResidentBytes() const noexcept;

  std::uint64_t RetiringBytes() const noexcept;

  std::uint64_t owner_{};
  BackendOptions options_;
  id<MTLDevice> device_;
  id<MTLCommandQueue> queue_;
  id<MTLSharedEvent> completion_event_;
  id<MTLLibrary> library_;
  id<MTLLibrary> gaussian_library_;
  id<MTLSamplerState> presentation_sampler_;
  id<MTLRenderPipelineState> pipeline_;
  id<MTLRenderPipelineState> gpu_scene_pipeline_;
  id<MTLRenderPipelineState> presentation_pipeline_;
  id<MTLDepthStencilState> depth_state_;
  id<MTLRenderPipelineState> gaussian_pipeline_;
  id<MTLRenderPipelineState> float_pipeline_;
  id<MTLRenderPipelineState> float_gpu_scene_pipeline_;
  id<MTLComputePipelineState> color_convert_pipeline_;
  id<MTLDepthStencilState> gaussian_depth_state_;
  id<MTLBuffer> gaussian_buffer_ = nil;
  std::unique_ptr<GaussianResidency> gaussian_residency_;
  std::unique_ptr<GaussianExecution> gaussian_execution_;
  extraction::PersistentTable<extraction::GaussianRecord> gaussian_source_;
  extraction::GaussianPreparationCounters gaussian_counters_;
  Mat4 gaussian_view_;
  Mat4 gaussian_projection_;
  std::uint32_t gaussian_width_{};
  std::uint32_t gaussian_height_{};
  bool gaussian_cache_valid_{};
  id<MTLArgumentEncoder> argument_encoder_;
  id<MTLHeap> heap_;
  CAMetalLayer* layer_;
  bool bindless_{};
  std::optional<render::PresentationTarget> presentation_;
  PresentationColorSpace presentation_color_space_{
      PresentationColorSpace::Srgb};
  std::uint32_t presentation_width_{};
  std::uint32_t presentation_height_{};

  render::RendererCapabilities capabilities_;
  render::RendererStatistics statistics_;
  MetalStatistics metal_statistics_;
  GpuSceneBuffers gpu_scene_buffers_;
  StableResourceTable texture_slots_;
  StableResourceTable sampler_slots_;
  std::unordered_map<std::uint64_t, Geometry> geometries_;
  std::unordered_map<std::uint64_t, Texture> textures_;
  std::unordered_map<std::uint64_t, Sampler> samplers_;
  std::vector<Retirement> retirements_;
  std::vector<FrameContext> frames_;
  std::unordered_map<std::uint64_t, Pending> pending_;
  std::size_t next_context_{};
  std::uint64_t submitted_value_{};
  std::atomic<std::uint64_t> completed_value_{};
  std::uint64_t gpu_scene_last_failure_value_{};
  std::uint64_t gpu_scene_invalid_through_value_{};
  std::string gpu_scene_failure_detail_;
  std::int32_t gpu_scene_failure_native_code_{};
  std::uint64_t uploaded_bytes_{};
  std::uint64_t readback_bytes_{};
  std::uint64_t presentation_copy_bytes_{};
  std::uint64_t aov_image_export_count_{};
  std::uint64_t active_aov_image_leases_{};
  std::uint64_t geometry_peak_resident_bytes_{};
};

} // namespace merlin::metal
