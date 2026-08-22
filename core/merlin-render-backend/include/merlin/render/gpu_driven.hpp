#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include <merlin/render/gpu_scene_abi.hpp>

namespace merlin::render {

// Five-field indexed-indirect command shared by native backends. Keeping this
// ordinary-data layout backend-neutral lets the CPU reference path and future
// native compute implementations share one command-generation contract.
struct GpuIndexedIndirectCommand {
  std::uint32_t index_count{};
  std::uint32_t instance_count{1};
  std::uint32_t first_index{};
  std::int32_t vertex_offset{};
  // The persistent GpuDraw slot, not a dense or compacted draw position.
  std::uint32_t first_instance{};
};

static_assert(std::is_standard_layout_v<GpuIndexedIndirectCommand>);
static_assert(std::is_trivially_copyable_v<GpuIndexedIndirectCommand>);
static_assert(sizeof(GpuIndexedIndirectCommand) == 20);
static_assert(alignof(GpuIndexedIndirectCommand) == 4);
static_assert(offsetof(GpuIndexedIndirectCommand, index_count) == 0);
static_assert(offsetof(GpuIndexedIndirectCommand, instance_count) == 4);
static_assert(offsetof(GpuIndexedIndirectCommand, first_index) == 8);
static_assert(offsetof(GpuIndexedIndirectCommand, vertex_offset) == 12);
static_assert(offsetof(GpuIndexedIndirectCommand, first_instance) == 16);

enum class GpuDrivenIndexedErrorCode {
  InvalidConfiguration,
  InvalidCandidate,
  MissingResidency,
  InvalidRecord,
  UnrepresentableGeometry,
};

class GpuDrivenIndexedError : public std::runtime_error {
 public:
  GpuDrivenIndexedError(GpuDrivenIndexedErrorCode code, std::string message);

  [[nodiscard]] GpuDrivenIndexedErrorCode code() const noexcept {
    return code_;
  }

 private:
  GpuDrivenIndexedErrorCode code_;
};

struct GpuDrivenIndexedConfig {
  Mat4 view_projection;
  std::uint32_t visibility_mask{~std::uint32_t{}};
  bool enable_visibility_mask_culling{true};
  bool enable_frustum_culling{true};
};

// Native geometry arenas may grow into multiple buffers whose byte offsets
// each restart at zero. This physical identity stays outside the common shader
// record while allowing the reference planner to form executable batches.
struct GpuDrivenGeometryBinding {
  std::uint32_t vertex_arena_block{kInvalidGpuSceneTableIndex};
  std::uint32_t index_arena_block{kInvalidGpuSceneTableIndex};

  friend constexpr bool operator==(const GpuDrivenGeometryBinding&,
                                   const GpuDrivenGeometryBinding&) = default;
};

struct GpuDrivenIndexedCounters {
  std::uint64_t candidate_draw_count{};
  std::uint64_t visible_draw_count{};
  std::uint64_t visibility_mask_culled_count{};
  std::uint64_t frustum_culled_count{};
  std::uint64_t indirect_command_count{};
  std::uint64_t indirect_batch_count{};
};

// Commands in one batch share the vertex and index buffers that a native API
// binds before executing its indirect command range. Batch sequence and command
// order preserve visible candidate order, and the physical draw slot remains
// the command's first-instance identity.
struct GpuDrivenIndexedBatch {
  GpuDrivenGeometryBinding binding;
  std::vector<std::uint32_t> visible_draw_slots;
  std::vector<GpuIndexedIndirectCommand> commands;
};

// CPU reference result for the first GPU-driven indexed Forward stage. The
// global visible slot list retains compaction order for validation and future
// shader consumers; native APIs execute the block-safe command batches.
struct GpuDrivenIndexedPlan {
  std::vector<std::uint32_t> visible_draw_slots;
  std::vector<GpuDrivenIndexedBatch> batches;
  GpuDrivenIndexedCounters counters;
};

// Builds the deterministic validation oracle for candidate culling,
// compaction, and indexed-indirect command generation. Tables are physical
// GPU Scene mirrors, and candidate_draw_slots name their persistent GpuDraw
// addresses. Invalid input is rejected before a partial plan is returned.
[[nodiscard]] GpuDrivenIndexedPlan BuildGpuDrivenIndexedPlan(
    std::span<const std::uint32_t> candidate_draw_slots,
    std::span<const GpuGeometry> geometries,
    std::span<const GpuDrivenGeometryBinding> geometry_bindings,
    std::span<const GpuInstance> instances,
    std::span<const GpuMaterial> materials,
    std::span<const GpuDraw> draws,
    const GpuDrivenIndexedConfig& config);

}  // namespace merlin::render
