#pragma once

#include "renderer_internal.hpp"

namespace merlin::vulkan {
using namespace detail;

class Renderer::Impl {
public:
  explicit Impl(RendererOptions options);

  ~Impl();

  void Destroy(bool wait_for_submissions) noexcept;

  std::uint64_t Submit(const RenderRequest& request);

  bool IsComplete(std::uint64_t completion) const;

  AovImageExport AcquireAovImage(std::uint64_t completion, Aov aov);

  void ReleaseAovImage(std::uint64_t completion, Aov aov);

  RenderResult Resolve(std::uint64_t completion,
      std::chrono::nanoseconds timeout);

  struct RetiredBuffer {
    Buffer buffer;
    std::uint64_t retire_value{};
  };

  struct TextureSlot {
    std::uint64_t revision{};
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
    bool pending_upload{};
    BindlessSlotHandle bindless_slot;
  };

  struct SamplerSlot {
    std::uint64_t revision{};
    VkSampler sampler{};
    BindlessSlotHandle bindless_slot;
  };

  struct RetiredTexture {
    TextureSlot texture;
    std::uint64_t retire_value{};
  };

  struct RetiredSampler {
    VkSampler sampler{};
    std::uint64_t retire_value{};
    BindlessSlotHandle bindless_slot;
  };

  // GPU residency for one mesh, keyed by the serialized RenderWorld handle
  // (slot index + generation) with per-sub-resource revisions. A revision
  // mismatch re-uploads only the stale sub-resource; a size-preserving edit
  // reuses the existing range in place.
  struct GeometrySlot {
    std::uint64_t vertex_revision{};
    std::uint64_t topology_revision{};
    BufferRange vertices;
    BufferRange indices;
    std::uint32_t index_count{};
  };

  struct GaussianAttributeSlot {
    std::uint64_t record_revision{};
    std::uint64_t positions_revision{};
    std::uint64_t covariance_revision{};
    std::uint64_t opacity_revision{};
    std::uint64_t radiance_revision{};
    std::uint64_t policy_revision{};
    std::uint64_t transform_revision{};
    std::uint64_t visibility_revision{};
    std::uint32_t generation{};
    std::uint32_t particle_count{};
    std::uint32_t coefficients_per_particle{};
    BufferRange positions;
    BufferRange covariances;
    BufferRange opacities;
    BufferRange radiance;
  };

  struct RetiredRange {
    DeviceArena* arena{};
    BufferRange range;
    std::uint64_t retire_value{};
  };

  struct PendingCopy {
    VkBuffer source{};
    VkDeviceSize source_offset{};
    VkBuffer destination{};
    VkDeviceSize destination_offset{};
    VkDeviceSize size{};
  };

  struct PendingImageCopy {
    VkBuffer source{};
    VkImage destination{};
    std::uint32_t width{};
    std::uint32_t height{};
  };

  struct GpuSceneBuffers {
    render::GpuScenePackingCapacities capacities;
    Buffer geometries;
    Buffer instances;
    Buffer materials;
    Buffer draws;
    std::uint64_t source_id{};
    std::uint64_t revision{};
    std::uint64_t pending_source_id{};
    std::uint64_t pending_revision{};
    std::shared_ptr<const std::vector<std::uint32_t>> draw_slot_indices;
    std::shared_ptr<const std::vector<std::uint32_t>>
        pending_draw_slot_indices;
    bool has_resident_update{};
    bool pending_update{};

    [[nodiscard]] bool enabled() const noexcept {
      return geometries.handle != VK_NULL_HANDLE;
    }
  };

  struct RenderTarget {
    std::uint32_t width{};
    std::uint32_t height{};
    VkImage color{};
    VkDeviceMemory color_memory{};
    VkImageView color_view{};
    VkImage gaussian_color{};
    VkDeviceMemory gaussian_color_memory{};
    VkImageView gaussian_color_view{};
    bool gaussian_float_color{};
    VkImage depth{};
    VkDeviceMemory depth_memory{};
    VkImageView depth_view{};
    VkImage prim_id{};
    VkDeviceMemory prim_id_memory{};
    VkImageView prim_id_view{};
    VkImage instance_id{};
    VkDeviceMemory instance_id_memory{};
    VkImageView instance_id_view{};
    VkRenderPass render_pass{};
    VkFramebuffer framebuffer{};
    VkPipelineLayout pipeline_layout{};
    std::map<std::uint32_t, VkPipeline> pipelines;
    std::map<std::uint32_t, VkPipeline> gpu_driven_pipelines;
    std::map<std::string, VkPipelineLayout> generated_pipeline_layouts;
    std::map<std::pair<std::string, std::uint32_t>, VkPipeline>
        generated_pipelines;
    VkPipeline gaussian_pipeline{};
    VkPipeline gaussian_id_pipeline{};
    // Same shaders reading the 64-byte GPU-prepared records in sorted order.
    VkPipeline gaussian_sorted_pipeline{};
    VkPipeline gaussian_sorted_id_pipeline{};
    // Color and IDs are also storage images and depth is also sampled, so
    // compute tile raster can composite into them after the render pass.
    bool gaussian_tile_raster_images{};
    Buffer color_readback;
    Buffer depth_readback;
    Buffer prim_id_readback;
    Buffer instance_id_readback;
    ShaderPaths shaders;
    std::vector<Aov> cpu_readback_aovs;
  };

  struct GpuDrivenBatchResources {
    VkDescriptorSet descriptor_set{};
    VkDeviceSize candidate_offset{};
    VkDeviceSize command_offset{};
    VkDeviceSize counter_offset{};
    std::uint32_t candidate_count{};
    std::uint32_t vertex_block{kInvalidBlock};
    std::uint32_t index_block{kInvalidBlock};
    std::uint32_t pipeline_variant{};
    VkPipeline compute_pipeline{};
    // The device-local candidate list remains valid while this frame context
    // is idle. Reusing the context with an identical physical-slot sequence
    // therefore needs no staging reservation or transfer. Keep a separate
    // pending value so a failed queue submission cannot publish bytes that
    // never reached the device.
    std::vector<std::uint32_t> candidate_draw_slot_shadow;
    std::vector<std::uint32_t> pending_candidate_draw_slot_shadow;
    VkDeviceSize candidate_draw_slot_shadow_offset{};
    VkDeviceSize pending_candidate_draw_slot_shadow_offset{};
    bool candidate_upload_pending{};
  };

