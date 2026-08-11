#pragma once

#include <cstdint>
#include <memory>
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

struct GpuScenePackingCapacities {
  std::uint32_t geometries{};
  std::uint32_t instances{};
  std::uint32_t materials{};
  std::uint32_t draws{};
};

struct GpuScenePackingInputs {
  std::span<const GpuGeometryPlacement> geometry_placements;
  std::span<const GpuInstanceIdentity> instance_identities;
  std::span<const GpuMaterialBinding> material_bindings;
};

struct GpuScenePackedFrameUpdate {
  GpuSceneResourceUpdatePlan geometry_plan;
  GpuSceneResourceUpdatePlan instance_plan;
  GpuSceneResourceUpdatePlan material_plan;
  GpuSceneDrawUpdatePlan draw_plan;
  GpuScenePackedUpdate<GpuGeometry> geometries;
  GpuScenePackedUpdate<GpuInstance> instances;
  GpuScenePackedUpdate<GpuMaterial> materials;
  GpuScenePackedUpdate<GpuDraw> draws;
  std::uint64_t copy_bytes{};
};

// Owns the four persistent GPU Scene mappings as one transaction boundary.
// Apply evaluates the snapshot against cloned candidate mappings, packs every
// dirty record, and publishes the candidates only after all packing succeeds.
// A rejected update therefore leaves source/revision, generations,
// retirements, telemetry, and free-slot order unchanged.
class GpuScenePackingState {
 public:
  explicit GpuScenePackingState(GpuScenePackingCapacities capacities);
  GpuScenePackingState(const GpuScenePackingState&) = delete;
  GpuScenePackingState& operator=(const GpuScenePackingState&) = delete;

  [[nodiscard]] GpuScenePackedFrameUpdate Apply(
      const extraction::FrameSnapshot& snapshot,
      std::uint64_t last_completion_value,
      std::uint64_t completed_value,
      const GpuScenePackingInputs& inputs);

  [[nodiscard]] std::uint64_t source_id() const noexcept {
    return geometries_->source_id();
  }
  [[nodiscard]] std::uint64_t revision() const noexcept {
    return geometries_->revision();
  }
  [[nodiscard]] std::size_t geometry_count() const noexcept {
    return geometries_->size();
  }
  [[nodiscard]] std::size_t instance_count() const noexcept {
    return instances_->size();
  }
  [[nodiscard]] std::size_t material_count() const noexcept {
    return materials_->size();
  }
  [[nodiscard]] std::size_t draw_count() const noexcept {
    return draws_->size();
  }
  [[nodiscard]] std::optional<GpuSceneSlotHandle> FindGeometry(
      std::uint64_t resource) const noexcept {
    return geometries_->Find(resource);
  }
  [[nodiscard]] std::optional<GpuSceneSlotHandle> FindInstance(
      std::uint64_t resource) const noexcept {
    return instances_->Find(resource);
  }
  [[nodiscard]] std::optional<GpuSceneSlotHandle> FindMaterial(
      std::uint64_t resource) const noexcept {
    return materials_->Find(resource);
  }
  [[nodiscard]] std::optional<GpuSceneSlotHandle> FindDraw(
      std::uint64_t draw) const noexcept {
    return draws_->Find(draw);
  }

 private:
  std::unique_ptr<GpuSceneResourceSlots> geometries_;
  std::unique_ptr<GpuSceneResourceSlots> instances_;
  std::unique_ptr<GpuSceneResourceSlots> materials_;
  std::unique_ptr<GpuSceneDrawSlots> draws_;
};

}  // namespace merlin::render
