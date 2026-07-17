#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <merlin/core/render_product.hpp>
#include <merlin/extraction/frame_snapshot.hpp>
#include <merlin/vulkan/bindless_resource_table.hpp>
#include <merlin/vulkan/descriptor_indexing.hpp>

namespace merlin::vulkan {

struct PresentationOptions {
  using CreateSurface = std::int32_t (*)(void* user_data,
                                         std::uintptr_t instance,
                                         std::uintptr_t* surface);

  // Supplied by a backend presentation adapter (GLFW in merlin-viewport).
  // Core and the viewport host never own or inspect the resulting surface.
  std::vector<std::string> required_instance_extensions;
  void* user_data{};
  CreateSurface create_surface{};
  bool vsync{true};
};

struct RendererOptions {
  bool enable_validation{};
  std::uint32_t frames_in_flight{3};
  DescriptorBackendRequest descriptor_backend{
      DescriptorBackendRequest::Automatic};
  std::uint32_t bindless_texture_capacity{4096};
  std::uint32_t bindless_sampler_capacity{256};
  // A zero limit uses the current device-local heap budget. A non-zero limit
  // is a hard cap on device-local memory allocated by this Renderer and is
  // clamped to the driver-reported budget when VK_EXT_memory_budget exists.
  std::uint64_t vram_limit_bytes{};
  bool enable_async_transfer{true};
  std::optional<PresentationOptions> presentation;
};

struct RendererCapabilities {
  std::string device_name;
  std::string driver_name;
  std::string driver_info;
  std::string sdk_version;
  std::uint32_t loader_api_version{};
  std::uint32_t api_version{};
  std::uint32_t header_version{};
  std::uint32_t driver_version{};
  std::uint32_t vendor_id{};
  std::uint32_t device_id{};
  std::uint32_t max_image_dimension_2d{};
  bool timeline_semaphore{};
  bool validation_enabled{};
  bool graphics_queue{};
  bool compute_queue{};
  bool transfer_queue{};
  bool async_transfer_queue{};
  bool queue_ownership_transfers{};
  std::uint32_t graphics_queue_family{};
  std::uint32_t transfer_queue_family{};
  bool memory_budget_extension{};
  std::uint64_t device_local_heap_capacity_bytes{};
  std::uint64_t device_local_heap_budget_bytes{};
  std::uint64_t device_local_heap_usage_bytes{};
  std::uint64_t configured_vram_limit_bytes{};
  // True when the selected graphics queue exposes timestamp bits. Per-frame
  // GPU execution durations are zero only when this capability is false.
  bool timestamp_queries{};
  bool external_presentation{};
  DescriptorIndexingFeatures descriptor_indexing_features;
  DescriptorIndexingLimits descriptor_indexing_limits;
  DescriptorIndexingSelection descriptor_indexing_selection;
};

// Persistent device-local arena state. `resident_bytes` includes ranges that
// have been retired by the scene but must remain alive for an in-flight frame;
// `retiring_bytes` identifies that completion-protected subset explicitly.
// Free-span fields expose fragmentation without relying on a floating-point
// ratio whose formatting would weaken deterministic benchmark evidence.
struct ArenaTelemetry {
  std::uint64_t capacity_bytes{};
  std::uint64_t resident_bytes{};
  std::uint64_t peak_resident_bytes{};
  std::uint64_t free_bytes{};
  std::uint64_t largest_free_span_bytes{};
  std::uint64_t retiring_bytes{};
  std::uint64_t allocation_count{};
  std::uint64_t release_count{};
  std::uint32_t active_ranges{};
  std::uint32_t peak_active_ranges{};
  std::uint32_t retiring_ranges{};
  std::uint32_t free_spans{};
  std::uint32_t blocks{};
  std::uint32_t growth_count{};
};

// Persistently mapped geometry-upload ring state. Reservation bytes include
// alignment padding; resource-class payload bytes remain per-frame counters.
struct UploadRingTelemetry {
  std::uint64_t capacity_bytes{};
  std::uint64_t peak_capacity_bytes{};
  std::uint64_t in_flight_bytes{};
  std::uint64_t peak_in_flight_bytes{};
  std::uint64_t reserved_bytes{};
  std::uint64_t reservation_count{};
  std::uint64_t retired_bytes{};
  std::uint32_t active_regions{};
  std::uint32_t peak_active_regions{};
  std::uint32_t wrap_count{};
  std::uint32_t growth_count{};
  std::uint32_t retired_buffers{};
};

// Device-local heap evidence combines the driver's current heap view with the
// renderer-owned allocations that can be enforced deterministically. Driver
// usage includes other processes and allocations outside this Renderer; the
// renderer fields are therefore retained separately.
struct MemoryBudgetTelemetry {
  bool extension_available{};
  std::uint64_t heap_capacity_bytes{};
  std::uint64_t heap_budget_bytes{};
  std::uint64_t heap_usage_bytes{};
  std::uint64_t heap_available_bytes{};
  std::uint64_t configured_limit_bytes{};
  std::uint64_t effective_limit_bytes{};
  std::uint64_t renderer_allocated_bytes{};
  std::uint64_t renderer_peak_allocated_bytes{};
  std::uint64_t allocation_count{};
  std::uint64_t release_count{};
  std::uint64_t exhaustion_count{};
  std::uint64_t query_count{};
};

struct TransferQueueTelemetry {
  bool asynchronous{};
  std::uint32_t graphics_family{};
  std::uint32_t transfer_family{};
  std::uint64_t submission_count{};
  std::uint64_t uploaded_bytes{};
  std::uint64_t ownership_transfer_count{};
  std::uint64_t latest_timeline_value{};
};

struct RendererStatistics {
  std::uint64_t frames_submitted{};
  std::uint64_t frames_presented{};
  std::uint64_t swapchain_recreates{};
  std::uint64_t scene_uploads{};
  std::uint64_t validation_messages{};
  std::uint32_t frame_context_count{};
  // Suballocated geometry ranges reclaimed after their retire-frame completed,
  // and ranges still awaiting completion. Retirement is deterministic: a range
  // released while frame N is being prepared is reclaimed exactly when frame
  // N-1 (the last submission that could reference it) completes.
  std::uint64_t geometry_range_retirements{};
  std::uint32_t pending_geometry_retirements{};
  std::uint32_t geometry_arena_blocks{};
  ArenaTelemetry vertex_arena;
  ArenaTelemetry index_arena;
  UploadRingTelemetry upload_ring;
  MemoryBudgetTelemetry memory_budget;
  TransferQueueTelemetry transfer_queue;
  // Logical bindless-table evidence is populated when the selected device and
  // configuration activate bindless Forward. Conventional Forward leaves
  // these fields zeroed and remains the correctness fallback.
  bool bindless_resource_tables{};
  BindlessSlotTelemetry bindless_texture_slots;
  BindlessSamplerTelemetry bindless_samplers;
};

// Backend-owned durations for one frame. Fields are CPU wall-clock durations
// except gpu_execution_ns, which comes from device timestamps. Nanoseconds keep
// the result machine-readable without floating-point formatting differences
// between standard library implementations.
struct FrameCpuTimings {
  // CPU work that reconciles immutable snapshot resources with GPU residency.
  std::uint64_t upload_ns{};
  std::uint64_t command_recording_ns{};
  std::uint64_t queue_submission_ns{};
  std::uint64_t completion_wait_ns{};
  std::uint64_t readback_ns{};
  std::uint64_t presentation_ns{};
  // Device timestamps span the graphics submission: single-queue uploads,
  // draws, and selected AOV copies. Dedicated transfer-queue execution is
  // synchronized but not folded into this graphics-queue duration.
  std::uint64_t gpu_execution_ns{};
  std::uint64_t backend_total_ns{};
};

// Structural counters describe work performed by one submission and resolve.
// They are stable enough for CI assertions even when wall-clock timings are
// too noisy for performance comparisons.
struct FrameCounters {
  std::uint64_t draw_count{};
  std::uint64_t visible_primitive_count{};
  std::uint64_t triangle_count{};
  std::uint64_t upload_bytes{};
  // Upload payload split by resource class. The sum equals upload_bytes for
  // the current Mesh/material implementation.
  std::uint64_t vertex_upload_bytes{};
  std::uint64_t index_upload_bytes{};
  std::uint64_t texture_upload_bytes{};
  // Aligned space reserved from the persistent mapped geometry-upload ring.
  // Texture uploads currently use completion-retired staging buffers and are
  // therefore excluded.
  std::uint64_t upload_ring_reserved_bytes{};
  std::uint64_t readback_bytes{};
  std::uint64_t requested_aov_mask{};
  std::uint64_t rendered_aov_mask{};
  std::uint64_t cpu_readback_aov_mask{};
  std::uint64_t requested_aov_count{};
  std::uint64_t rendered_aov_count{};
  std::uint64_t cpu_readback_aov_count{};
  std::uint64_t wait_count{};
  std::uint64_t resolve_count{};
  std::uint64_t map_count{};
  std::uint64_t allocation_count{};
  std::uint64_t buffer_allocation_count{};
  std::uint64_t image_allocation_count{};
  std::uint64_t buffer_allocation_bytes{};
  std::uint64_t image_allocation_bytes{};
  std::uint64_t pipeline_creation_count{};
  std::uint64_t scene_cache_hits{};
  std::uint64_t scene_cache_misses{};
  std::uint64_t geometry_cache_hits{};
  std::uint64_t geometry_cache_misses{};
  std::uint64_t texture_cache_hits{};
  std::uint64_t texture_cache_misses{};
  std::uint64_t sampler_cache_hits{};
  std::uint64_t sampler_cache_misses{};
  // Number of resource entries selected for residency reconciliation. A
  // SceneExtractor delta keeps these at zero for a static frame and bounded by
  // changed resources for a continuous revision stream. Full-fallback
  // reconciliation reports every selected record and removal.
  std::uint64_t geometry_reconcile_count{};
  std::uint64_t texture_reconcile_count{};
  std::uint64_t sampler_reconcile_count{};
  std::uint64_t buffer_suballocation_count{};
  std::uint64_t buffer_range_release_count{};
  // Resident ranges kept at the same block/offset for a changed payload.
  std::uint64_t geometry_range_reuse_count{};
  std::uint64_t geometry_arena_growth_count{};
  std::uint64_t geometry_arena_growth_bytes{};
  std::uint64_t upload_ring_growth_count{};
  std::uint64_t upload_ring_growth_bytes{};
  std::uint64_t pipeline_cache_hits{};
  std::uint64_t pipeline_cache_misses{};
  std::uint64_t shader_module_cache_hits{};
  std::uint64_t shader_module_cache_misses{};
  std::uint64_t descriptor_layout_cache_hits{};
  std::uint64_t descriptor_layout_cache_misses{};
  std::uint64_t descriptor_pool_creation_count{};
  std::uint64_t descriptor_allocation_count{};
  std::uint64_t descriptor_update_count{};
  // Global table writes are split out from the conventional per-material
  // descriptor work so localized-edit and steady-state evidence can measure
  // the bindless residency contract directly.
  std::uint64_t bindless_sampled_image_descriptor_update_count{};
  std::uint64_t bindless_sampler_descriptor_update_count{};
  std::uint64_t transfer_submission_count{};
  std::uint64_t queue_ownership_transfer_count{};
  std::uint64_t present_count{};
  std::uint64_t presentation_copy_bytes{};