  struct GpuDrivenBatchSource {
    std::uint64_t source_id{};
    extraction::PersistentTable<extraction::DrawRecord> draws;
    extraction::PersistentTable<extraction::GeometryRecord> geometries;
    extraction::PersistentTable<extraction::MaterialRecord> materials;
    FrontFaceWinding front_face{};
  };

  struct GpuDrivenFrameResources {
    Buffer candidate_draw_slots;
    Buffer candidate_results;
    Buffer indirect_commands;
    Buffer dispatch_counters;
    Buffer counter_readback;
    VkDescriptorPool descriptor_pool{};
    std::vector<VkDescriptorSet> descriptor_sets;
    std::vector<GpuDrivenBatchResources> batches;
    VkDeviceSize candidate_capacity_bytes{};
    VkDeviceSize command_capacity_bytes{};
    VkDeviceSize counter_capacity_bytes{};
    std::uint32_t batch_capacity{};
    std::uint32_t candidate_count{};
    bool selected{};
    // Retain immutable roots/slots until the next successful submission; a
    // camera-only snapshot can reuse the batch layout without visiting draws.
    std::shared_ptr<const GpuDrivenBatchSource> batch_snapshot;
    std::shared_ptr<const GpuDrivenBatchSource> pending_batch_snapshot;
    std::shared_ptr<const std::vector<std::uint32_t>> batch_draw_slots;
    std::shared_ptr<const std::vector<std::uint32_t>> pending_batch_draw_slots;
    std::uint64_t geometry_generation{};
  };

  struct GaussianGpuPreparationBatch {
    VkDescriptorSet descriptor_set{};
    VkDeviceSize candidate_offset{};
    VkDeviceSize prepared_offset{};
    VkDeviceSize counter_offset{};
    VkDeviceSize constants_offset{};
    std::uint64_t resource{};
    std::uint32_t particle_count{};
  };

  struct GaussianGpuPreparationFrameResources {
    Buffer candidate_results;
    Buffer prepared_records;
    Buffer dispatch_counters;
    Buffer counter_readback;
    Buffer constants;
    VkDescriptorPool descriptor_pool{};
    std::vector<VkDescriptorSet> descriptor_sets;
    std::vector<GaussianGpuPreparationBatch> batches;
    VkDeviceSize candidate_capacity_bytes{};
    VkDeviceSize prepared_capacity_bytes{};
    VkDeviceSize counter_capacity_bytes{};
    VkDeviceSize constants_capacity_bytes{};
    std::uint32_t batch_capacity{};
    VkPipeline compute_pipeline{};
    bool selected{};
  };

  enum class GaussianGpuStageSelection {
    NotRequested,
    NoWork,
    Selected,
    Unavailable,
  };

  enum GaussianSortKernel : std::size_t {
    kGaussianSortKeys,
    kGaussianSortHistogram,
    kGaussianSortScanBlocks,
    kGaussianSortScanAdd,
    kGaussianSortScatter,
    kGaussianSortVerify,
    kGaussianSortKernelCount,
  };

  // One exclusive-scan level inside the scan/control buffer, in uint words.
  // Each level's workgroup totals become the next level's input.
  struct GaussianGpuSortScanLevel {
    std::uint32_t offset{};
    std::uint32_t count{};
    std::uint32_t sums_offset{};
  };

  struct GaussianGpuSortKeyBatch {
    shader_abi::GaussianSortConstants constants;
    std::uint32_t workgroup_count{};
  };

  // Keys ping-pong between the two element buffers. Descriptor set 0 reads
  // keys[0] and writes keys[1]; set 1 is the reverse, so pass p uses set
  // p % 2 and the verified result lives in keys[pass_count % 2].
  struct GaussianGpuSortFrameResources {
    std::array<Buffer, 2> keys;
    Buffer scan;
    Buffer verification_readback;
    VkDescriptorPool descriptor_pool{};
    std::array<VkDescriptorSet, 2> descriptor_sets{};
    VkDeviceSize key_capacity_bytes{};
    VkDeviceSize scan_capacity_bytes{};
    std::array<VkPipeline, kGaussianSortKernelCount> pipelines{};
    std::vector<GaussianGpuSortKeyBatch> key_batches;
    std::vector<VkBufferCopy> visible_count_copies;
    std::vector<GaussianGpuSortScanLevel> scan_levels;
    std::uint32_t key_count{};
    std::uint32_t block_count{};
    std::uint32_t low_word_pass_count{};
    std::uint32_t pass_count{};
    // Order-sensitive identity checksum of this frame's CPU reference stream,
    // valid only when the frame prepared that stream.
    std::uint32_t reference_checksum{};
    bool reference_valid{};
    bool selected{};
  };

  // Sorted-stream raster input. The gather copies prepared records into the
  // verified sort order and writes the instance count of one indirect draw,
  // which the Gaussian color and ID subpasses both consume. The descriptor
  // set uses the sort's layout.
  struct GaussianGpuRasterFrameResources {
    Buffer records;
    Buffer draw_arguments;
    Buffer draw_readback;
    VkDescriptorPool descriptor_pool{};
    VkDescriptorSet descriptor_set{};
    VkDeviceSize record_capacity_bytes{};
    VkPipeline gather_pipeline{};
    std::uint32_t element_count{};
    std::uint32_t workgroup_count{};
    bool selected{};
  };

  enum GaussianTileKernel : std::size_t {
    kGaussianTileCount,
    kGaussianTileEmit,
    kGaussianTileRanges,
    kGaussianTileVerify,
    kGaussianTileKernelCount,
  };

  // Tile binning over the gathered raster-order records. Pairs ping-pong like
  // the sort's keys: set 0 reads pairs[0] and writes pairs[1], set 1 the
  // reverse. Emit writes through set 1 into pairs[0], so pair-sort pass p
  // uses set p % 2 and the grouped pairs end in pairs[sort_pass_count % 2].
  // The control buffer holds verification words, per-record pair offsets,
  // the pair histogram, their scan levels, and two range words per tile.
  struct GaussianGpuTileFrameResources {
    shader_abi::GaussianTileConstants constants;
    std::array<Buffer, 2> pairs;
    Buffer control;
    Buffer verification_readback;
    VkDescriptorPool descriptor_pool{};
    std::array<VkDescriptorSet, 2> descriptor_sets{};
    VkDeviceSize pair_capacity_bytes{};
    VkDeviceSize control_capacity_bytes{};
    std::array<VkPipeline, kGaussianTileKernelCount> pipelines{};
    std::array<VkPipeline, kGaussianSortKernelCount> sort_pipelines{};
    std::vector<GaussianGpuSortScanLevel> offset_levels;
    std::vector<GaussianGpuSortScanLevel> histogram_levels;
    std::uint32_t record_blocks{};
    std::uint32_t pair_blocks{};
    std::uint32_t sort_pass_count{};
    // CPU reference over the CPU-sorted stream, with the same bounds,
    // per-record limit, and capacity truncation.
    std::uint32_t reference_requested_pair_count{};
    std::uint32_t reference_checksum{};
    bool reference_valid{};
    bool selected{};
  };

