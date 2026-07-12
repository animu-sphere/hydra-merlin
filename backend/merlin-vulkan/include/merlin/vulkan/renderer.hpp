#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <merlin/core/render_product.hpp>
#include <merlin/extraction/frame_snapshot.hpp>

namespace merlin::vulkan {

struct RendererOptions {
  bool enable_validation{};
  std::uint32_t frames_in_flight{3};
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
};

struct RendererStatistics {
  std::uint64_t frames_submitted{};
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
};

// CPU wall-clock durations for the backend-owned portions of one frame.
// Nanoseconds keep the result machine-readable without floating-point
// formatting differences between standard library implementations.
struct FrameCpuTimings {
  std::uint64_t upload_ns{};
  std::uint64_t command_recording_ns{};
  std::uint64_t readback_ns{};
  std::uint64_t backend_total_ns{};
};

// Structural counters describe work performed by a single Render() call.
// They are stable enough for CI assertions even when wall-clock timings are
// too noisy for performance comparisons.
struct FrameCounters {
  std::uint64_t draw_count{};
  std::uint64_t triangle_count{};
  std::uint64_t upload_bytes{};
  std::uint64_t readback_bytes{};
  std::uint64_t allocation_count{};
  std::uint64_t buffer_allocation_count{};
  std::uint64_t image_allocation_count{};
  std::uint64_t pipeline_creation_count{};
  std::uint64_t scene_cache_hits{};
  std::uint64_t scene_cache_misses{};
  std::uint64_t geometry_cache_hits{};
  std::uint64_t geometry_cache_misses{};
  std::uint64_t buffer_suballocation_count{};
  std::uint64_t buffer_range_release_count{};
  std::uint64_t pipeline_cache_hits{};
  std::uint64_t pipeline_cache_misses{};

  // Member-wise equality keeps steady-state drift detection in lockstep with
  // this field list; adding a counter cannot silently escape the comparison.
  bool operator==(const FrameCounters&) const = default;
};

struct ShaderPaths {
  std::filesystem::path vertex;
  std::filesystem::path fragment;
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

struct RenderResult {
  ImageRgba8 color;
  ImageDepth32 depth;
  std::uint64_t scene_revision{};
  std::uint64_t completion_value{};
  FrameCpuTimings cpu_timings;
  FrameCounters counters;
};

// Throws std::invalid_argument when a backend result violates Merlin's Tier 0
// CPU readback contract.
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
  [[nodiscard]] RenderResult Render(const extraction::FrameSnapshot& snapshot,
                                    std::uint32_t width,
                                    std::uint32_t height,
                                    const ShaderPaths& shaders);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace merlin::vulkan