  // Member-wise equality keeps steady-state drift detection in lockstep with
  // this field list; adding a counter cannot silently escape the comparison.
  bool operator==(const FrameCounters&) const = default;
};

struct ShaderPaths {
  std::filesystem::path vertex;
  std::filesystem::path fragment;
  std::filesystem::path bindless_vertex;
  std::filesystem::path bindless_fragment;
  std::filesystem::path environment;

  friend bool operator==(const ShaderPaths&, const ShaderPaths&) = default;
};

struct RenderProductRequest {
  Aov aov{Aov::Color};
  bool cpu_readback{true};

  friend constexpr bool operator==(const RenderProductRequest&,
                                   const RenderProductRequest&) = default;
};

struct RenderRequest {
  // The request owns the immutable scene boundary until Submit has recorded
  // every command that refers to it. Completion owns only backend frame,
  // target, and readback resources; callers may release the request itself
  // immediately after Submit returns.
  std::shared_ptr<const extraction::FrameSnapshot> snapshot;
  std::uint32_t width{512};
  std::uint32_t height{512};
  ShaderPaths shaders;
  Vec4 clear_color{kDefaultClearColor};
  std::vector<RenderProductRequest> products{
      {Aov::Color, true}, {Aov::Depth, true}};
  // Presentation uses the renderer's backend-owned default target. The color
  // attachment is copied GPU-to-GPU into the acquired swapchain image; CPU
  // readback remains independently controlled by the product requests.
  bool present{};
};

enum class RendererErrorCode {
  InvalidRequest,
  InvalidToken,
  ResourceBusy,
  Timeout,
  DeviceLost,
  Unsupported,
  BackendFailure,
  ResourceExhausted,
};

[[nodiscard]] std::string_view RendererErrorCodeName(
    RendererErrorCode code) noexcept;

class RendererError : public std::runtime_error {
 public:
  RendererError(RendererErrorCode code, std::string operation,
                std::string detail, std::int32_t native_code = 0);