  // Compute tile raster over the verified tile ranges. Its descriptor set
  // binds the grouped pairs, the tile control buffer, the gathered records,
  // the sorted-stream draw arguments, and this frame's color, ID, and depth
  // targets, so it is rewritten every selected frame.
  struct GaussianGpuTileRasterFrameResources {
    shader_abi::GaussianTileRasterConstants constants;
    VkDescriptorPool descriptor_pool{};
    VkDescriptorSet descriptor_set{};
    VkPipeline select_pipeline{};
    VkPipeline raster_pipeline{};
    bool selected{};
  };

  struct MaterialDescriptorSource {
    std::uint64_t source_id{};
    std::uint64_t resource_generation{};
    extraction::PersistentTable<extraction::MaterialRecord> materials;
    extraction::PersistentTable<extraction::TextureRecord> textures;
    extraction::PersistentTable<extraction::SamplerRecord> samplers;
    std::vector<const GeneratedMaterialArtifact*> selected_artifacts;
  };

  struct FrameContext {
    // True when this frame prepared the CPU reference stream, to draw it or
    // to validate the GPU-sorted stream against it.
    bool gaussian_cpu_reference{};
    VkCommandPool command_pool{};
    VkCommandBuffer command_buffer{};
    VkCommandPool transfer_command_pool{};
    VkCommandBuffer transfer_command_buffer{};
    VkFence fence{};
    VkQueryPool timestamp_pool{};
    VkSemaphore image_available{};
    std::uint32_t present_image_index{};
    bool present_pending{};
    std::uint64_t completion_value{};
    bool outstanding{};
    std::uint64_t exported_aov_mask{};
    RenderTarget target;
    std::uint64_t scene_revision{};
    std::vector<Aov> rendered_aovs;
    std::vector<Aov> cpu_readback_aovs;
    FrameCpuTimings cpu_timings;
    FrameCounters counters;
    VkDescriptorPool descriptor_pool{};
    std::uint32_t descriptor_capacity{};
    std::vector<VkDescriptorSet> material_descriptor_sets;
    VkDescriptorSet bindless_material_descriptor_set{};
    VkDeviceSize material_uniform_stride{};
    Buffer material_uniforms;
    std::vector<VkDeviceSize> generated_parameter_offsets;
    std::shared_ptr<const MaterialDescriptorSource> material_descriptor_source;
    Buffer generated_parameter_uniforms;
    Buffer gaussian_instances;
    // Host-visible Gaussian buffers are frame-owned so they remain immutable
    // while a submission is in flight. Retain the packed contents for this
    // particular buffer and write only instance ranges whose derived values
    // changed when the frame context is reused.
    std::vector<GaussianGpuInstance> gaussian_instance_shadow;
    std::uint64_t gaussian_preparation_generation{};
    std::uint32_t gaussian_instance_count{};
    std::vector<MaterialDiagnostic> material_diagnostics;
    GpuDrivenFrameResources gpu_driven;
    GaussianGpuPreparationFrameResources gaussian_gpu_preparation;
    GaussianGpuSortFrameResources gaussian_gpu_sort;
    GaussianGpuRasterFrameResources gaussian_gpu_raster;
    GaussianGpuTileFrameResources gaussian_gpu_tiles;
    GaussianGpuTileRasterFrameResources gaussian_gpu_tile_raster;
  };

  struct SwapchainState {
    VkSwapchainKHR handle{};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkColorSpaceKHR color_space{VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    VkPresentModeKHR present_mode{VK_PRESENT_MODE_FIFO_KHR};
    VkExtent2D extent{};
    std::uint32_t requested_width{};
    std::uint32_t requested_height{};
    std::vector<VkImage> images;
    std::vector<VkImageView> image_views;
    std::vector<VkFramebuffer> overlay_framebuffers;
    std::vector<VkSemaphore> render_finished;
    std::vector<bool> initialized;
    VkRenderPass overlay_render_pass{};
    bool dirty{};
  };

  enum class ResourceSyncMode {
    Full,
    ReplaceAll,
    Incremental,
    Unchanged,
  };

  [[nodiscard]] ResourceSyncMode SelectResourceSyncMode(
      const extraction::FrameSnapshot& snapshot) const noexcept;

  void CommitResourceSnapshot(
      const extraction::FrameSnapshot& snapshot) noexcept;

  void AdoptBorrowedContext(const RendererOptions& options);

  void CreateDevice(const RendererOptions& options);

  void CreateFrameContexts(std::uint32_t count);

  Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
      VkMemoryPropertyFlags properties,
      VkMemoryPropertyFlags preferred = 0,
      std::uint32_t first_queue_family =
          VK_QUEUE_FAMILY_IGNORED,
      std::uint32_t second_queue_family =
          VK_QUEUE_FAMILY_IGNORED);

  void DestroyBuffer(Buffer& buffer) noexcept;

  template <typename Record>
  Buffer CreateGpuSceneTable(std::uint32_t capacity) {
    if (capacity == 0) {
      throw RendererError(RendererErrorCode::InvalidRequest,
          "create GPU Scene buffers",
          "every GPU Scene table capacity must be non-zero");
    }
    const auto bytes = static_cast<VkDeviceSize>(capacity) * sizeof(Record);
    if (bytes > max_storage_buffer_range_) {
      throw RendererError(
          RendererErrorCode::Unsupported, "create GPU Scene buffers",
          "GPU Scene table exceeds the device maxStorageBufferRange limit");
    }
    ++frame_counters_.allocation_count;
    ++frame_counters_.buffer_allocation_count;
    frame_counters_.buffer_allocation_bytes += bytes;
    return CreateBufferRaw(
        device_, physical_device_, bytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory_budget_, queue_family_,
        transfer_queue_family_);
  }

  void CreateGpuSceneBuffers(
      render::GpuScenePackingCapacities capacities);

  void DestroyGpuSceneBuffers() noexcept;

  void DestroyGpuDrivenFrameResources(
      GpuDrivenFrameResources& resources) noexcept;

