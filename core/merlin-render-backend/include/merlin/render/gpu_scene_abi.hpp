#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <merlin/core/types.hpp>

namespace merlin::render {

// Common CPU/shader layout for persistent GPU Scene tables. Bump the version
// whenever any record's meaning, field order, size, or alignment changes.
inline constexpr std::uint32_t kGpuSceneAbiVersion = 1;

inline constexpr std::uint32_t kInvalidGpuSceneTableIndex = ~std::uint32_t{};

// Geometry arena offsets are byte offsets. Version 1 deliberately uses 32-bit
// offsets so Vulkan and Metal share the same ordinary-data layout without a
// shader 64-bit capability requirement. A backend must reject an arena that
// cannot be represented instead of truncating it.
struct alignas(16) GpuGeometry {
  std::uint32_t vertex_offset{};
  std::uint32_t vertex_count{};
  std::uint32_t index_offset{};
  std::uint32_t index_count{};
  std::uint32_t index_type{};
  std::uint32_t attribute_mask{};
  std::uint32_t meshlet_offset{kInvalidGpuSceneTableIndex};
  std::uint32_t meshlet_count{};
  // xyz contains the object-space bound. w is reserved and must be zero.
  Vec4 bounds_min;
  Vec4 bounds_max;
};

// normal_matrix_columns stores the three world normal-matrix columns in xyz;
// each w component is reserved and must be zero. object_id and instance_id are
// the stable 32-bit identities written to selection AOVs.
struct alignas(16) GpuInstance {
  Mat4 transform;
  std::array<Vec4, 3> normal_matrix_columns;
  std::uint32_t object_id{};
  std::uint32_t instance_id{};
  std::uint32_t visibility_mask{~std::uint32_t{}};
  std::uint32_t flags{};
};

// surface_factors contains metallic, roughness, alpha cutoff, and one reserved
// zero in that order. Resource indices name the persistent bindless tables;
// an absent resource uses kInvalidGpuSceneTableIndex.
struct alignas(16) GpuMaterial {
  Vec4 base_color{1.0F, 1.0F, 1.0F, 1.0F};
  Vec4 surface_factors{0.0F, 0.5F, 0.5F, 0.0F};
  std::uint32_t material_class_flags{};
  std::uint32_t base_color_texture_index{kInvalidGpuSceneTableIndex};
  std::uint32_t base_color_sampler_index{kInvalidGpuSceneTableIndex};
  std::uint32_t base_color_texcoord_set{};
};

// draw_id_low/high preserve the source-local 64-bit DrawRecord identity across
// record replacement and visible-list compaction. The completion-safe table
// slot is only the shader-visible address of one resident record and may change
// while an older version remains in flight.
struct alignas(16) GpuDraw {
  std::uint32_t geometry_index{kInvalidGpuSceneTableIndex};
  std::uint32_t material_index{kInvalidGpuSceneTableIndex};
  std::uint32_t instance_index{kInvalidGpuSceneTableIndex};
  std::uint32_t primitive_base{};
  std::uint32_t primitive_count{};
  std::uint32_t flags{};
  std::uint32_t draw_id_low{};
  std::uint32_t draw_id_high{};
};

constexpr void SetGpuDrawIdentity(GpuDraw& draw,
                                  std::uint64_t identity) noexcept {
  draw.draw_id_low = static_cast<std::uint32_t>(identity);
  draw.draw_id_high = static_cast<std::uint32_t>(identity >> 32U);
}

[[nodiscard]] constexpr std::uint64_t GpuDrawIdentity(
    const GpuDraw& draw) noexcept {
  return static_cast<std::uint64_t>(draw.draw_id_low) |
         (static_cast<std::uint64_t>(draw.draw_id_high) << 32U);
}

static_assert(std::is_standard_layout_v<GpuGeometry>);
static_assert(std::is_trivially_copyable_v<GpuGeometry>);
static_assert(sizeof(GpuGeometry) == 64);
static_assert(alignof(GpuGeometry) == 16);
static_assert(offsetof(GpuGeometry, vertex_offset) == 0);
static_assert(offsetof(GpuGeometry, vertex_count) == 4);
static_assert(offsetof(GpuGeometry, index_offset) == 8);
static_assert(offsetof(GpuGeometry, index_count) == 12);
static_assert(offsetof(GpuGeometry, index_type) == 16);
static_assert(offsetof(GpuGeometry, attribute_mask) == 20);
static_assert(offsetof(GpuGeometry, meshlet_offset) == 24);
static_assert(offsetof(GpuGeometry, meshlet_count) == 28);
static_assert(offsetof(GpuGeometry, bounds_min) == 32);
static_assert(offsetof(GpuGeometry, bounds_max) == 48);

static_assert(std::is_standard_layout_v<GpuInstance>);
static_assert(std::is_trivially_copyable_v<GpuInstance>);
static_assert(sizeof(GpuInstance) == 128);
static_assert(alignof(GpuInstance) == 16);
static_assert(offsetof(GpuInstance, transform) == 0);
static_assert(offsetof(GpuInstance, normal_matrix_columns) == 64);
static_assert(offsetof(GpuInstance, object_id) == 112);
static_assert(offsetof(GpuInstance, instance_id) == 116);
static_assert(offsetof(GpuInstance, visibility_mask) == 120);
static_assert(offsetof(GpuInstance, flags) == 124);

static_assert(std::is_standard_layout_v<GpuMaterial>);
static_assert(std::is_trivially_copyable_v<GpuMaterial>);
static_assert(sizeof(GpuMaterial) == 48);
static_assert(alignof(GpuMaterial) == 16);
static_assert(offsetof(GpuMaterial, base_color) == 0);
static_assert(offsetof(GpuMaterial, surface_factors) == 16);
static_assert(offsetof(GpuMaterial, material_class_flags) == 32);
static_assert(offsetof(GpuMaterial, base_color_texture_index) == 36);
static_assert(offsetof(GpuMaterial, base_color_sampler_index) == 40);
static_assert(offsetof(GpuMaterial, base_color_texcoord_set) == 44);

static_assert(std::is_standard_layout_v<GpuDraw>);
static_assert(std::is_trivially_copyable_v<GpuDraw>);
static_assert(sizeof(GpuDraw) == 32);
static_assert(alignof(GpuDraw) == 16);
static_assert(offsetof(GpuDraw, geometry_index) == 0);
static_assert(offsetof(GpuDraw, material_index) == 4);
static_assert(offsetof(GpuDraw, instance_index) == 8);
static_assert(offsetof(GpuDraw, primitive_base) == 12);
static_assert(offsetof(GpuDraw, primitive_count) == 16);
static_assert(offsetof(GpuDraw, flags) == 20);
static_assert(offsetof(GpuDraw, draw_id_low) == 24);
static_assert(offsetof(GpuDraw, draw_id_high) == 28);

}  // namespace merlin::render