  [[nodiscard]] RendererErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& operation() const noexcept {
    return operation_;
  }
  [[nodiscard]] const std::string& detail() const noexcept { return detail_; }
  [[nodiscard]] std::int32_t native_code() const noexcept {
    return native_code_;
  }

 private:
  RendererErrorCode code_;
  std::string operation_;
  std::string detail_;
  std::int32_t native_code_{};
};

class CompletionToken {
 public:
  CompletionToken() = default;

  [[nodiscard]] explicit operator bool() const noexcept { return value_ != 0; }
  [[nodiscard]] std::uint64_t value() const noexcept { return value_; }

  friend constexpr bool operator==(const CompletionToken&,
                                   const CompletionToken&) = default;

 private:
  friend class Renderer;
  CompletionToken(std::uint64_t owner, std::uint64_t value) noexcept
      : owner_(owner), value_(value) {}

  std::uint64_t owner_{};
  std::uint64_t value_{};
};

struct ImageRgba8 {
  RenderProduct product;
  std::uint32_t row_pitch_bytes{};
  std::vector<std::uint8_t> pixels;
};

struct ImageDepth32 {
  RenderProduct product;
  std::uint32_t row_pitch_bytes{};
  std::vector<float> pixels;
};

struct ImageUint32 {
  RenderProduct product;
  std::uint32_t row_pitch_bytes{};
  std::vector<std::uint32_t> pixels;
};

struct RenderResult {
  ImageRgba8 color;
  ImageDepth32 depth;
  ImageUint32 prim_id;
  ImageUint32 instance_id;
  std::vector<Aov> rendered_aovs;
  std::vector<Aov> cpu_readback_aovs;
  std::uint64_t scene_revision{};
  std::uint64_t completion_value{};
  FrameCpuTimings cpu_timings;
  FrameCounters counters;
};

[[nodiscard]] bool HasAov(const std::vector<Aov>& aovs, Aov aov) noexcept;
[[nodiscard]] bool HasCpuReadback(const RenderResult& result, Aov aov) noexcept;

// Throws std::invalid_argument when a selected backend result violates Merlin's
// Tier 0 CPU readback contract. Rendered GPU-only AOVs have no payload to
// validate.
void ValidateRenderResult(const RenderResult& result);

class Renderer {
 public:
  explicit Renderer(RendererOptions options = {});
  ~Renderer();

  Renderer(Renderer&&) noexcept;
  Renderer& operator=(Renderer&&) noexcept;
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  [[nodiscard]] const RendererCapabilities& capabilities() const noexcept;
  [[nodiscard]] RendererStatistics statistics() const noexcept;
  [[nodiscard]] CompletionToken Submit(const RenderRequest& request);
  [[nodiscard]] bool IsComplete(CompletionToken token) const;
  [[nodiscard]] RenderResult Resolve(
      CompletionToken token,
      std::chrono::nanoseconds timeout = std::chrono::nanoseconds::max());

  // Convenience wrapper for synchronous callers. New execution paths should
  // construct a RenderRequest explicitly so AOV production and CPU readback
  // are visible at the call site.
  [[nodiscard]] RenderResult Render(const extraction::FrameSnapshot& snapshot,
                                    std::uint32_t width,
                                    std::uint32_t height,
                                    const ShaderPaths& shaders);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace merlin::vulkan