  void DestroyGaussianGpuPreparationFrameResources(
      GaussianGpuPreparationFrameResources& resources) noexcept;

  void DestroyGaussianGpuSortFrameResources(
      GaussianGpuSortFrameResources& resources) noexcept;

  void DestroyGaussianGpuRasterFrameResources(
      GaussianGpuRasterFrameResources& resources) noexcept;

  void DestroyGaussianGpuTileFrameResources(
      GaussianGpuTileFrameResources& resources) noexcept;

  void DestroyGaussianGpuTileRasterFrameResources(
      GaussianGpuTileRasterFrameResources& resources) noexcept;

  template <typename Record>
  void ValidateGpuSceneTableUpdate(
      const render::GpuScenePackedUpdate<Record>& packed,
      const std::vector<render::GpuSceneDirtyRange>& dirty_ranges,
      std::uint32_t capacity, std::string_view table_name) const {
    if (packed.ranges.size() != dirty_ranges.size()) {
      throw RendererError(RendererErrorCode::InvalidRequest,
          "upload GPU Scene",
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
        throw RendererError(RendererErrorCode::InvalidRequest,
            "upload GPU Scene",
            std::string(table_name) +
                " packed range does not match its dirty range");
      }
      const auto end = static_cast<std::uint64_t>(range.first_slot) +
                       range.records.size();
      if (range.first_slot < previous_end || end > capacity) {
        throw RendererError(RendererErrorCode::InvalidRequest,
            "upload GPU Scene",
            std::string(table_name) +
                " packed range is unordered or exceeds capacity");
      }
      previous_end = end;
      record_count += range.records.size();
      copy_bytes += range.records.size() * sizeof(Record);
    }
    if (packed.record_count != record_count ||
        packed.copy_bytes != copy_bytes) {
      throw RendererError(RendererErrorCode::InvalidRequest,
          "upload GPU Scene",
          std::string(table_name) +
              " packed telemetry does not match its payload");
    }
  }

  void StageGpuSceneUpdate(
      const extraction::FrameSnapshot& snapshot,
      const render::GpuScenePackedFrameUpdate* update);

  void CommitGpuSceneUpdate() noexcept;

  void PrepareGpuDrivenIndexed(FrameContext& frame,
      const extraction::FrameSnapshot& snapshot,
      const RenderRequest& request);

  GaussianGpuStageSelection PrepareGaussianGpuPreparation(
      FrameContext& frame, const extraction::FrameSnapshot& snapshot,
      const RenderRequest& request);

  static void CommitGpuDrivenCandidates(FrameContext& frame) noexcept;

  static void InvalidateGpuDrivenCandidates(FrameContext& frame) noexcept;

  static bool HasPendingGpuDrivenCandidateUpload(
      const FrameContext& frame) noexcept;

  void InvalidateGpuSceneUpdate() noexcept;

  void CreateGaussianCornerBuffer();

  void DestroyTexture(TextureSlot& texture) noexcept;

  void ResetAbandonedUploads();

  TextureSlot CreateTextureResource(
      std::uint32_t width, std::uint32_t height, std::uint64_t revision,
      const std::vector<std::uint8_t>& pixels, bool count_upload = true);

  void CommitTextureUploads() noexcept;

  VkSampler CreateSamplerResource(FilterMode min_filter, FilterMode mag_filter,
      AddressMode address_u,
      AddressMode address_v);

  void CollectCompletedBindlessSlots(std::uint64_t completed);

  void RetireTexture(TextureSlot& texture);

  void RetireSampler(SamplerSlot& sampler);

  void SyncTextures(const extraction::FrameSnapshot& snapshot,
      ResourceSyncMode mode);

  void SyncSamplers(const extraction::FrameSnapshot& snapshot,
      ResourceSyncMode mode);

  void EnsureFallbackTextureAndSampler();

  void EnsureReservedBindlessTextures();

  [[nodiscard]] VkImageView FallbackTextureView() const noexcept;

  void EnsureBindlessDescriptorSet();

  void EnsureBindlessMaterialDescriptorSetLayout();

  void EnsureGpuDrivenDescriptorAndPipelineLayouts();

  VkPipeline EnsureGpuDrivenComputePipeline(const ShaderPaths& shaders);

  void EnsureGpuDrivenFrameResources(GpuDrivenFrameResources& resources,
      VkDeviceSize candidate_bytes,
      VkDeviceSize command_bytes,
      VkDeviceSize counter_bytes,
      std::uint32_t batch_count);

  void UpdateGpuDrivenBatchDescriptors(
      const GpuDrivenFrameResources& resources);

  // The compute set below binds every Gaussian attribute range plus the
  // compacted outputs. Vulkan only guarantees four storage buffers per stage
  // and per set, so the counts are named here and preflighted against the
  // device limits before selection.
  static constexpr std::uint32_t kGaussianPrepareStorageBufferCount{7U};
  static constexpr std::uint32_t kGaussianPrepareUniformBufferCount{1U};

  void EnsureGaussianPrepareDescriptorAndPipelineLayouts();

  static std::filesystem::path GaussianPrepareComputePath(
      const ShaderPaths& shaders);

  VkPipeline EnsureGaussianPrepareComputePipeline(const ShaderPaths& shaders);

  // Every sort kernel shares one layout of four storage buffers, the Vulkan
  // guaranteed per-stage minimum, so selection needs no descriptor-limit
  // preflight.
  static constexpr std::uint32_t kGaussianSortStorageBufferCount{4U};

  void EnsureGaussianSortDescriptorAndPipelineLayouts();

  std::array<VkPipeline, kGaussianSortKernelCount>
  EnsureGaussianSortComputePipelines(const ShaderPaths& shaders);

  static std::filesystem::path GaussianSortDirectory(
      const ShaderPaths& shaders);

  // The sort kernels and the raster gather share one set layout and pipeline
  // layout, so their pipelines share one cache keyed by artifact path.
  VkPipeline EnsureGaussianSortLayoutPipeline(
      const std::filesystem::path& path);

  VkPipeline EnsureGaussianRasterGatherPipeline(const ShaderPaths& shaders);

  void EnsureGaussianGpuSortFrameResources(
      GaussianGpuSortFrameResources& resources, VkDeviceSize key_bytes,
      VkDeviceSize scan_bytes);

  // Rewritten every selected frame, like the preparation descriptors: a
  // recreated prepared-record buffer may reuse a destroyed handle's value.
  void UpdateGaussianGpuSortDescriptors(
      const GaussianGpuSortFrameResources& resources,
      VkBuffer prepared_records);

