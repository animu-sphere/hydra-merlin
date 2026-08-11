#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <merlin/render/gpu_scene.hpp>

namespace merlin::render {

// Shader-visible values for the version 1 GPU Scene ABI. These values are
// renderer-owned rather than native API enums so Vulkan and Metal pack the
// same records.
inline constexpr std::uint32_t kGpuGeometryIndexTypeUint32 = 1U;
inline constexpr std::uint32_t kGpuGeometryHasNormals = 1U << 0U;
inline constexpr std::uint32_t kGpuGeometryHasColors = 1U << 1U;
inline constexpr std::uint32_t kGpuGeometryHasTexcoords = 1U << 2U;

inline constexpr std::uint32_t kGpuMaterialFeatureMask = 0x0000ffffU;
inline constexpr std::uint32_t kGpuMaterialAlphaMasked = 1U << 16U;
inline constexpr std::uint32_t kGpuMaterialAlphaBlended = 1U << 17U;
inline constexpr std::uint32_t kGpuMaterialDoubleSided = 1U << 18U;

enum class GpuScenePackingErrorCode {
  InvalidPlan,
  InvalidRecord,
  MissingResidency,
  UnrepresentableValue,
};

class GpuScenePackingError : public std::runtime_error {
 public:
  GpuScenePackingError(GpuScenePackingErrorCode code, std::string message);

  [[nodiscard]] GpuScenePackingErrorCode code() const noexcept {
    return code_;
  }

 private:
  GpuScenePackingErrorCode code_;
};

// Native geometry residency supplies byte offsets into the common vertex and
// index arenas. ABI v1 deliberately rejects offsets above 4 GiB instead of
// truncating them.
struct GpuGeometryPlacement {
  std::uint64_t vertex_offset{};
  std::uint64_t index_offset{};
};

// Object and instance IDs remain adapter/backend policy. Packing requires the
// selected stable 32-bit identities explicitly so a serialized 64-bit Core
// handle is never truncated accidentally.
struct GpuInstanceIdentity {
  std::uint32_t object_id{};
  std::uint32_t instance_id{};
  std::uint32_t visibility_mask{~std::uint32_t{}};
  std::uint32_t flags{};
};

// Physical indices into the persistent sampled-image and sampler tables.
// Both stay invalid for an untextured material.
struct GpuMaterialBinding {
  std::uint32_t texture_index{kInvalidGpuSceneTableIndex};
  std::uint32_t sampler_index{kInvalidGpuSceneTableIndex};
};

template <typename Record>
struct GpuScenePackedRange {
  std::uint32_t first_slot{};
  std::vector<Record> records;
};

// A packed update contains only newly allocated physical slots. Ranges are
// ordered and contiguous, so a native backend can stage one copy per range at
// first_slot * sizeof(Record) without scanning the full persistent table.
template <typename Record>
struct GpuScenePackedUpdate {
  std::vector<GpuScenePackedRange<Record>> ranges;
  std::uint64_t record_count{};
  std::uint64_t copy_bytes{};
};

[[nodiscard]] GpuScenePackedUpdate<GpuGeometry> PackGpuGeometryUpdate(
    const extraction::FrameSnapshot& snapshot,
    const GpuSceneResourceUpdatePlan& plan,
    std::span<const GpuGeometryPlacement> placements);

[[nodiscard]] GpuScenePackedUpdate<GpuInstance> PackGpuInstanceUpdate(
    const extraction::FrameSnapshot& snapshot,
    const GpuSceneResourceUpdatePlan& plan,
    std::span<const GpuInstanceIdentity> identities);

[[nodiscard]] GpuScenePackedUpdate<GpuMaterial> PackGpuMaterialUpdate(
    const extraction::FrameSnapshot& snapshot,
    const GpuSceneResourceUpdatePlan& plan,
    std::span<const GpuMaterialBinding> bindings);

[[nodiscard]] GpuScenePackedUpdate<GpuDraw> PackGpuDrawUpdate(
    const extraction::FrameSnapshot& snapshot,
    const GpuSceneDrawUpdatePlan& plan,
    const GpuSceneResourceSlots& geometries,
    const GpuSceneResourceSlots& materials,
    const GpuSceneResourceSlots& instances);

}  // namespace merlin::render
