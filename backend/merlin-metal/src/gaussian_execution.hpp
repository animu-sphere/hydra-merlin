#pragma once

#include "gaussian_compute_abi.hpp"
#include "gaussian_residency.hpp"
#include <merlin/extraction/gaussian_preparation.hpp>

namespace merlin::metal {

inline constexpr NSUInteger kGaussianTimestampCount = 12;
inline constexpr NSUInteger kGaussianColorConvertBegin = 10;
inline constexpr NSUInteger kGaussianColorConvertEnd = 11;
inline constexpr NSUInteger kGaussianPrepareBegin = 0;
inline constexpr NSUInteger kGaussianPrepareEnd = 1;
inline constexpr NSUInteger kGaussianSortBegin = 2;
inline constexpr NSUInteger kGaussianSortEnd = 3;
inline constexpr NSUInteger kGaussianRasterBegin = 4;
inline constexpr NSUInteger kGaussianRasterEnd = 5;
inline constexpr NSUInteger kGaussianTileBegin = 6;
inline constexpr NSUInteger kGaussianTileEnd = 7;
inline constexpr NSUInteger kGaussianTileRasterBegin = 8;
inline constexpr NSUInteger kGaussianTileRasterEnd = 9;

// Backend-private prepare/sort/gather scheduling. Calls are externally
// serialized. Outputs may be consumed by a render encoder in the same command;
// no visible-count readback or CPU particle traversal is needed for scheduling.
class GaussianExecution {
  struct Budget {
    std::atomic<std::uint64_t> live{};
    std::uint64_t limit{};
  };

public:
  struct Scratch {
    Scratch() = default;
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    ~Scratch();
    id<MTLBuffer> prepared = nil;
    id<MTLBuffer> sort[2] = {nil, nil};
    id<MTLBuffer> control = nil;
    id<MTLBuffer> counters = nil;
    id<MTLBuffer> classifications = nil;
    id<MTLBuffer> instances = nil;
    id<MTLBuffer> draw = nil;
    id<MTLBuffer> tile_pairs[2] = {nil, nil};
    id<MTLBuffer> tile_control = nil;
    id<MTLBuffer> tile_sorted = nil;
    std::uint32_t particle_capacity{};
    std::uint32_t resource_capacity{};
    std::uint32_t pair_capacity{};
    std::uint32_t tile_control_words{};
    std::uint64_t bytes{};

  private:
    friend class GaussianExecution;
    std::shared_ptr<Budget> budget;
  };

  struct Frame {
    std::shared_ptr<const Scratch> scratch;
    std::uint32_t particle_count{};
    std::uint32_t padded_count{};
    std::uint32_t resource_count{};
    extraction::GaussianSortingPolicy sorting_policy;
    std::uint64_t allocation_count{};
    std::uint64_t dispatch_count{};
    bool tiled{};
    gaussian_compute::TileRasterConstants tile_raster{};
    id<MTLBuffer> grouped_pairs = nil;
    // Eight radix passes leave the sorted elements in scratch->sort[1].
  };

  GaussianExecution(id<MTLDevice> device, id<MTLLibrary> library,
      std::uint64_t scratch_byte_budget);
  GaussianExecution(const GaussianExecution&) = delete;
  GaussianExecution& operator=(const GaussianExecution&) = delete;

  // Encode the residency update before calling this method, and commit it
  // only after submitting the command. Camera matrices/extent belong to this
  // frame; all resource metadata comes from the immutable residency update.
  // device_count requires exactly one resource. validate enables GPU order
  // checks and poisoned output guards, for diagnostic captures only.
  // On encoding failure, discard the entire unsubmitted command buffer.
  std::shared_ptr<const Frame> Encode(const std::shared_ptr<GaussianResidency::Update>& attributes,
      const Mat4& view, const Mat4& projection, std::uint32_t width, std::uint32_t height,
      id<MTLCommandBuffer> command, bool device_count = false, bool validate = false,
      id<MTLCounterSampleBuffer> timestamps = nil, bool tiled = false,
      std::uint32_t requested_pair_capacity = 0);
  void EncodeTileRaster(const Frame& frame, id<MTLCommandBuffer> command,
      id<MTLTexture> color, id<MTLTexture> depth, id<MTLTexture> prim_id,
      id<MTLTexture> instance_id, id<MTLCounterSampleBuffer> timestamps = nil) const;
  // Drop cached scratch. Unfinished commands and retained Frames keep their
  // allocations charged to the budget until they release them.
  void Reset();
  [[nodiscard]] std::uint64_t live_bytes() const noexcept;

private:
  std::shared_ptr<Scratch> Acquire(std::uint32_t particles, std::uint32_t resources,
      std::uint32_t pairs, std::uint32_t tile_words, std::uint64_t& allocations);
  id<MTLDevice> device_;
  id<MTLComputePipelineState> prepare_, keys_, histogram_, scan_, add_, scatter_, verify_, gather_;
  id<MTLComputePipelineState> tile_count_, tile_emit_, tile_ranges_, tile_verify_,
      tile_select_, tile_raster_;
  std::shared_ptr<Budget> budget_;
  std::vector<std::shared_ptr<Scratch>> pool_;
};

} // namespace merlin::metal