  // Appends the exclusive-scan levels of `count` words starting at word
  // `offset`; each level's workgroup totals become the next level's input.
  // Returns the first word past the top level's total, or nothing when a
  // level would leave uint32 word addressing.
  static std::optional<std::uint64_t> AppendGaussianScanLevels(
      std::uint64_t offset, std::uint64_t count,
      std::vector<GaussianGpuSortScanLevel>& levels);

  GaussianGpuStageSelection PrepareGaussianGpuSort(FrameContext& frame,
      const RenderRequest& request, GaussianGpuStageSelection preparation);

  // The GPU checksum weights each sorted record by its position, so it
  // matches only when both streams hold the same records in the same order.
  void ComputeGaussianSortReference(GaussianGpuSortFrameResources& resources);

  // Prepares and sorts the CPU reference stream, reusing it while the
  // Gaussian table, camera, and viewport are unchanged. A frame drawn from
  // the GPU-sorted stream without validation does no per-Gaussian CPU work:
  // it counts only per-resource totals, and resolve fills the per-particle
  // counters from the GPU preparation. Returns the preparation time.
  std::uint64_t PrepareGaussianCpuReference(const RenderRequest& request,
      bool required);

  void EnsureGaussianGpuRasterFrameResources(
      GaussianGpuRasterFrameResources& resources, VkDeviceSize record_bytes);

  // Rewritten every selected frame: the verified keys alternate between the
  // two sort buffers with the pass count, and a recreated prepared-record
  // buffer may reuse a destroyed handle's value.
  void UpdateGaussianGpuRasterDescriptors(
      const GaussianGpuRasterFrameResources& resources, VkBuffer sorted_keys,
      VkBuffer prepared_records);

  // Selecting sorted-stream raster replaces the CPU-sorted upload for this
  // frame, so the choice is final before any prepared stream is written.
  GaussianGpuStageSelection PrepareGaussianGpuRaster(FrameContext& frame,
      const RenderRequest& request, GaussianGpuStageSelection sort);

  static std::filesystem::path GaussianTileDirectory(
      const ShaderPaths& shaders);

  std::array<VkPipeline, kGaussianTileKernelCount>
  EnsureGaussianTileComputePipelines(const ShaderPaths& shaders);

  void EnsureGaussianGpuTileFrameResources(
      GaussianGpuTileFrameResources& resources, VkDeviceSize pair_bytes,
      VkDeviceSize control_bytes);

  // Rewritten every selected frame: a recreated raster-record buffer may
  // reuse a destroyed handle's value.
  void UpdateGaussianGpuTileDescriptors(
      const GaussianGpuTileFrameResources& resources, VkBuffer records);

  // Replays tile binning over the CPU-sorted reference stream with the same
  // bounds, per-record limit, capacity truncation, and stable tile grouping,
  // so matching streams produce the GPU verification checksum.
  void ComputeGaussianTileReference(GaussianGpuTileFrameResources& resources);

  // Tile binning consumes the gathered raster-order records and the draw
  // arguments' instance count, so it depends on sorted-stream raster for the
  // same frame. Its dispatch plan is fixed by the padded record count, the
  // pair capacity, and the tile grid.
  GaussianGpuStageSelection PrepareGaussianGpuTiles(FrameContext& frame,
      const RenderRequest& request, GaussianGpuStageSelection raster);

  static std::filesystem::path GaussianTileRasterDirectory(
      const ShaderPaths& shaders);

  static constexpr std::uint32_t kGaussianTileRasterStorageBufferCount{4U};
  static constexpr std::uint32_t kGaussianTileRasterStorageImageCount{3U};

  void EnsureGaussianTileRasterDescriptorAndPipelineLayouts();

  VkPipeline EnsureGaussianTileRasterPipeline(
      const std::filesystem::path& path);

  void EnsureGaussianGpuTileRasterFrameResources(
      GaussianGpuTileRasterFrameResources& resources);

  // Rewritten every selected frame: the grouped pair buffer follows the
  // frame's pass count, and recreated buffers or targets may reuse a
  // destroyed handle's value. The images are bound in the layouts the
  // raster dispatch transitions them to.
  void UpdateGaussianGpuTileRasterDescriptors(const FrameContext& frame);

  // Tile raster composites the verified tile ranges, so it depends on tile
  // binning for the same frame and on a target created with storage color
  // and ID images and a sampled depth image. Its dispatch plan is one
  // selection thread and one workgroup per tile.
  void PrepareGaussianGpuTileRaster(FrameContext& frame,
      const RenderRequest& request, GaussianGpuStageSelection tiles);

  void EnsureGaussianGpuPreparationFrameResources(
      GaussianGpuPreparationFrameResources& resources,
      VkDeviceSize candidate_bytes, VkDeviceSize prepared_bytes,
      VkDeviceSize counter_bytes, VkDeviceSize constants_bytes,
      std::uint32_t batch_count);

  void UpdateGaussianGpuPreparationDescriptors(
      const GaussianGpuPreparationFrameResources& resources);

  void WriteBindlessDescriptors(
      const std::vector<std::uint32_t>& texture_indices,
      const std::vector<std::uint32_t>& sampler_indices);

  void PrepareBindlessDescriptors();

  void EnsureDescriptorSetLayout();

  void EnsureGeneratedDescriptorSetLayouts();

  static render::ForwardDirectionalLighting ExtractDirectionalLighting(
      const extraction::FrameSnapshot& snapshot);

  void EnsureEnvironmentLighting(const std::filesystem::path& path);

  MaterialUniforms MakeMaterialUniforms(
      const extraction::MaterialRecord& material,
      const render::ForwardDirectionalLighting& lighting) const;

  void RejectGeneratedMaterial(std::size_t index,
      MaterialDiagnosticCategory category,
      std::string message);

  static std::optional<std::size_t> PackedMaterialValueSize(
      MaterialValueType type) noexcept;

  static bool GeneratedBindingFits(
      const GeneratedMaterialParameterBinding& binding,
      std::uint32_t parameter_buffer_size) noexcept;

  void PrepareMeshDrawSummary(const extraction::FrameSnapshot& snapshot);

  void SelectGeneratedMaterials();

  static std::size_t MaterialValueSize(MaterialValueType type);

  static void CopyMaterialValue(std::byte* destination,
      const MaterialValue& value,
      MaterialValueType type);

  void PackGeneratedMaterialParameters(
      std::byte* destination, const extraction::MaterialRecord& material,
      const GeneratedMaterialArtifact& artifact) const;

  void PrepareBindlessMaterialDescriptors(
      FrameContext& frame, const extraction::FrameSnapshot& snapshot);

  void PrepareGaussianInstances(FrameContext& frame);

  void ValidateMaterialTextureBindings() const;

  void PrepareMaterialDescriptors(
      FrameContext& frame, const extraction::FrameSnapshot& snapshot);

  void ReleaseGaussianAttributes(GaussianAttributeSlot& slot);

  // Retains source-space Gaussian attributes independently of the
  // camera-dependent prepared stream. Exact snapshot deltas visit only changed
  // resources; exact particle ranges visit only changed elements of the
  // affected attribute arrays when completion safety permits in-place reuse.
  void SyncGaussianAttributes(const extraction::FrameSnapshot& snapshot,
      ResourceSyncMode mode);
  // Reconciles GPU geometry residency with one immutable snapshot. Continuous
  // SceneExtractor revisions select only delta records; unrelated scene edits
  // and static frames do not walk the full geometry table. Revision gaps,
  // foreign sources, and manually constructed snapshots use the full path.
  void SyncGeometry(const extraction::FrameSnapshot& snapshot,
      ResourceSyncMode mode);

  void EnsureRange(DeviceArena& arena, BufferRange& range,
      VkDeviceSize bytes);

  void ReleaseRange(DeviceArena& arena, BufferRange& range);

  void RecordUploads(VkCommandBuffer command, bool release_to_graphics);

  void RecordUploadAcquires(VkCommandBuffer command);

  void Retire(Buffer& buffer);

  void CollectDeferred(std::uint64_t completed,
      bool write_bindless_descriptors = true);

  void ValidateExtent(std::uint32_t width, std::uint32_t height) const;

  static bool IsSupportedAov(Aov aov) noexcept;

  void ValidateRequest(const RenderRequest& request) const;

  static std::vector<Aov> RenderedAovs(const RenderRequest& request);

  static std::vector<Aov> CpuReadbackAovs(const RenderRequest& request);

  FrameContext& AcquireFrame(std::uint32_t width, std::uint32_t height,
      const ShaderPaths& shaders,
      const std::vector<Aov>& cpu_readback_aovs,
      bool gaussian_tile_raster_images, bool gaussian_float_color);

  FrameContext& FindFrame(std::uint64_t completion);

  const FrameContext& FindFrame(std::uint64_t completion) const;

  VkImage CreateImage(std::uint32_t width, std::uint32_t height, VkFormat format,
      VkImageUsageFlags usage, VkDeviceMemory& memory);

  VkImageView CreateImageView(VkImage image, VkFormat format,
      VkImageAspectFlags aspect);

  void EnsureTarget(RenderTarget& target, std::uint32_t width,
      std::uint32_t height,
      const ShaderPaths& shaders,
      const std::vector<Aov>& cpu_readback_aovs,
      bool gaussian_tile_raster_images, bool gaussian_float_color);

  bool GaussianFloatColorSupported();

  bool GaussianTileRasterFormatsSupported();

  static VkImageUsageFlags TargetImageUsage(Aov aov,
      bool gaussian_tile_raster_images);

  void CreateTarget(std::uint32_t width, std::uint32_t height,
      const ShaderPaths& shaders,
      const std::vector<Aov>& cpu_readback_aovs,
      bool gaussian_tile_raster_images, bool gaussian_float_color);

  void CreateRenderPass();

  VkShaderModule GetShaderModule(const std::filesystem::path& path);

  VkPipeline CreatePipeline(
      const ShaderPaths& shaders, std::uint32_t variant_key,
      const GeneratedMaterialArtifact* generated_artifact = nullptr,
      bool gpu_driven_pipeline = false);

  VkPipeline EnsurePipeline(const ShaderPaths& shaders,
      std::uint32_t variant_key);

  VkPipeline EnsureGpuDrivenGraphicsPipeline(const ShaderPaths& shaders,
      std::uint32_t variant_key);

  // Per-instance vertex input of one Gaussian raster stream. The CPU-sorted
  // upload and the GPU-prepared records feed the same shader locations.
  struct GaussianInstanceLayout {
    std::uint32_t stride{};
    std::uint32_t center_pixels{};
    std::uint32_t inverse_conic{};
    std::uint32_t radiance{};
    std::uint32_t opacity{};
    std::uint32_t radius_pixels{};
    std::uint32_t depth{};
    std::uint32_t resource_id{};
    std::uint32_t particle_id{};
  };

  static constexpr GaussianInstanceLayout kCpuGaussianInstanceLayout{
      sizeof(GaussianGpuInstance),
      offsetof(GaussianGpuInstance, center_pixels),
      offsetof(GaussianGpuInstance, inverse_conic),
      offsetof(GaussianGpuInstance, radiance),
      offsetof(GaussianGpuInstance, opacity),
      offsetof(GaussianGpuInstance, radius_pixels),
      offsetof(GaussianGpuInstance, depth),
      offsetof(GaussianGpuInstance, resource_id),
      offsetof(GaussianGpuInstance, particle_id),
  };

  // The low identity word matches the CPU stream's truncated resource handle.
  static constexpr GaussianInstanceLayout kSortedGaussianInstanceLayout{
      sizeof(shader_abi::GaussianPreparedRecord),
      offsetof(shader_abi::GaussianPreparedRecord, center_pixels),
      offsetof(shader_abi::GaussianPreparedRecord, inverse_conic),
      offsetof(shader_abi::GaussianPreparedRecord, radiance),
      offsetof(shader_abi::GaussianPreparedRecord, opacity),
      offsetof(shader_abi::GaussianPreparedRecord, radius_pixels),
      offsetof(shader_abi::GaussianPreparedRecord, depth),
      offsetof(shader_abi::GaussianPreparedRecord, resource_id_low),
      offsetof(shader_abi::GaussianPreparedRecord, particle_id),
  };

  VkPipeline EnsureGaussianPipeline(const ShaderPaths& shaders,
      bool id_pass = false, bool sorted_stream = false);

  VkPipeline EnsureGeneratedPipeline(
      const ShaderPaths& shaders, const GeneratedMaterialArtifact& artifact,
      std::uint32_t variant_key);

  void DestroyTarget(RenderTarget& target) noexcept;

  void DestroySwapchain() noexcept;

  PresentationOverlayContext MakePresentationOverlayContext(
      PresentationOverlayPhase phase,
      VkCommandBuffer command = VK_NULL_HANDLE) const noexcept;

  void InitializePresentationOverlay();

  void ShutdownPresentationOverlay() noexcept;

  VkSurfaceFormatKHR SelectSurfaceFormat() const;

  VkPresentModeKHR SelectPresentMode() const;

  void CreateSwapchain(std::uint32_t requested_width,
      std::uint32_t requested_height);

  void EnsureSwapchain(std::uint32_t width, std::uint32_t height);

  void PreparePresentation(FrameContext& frame, std::uint32_t width,
      std::uint32_t height);

  void RecordPresentation(VkCommandBuffer command,
      const FrameContext& frame);

  void PresentFrame(const FrameContext& frame);

  // Recovers from a Submit failure between image acquisition and queue
  // submission. The acquire semaphore may still carry a pending signal, so it
  // is drained with a wait-only submission rather than destroyed, and the
  // swapchain is retired so the presentation engine releases the
  // never-presented image on recreation.
  void ReclaimAcquiredImage(FrameContext& frame) noexcept;

  struct DrawPipelineVariant {
    std::uint32_t feature_mask{};
    std::uint32_t variant_key{};
  };

  DrawPipelineVariant MakeDrawPipelineVariant(
      const extraction::DrawRecord& draw,
      const extraction::FrameSnapshot& snapshot) const;

  void PreflightGeneratedMaterialPipelines(
      const extraction::FrameSnapshot& snapshot);

  void RecordGaussianGpuPreparation(VkCommandBuffer command,
      const FrameContext& frame);

  void RecordGaussianGpuSort(VkCommandBuffer command,
      const FrameContext& frame);

  // Runs after the sort, whose barriers already make the final scatter and
  // the prepared records visible to later compute work.
  void RecordGaussianGpuRasterGather(VkCommandBuffer command,
      const FrameContext& frame);

  // Runs after the gather, whose barrier already made the draw arguments
  // visible to transfer reads.
  void RecordGaussianGpuTiles(VkCommandBuffer command,
      const FrameContext& frame);

  // Runs after the render pass, whose final layouts leave every attachment
  // ready for transfer reads. Color and IDs move to GENERAL for storage
  // access and depth to read-only for sampling; all return to the transfer-
  // source layout that readback, presentation, and AOV export expect.
  void RecordGaussianGpuTileRaster(VkCommandBuffer command,
      const FrameContext& frame);

  // Native export, host copies, readback, and presentation all consume the
  // same RGBA8 image. Quantize the completed Gaussian composite only once.
  void RecordGaussianColorConversion(VkCommandBuffer command);

  void RecordGpuDrivenDispatch(VkCommandBuffer command,
      const FrameContext& frame,
      const extraction::FrameSnapshot& snapshot,
      const GpuDrivenIndexedRequest& request);

  void RecordFrame(VkCommandBuffer command,
      const FrameContext& frame,
      const extraction::FrameSnapshot& snapshot,
      const Vec4& clear_color,
      const std::vector<Aov>& cpu_readback_aovs);

  std::uint64_t SubmitTransfers(FrameContext& frame);

  std::uint64_t SubmitFrame(FrameContext& frame,
      std::uint64_t transfer_completion);

  void WaitForFrame(FrameContext& frame, std::chrono::nanoseconds timeout);

  std::uint64_t ReadGpuTimestampSpanNanoseconds(
      const FrameContext& frame, std::uint32_t first,
      std::uint32_t second) const;

  std::uint64_t ReadGpuExecutionNanoseconds(const FrameContext& frame) const;

  std::uint64_t ReadGaussianGpuSortNanoseconds(
      const FrameContext& frame) const;

  std::uint64_t ReadGaussianGpuTileNanoseconds(
      const FrameContext& frame) const;

  std::uint64_t ReadGaussianRasterNanoseconds(
      const FrameContext& frame) const;

  ImageRgba8 ReadColor(std::uint32_t width, std::uint32_t height);

  ImageDepth32 ReadDepth(std::uint32_t width, std::uint32_t height);

  ImageUint32 ReadId(std::uint32_t width, std::uint32_t height, Aov aov,
      const Buffer& readback);

  void ResolveGaussianGpuPreparationCounters(FrameContext& frame);

  void ResolveGaussianGpuSortVerification(FrameContext& frame);

  // Runs after sort verification, which has already failed the frame for an
  // out-of-order stream.
  void ResolveGaussianGpuRasterDraw(FrameContext& frame);

  // Runs after the raster draw resolve, which has already checked that the
  // gathered stream covers exactly the verified sorted records.
  void ResolveGaussianGpuTiles(FrameContext& frame);

  void ResolveGpuDrivenCounters(FrameContext& frame);

  static VKAPI_ATTR VkBool32 VKAPI_CALL ValidationCallback(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT type,
      const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
      void* user_data);

  VkInstance instance_{};
  VkSurfaceKHR surface_{};
  VkPhysicalDevice physical_device_{};
  VkDevice device_{};
  VkQueue queue_{};
  VkQueue transfer_queue_{};
  VkDebugUtilsMessengerEXT debug_messenger_{};
  VkSemaphore timeline_semaphore_{};
  VkSemaphore transfer_timeline_semaphore_{};
  std::uint32_t queue_family_{};
  std::uint32_t transfer_queue_family_{VK_QUEUE_FAMILY_IGNORED};
  std::uint32_t selected_timestamp_valid_bits_{};
  float timestamp_period_ns_{};
  std::uint64_t timeline_value_{};
  std::uint64_t transfer_timeline_value_{};
  std::uint64_t transfer_submission_count_{};
  std::uint64_t transfer_uploaded_bytes_{};
  std::uint64_t transfer_ownership_count_{};
  std::uint64_t aov_image_export_count_{};
  std::uint64_t active_aov_image_leases_{};
  std::uint64_t latest_completed_value_{};
  VkDeviceSize uniform_buffer_alignment_{16U};
  VkDeviceSize storage_buffer_alignment_{4U};
  VkDeviceSize max_storage_buffer_range_{};
  std::uint32_t max_draw_indirect_count_{};
  std::uint32_t max_compute_work_group_count_x_{};
  std::uint32_t max_compute_work_group_count_y_{};
  // Lazily queried storage-image support of the color and ID formats and
  // sampled support of the depth format, which tile raster needs.
  std::optional<bool> gaussian_tile_raster_formats_supported_;
  std::optional<bool> gaussian_float_color_supported_;
  std::uint32_t max_per_stage_descriptor_storage_buffers_{};
  std::uint32_t max_descriptor_set_storage_buffers_{};
  bool owns_vulkan_context_{true};
  const std::uint64_t owner_id_{
      g_renderer_owner.fetch_add(1, std::memory_order_relaxed)};
  RendererCapabilities capabilities_;
  RendererStatistics statistics_;
  FrameCounters frame_counters_;
  DeviceMemoryBudget memory_budget_;
  IndexedTableView<extraction::GeometryRecord> geometry_records_;
  IndexedTableView<extraction::TextureRecord> texture_records_;
  IndexedTableView<extraction::SamplerRecord> sampler_records_;
  IndexedTableView<extraction::MaterialRecord> material_records_;
  IndexedTableView<extraction::InstanceRecord> instance_records_;
  DenseTableView<extraction::DrawRecord> draw_records_;
  extraction::PersistentTable<extraction::DrawRecord> mesh_summary_draws_;
  extraction::PersistentTable<extraction::GeometryRecord> mesh_summary_geometries_;
  extraction::PersistentTable<extraction::DrawRecord> generated_preflight_draws_;
  extraction::PersistentTable<extraction::GeometryRecord> generated_preflight_geometries_;
  extraction::PersistentTable<extraction::MaterialRecord> generated_preflight_materials_;
  FrontFaceWinding generated_preflight_front_face_{};
  std::vector<std::pair<std::uint32_t, std::uint32_t>> generated_preflight_variants_;
  std::vector<bool> mesh_material_is_drawn_;
  std::uint64_t mesh_triangle_count_{};
  std::uint64_t geometry_residency_generation_{};
  std::uint64_t material_resource_generation_{};
  detail::GaussianPreparationResult prepared_gaussians_;
  extraction::PersistentTable<extraction::GaussianRecord>
      gaussian_preparation_source_;
  Mat4 gaussian_preparation_view_;
  Mat4 gaussian_preparation_projection_;
  std::uint32_t gaussian_preparation_width_{};
  std::uint32_t gaussian_preparation_height_{};
  bool gaussian_preparation_cache_valid_{};
  std::uint64_t gaussian_preparation_generation_{};
  std::vector<FrameContext> frames_;
  std::vector<RetiredBuffer> deferred_;
  std::vector<RetiredTexture> retired_textures_;
  std::vector<RetiredSampler> retired_samplers_;
  std::filesystem::path environment_path_;
  detail::DiffuseEnvironment environment_lighting_;
  DeviceArena vertex_arena_;
  DeviceArena index_arena_;
  DeviceArena gaussian_position_arena_;
  DeviceArena gaussian_covariance_arena_;
  DeviceArena gaussian_opacity_arena_;
  DeviceArena gaussian_radiance_arena_;
  StagingRing staging_;
  StagingRing gaussian_staging_;
  StagingRing gpu_scene_staging_;
  StagingRing gpu_driven_staging_;
  GpuSceneBuffers gpu_scene_buffers_;
  Buffer gaussian_corner_vertices_;
  std::map<std::uint64_t, GeometrySlot> geometry_slots_;
  std::map<std::uint64_t, GaussianAttributeSlot> gaussian_attribute_slots_;
  std::map<std::uint64_t, TextureSlot> texture_slots_;
  std::map<std::uint64_t, SamplerSlot> sampler_slots_;
  std::vector<RetiredRange> retired_ranges_;
  std::vector<PendingCopy> pending_copies_;
  std::vector<PendingImageCopy> pending_image_copies_;
  std::vector<VkImage> pending_graphics_acquire_images_;
  std::vector<std::uint64_t> pending_texture_handles_;
  std::vector<Buffer> frame_upload_buffers_;
  std::unique_ptr<BindlessTextureTable> bindless_texture_table_;
  std::unique_ptr<BindlessSamplerTable> bindless_sampler_table_;
  std::vector<VkImageView> bindless_texture_views_;
  std::vector<VkSampler> bindless_samplers_;
  std::vector<TextureSlot> reserved_bindless_textures_;
  bool force_reserved_bindless_texture_writes_{};
  VkDescriptorSetLayout bindless_descriptor_set_layout_{};
  VkDescriptorSetLayout bindless_material_descriptor_set_layout_{};
  VkDescriptorSetLayout gpu_driven_descriptor_set_layout_{};
  VkPipelineLayout gpu_driven_pipeline_layout_{};
  std::map<std::filesystem::path, VkPipeline> gpu_driven_pipelines_;
  VkDescriptorSetLayout gaussian_prepare_empty_descriptor_set_layout_{};
  VkDescriptorSetLayout gaussian_prepare_descriptor_set_layout_{};
  VkPipelineLayout gaussian_prepare_pipeline_layout_{};
  std::map<std::filesystem::path, VkPipeline>
      gaussian_prepare_compute_pipelines_;
  VkDescriptorSetLayout gaussian_sort_descriptor_set_layout_{};
  VkPipelineLayout gaussian_sort_pipeline_layout_{};
  std::map<std::filesystem::path, VkPipeline>
      gaussian_sort_compute_pipelines_;
  VkDescriptorSetLayout gaussian_tile_raster_descriptor_set_layout_{};
  VkPipelineLayout gaussian_tile_raster_pipeline_layout_{};
  std::map<std::filesystem::path, VkPipeline>
      gaussian_tile_raster_compute_pipelines_;
  VkDescriptorPool bindless_descriptor_pool_{};
  VkDescriptorSet bindless_descriptor_set_{};
  TextureSlot fallback_texture_;
  SamplerSlot fallback_sampler_;
  VkDescriptorSetLayout descriptor_set_layout_{};
  std::map<std::string, VkDescriptorSetLayout>
      generated_descriptor_set_layouts_;
  std::map<std::string, GeneratedMaterialArtifact>
      generated_material_artifacts_;
  std::vector<const GeneratedMaterialArtifact*> selected_material_artifacts_;
  std::vector<MaterialDiagnostic> frame_material_diagnostics_;
  std::map<std::filesystem::path, VkShaderModule> shader_modules_;
  std::uint64_t resident_snapshot_source_{};
  std::uint64_t resident_snapshot_revision_{};
  bool resource_residency_dirty_{};
  bool presentation_vsync_{true};
  void* presentation_overlay_user_data_{};
  PresentationOptions::RenderOverlay presentation_overlay_{};
  bool presentation_overlay_initialized_{};
  SwapchainState swapchain_;
  RenderTarget* active_target_{};
  std::atomic<std::uint64_t> validation_messages_{};
  DiagnosticSink* diagnostic_sink_{};
};

} // namespace merlin::vulkan
