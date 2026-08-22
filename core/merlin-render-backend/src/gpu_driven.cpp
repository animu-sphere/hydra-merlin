#include <merlin/render/gpu_driven.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string_view>
#include <utility>

#include <merlin/extraction/frame_snapshot.hpp>
#include <merlin/render/gpu_scene_packing.hpp>

namespace merlin::render {
namespace {

[[noreturn]] void Throw(GpuDrivenIndexedErrorCode code,
                        std::string_view message) {
  throw GpuDrivenIndexedError(code, std::string(message));
}

bool IsFinite(const Mat4& value) noexcept {
  for (const auto component : value.values) {
    if (!std::isfinite(component)) {
      return false;
    }
  }
  return true;
}

bool IsFinite(const Vec4& value) noexcept {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z) && std::isfinite(value.w);
}

Vec4 Transform(const Mat4& matrix, const Vec4& value) noexcept {
  return {
      matrix.values[0] * value.x + matrix.values[4] * value.y +
          matrix.values[8] * value.z + matrix.values[12] * value.w,
      matrix.values[1] * value.x + matrix.values[5] * value.y +
          matrix.values[9] * value.z + matrix.values[13] * value.w,
      matrix.values[2] * value.x + matrix.values[6] * value.y +
          matrix.values[10] * value.z + matrix.values[14] * value.w,
      matrix.values[3] * value.x + matrix.values[7] * value.y +
          matrix.values[11] * value.z + matrix.values[15] * value.w,
  };
}

bool OutsideFrustum(const GpuGeometry& geometry,
                    const GpuInstance& instance,
                    const Mat4& view_projection) noexcept {
  const std::array<float, 2> xs{geometry.bounds_min.x,
                                geometry.bounds_max.x};
  const std::array<float, 2> ys{geometry.bounds_min.y,
                                geometry.bounds_max.y};
  const std::array<float, 2> zs{geometry.bounds_min.z,
                                geometry.bounds_max.z};
  std::array<Vec4, 8> clip_corners;
  std::size_t corner_index{};
  for (const auto z : zs) {
    for (const auto y : ys) {
      for (const auto x : xs) {
        const auto world = Transform(instance.transform, {x, y, z, 1.0F});
        clip_corners[corner_index++] = Transform(view_projection, world);
      }
    }
  }

  // Renderer zero-to-one clip volume: -w <= x,y <= w and 0 <= z <= w. Reject
  // only when every corner is outside one plane, which is conservative for
  // transformed object-space AABBs.
  const auto all = [&clip_corners](auto predicate) {
    for (const auto& corner : clip_corners) {
      if (!predicate(corner)) {
        return false;
      }
    }
    return true;
  };
  return all([](const Vec4& value) { return value.x < -value.w; }) ||
         all([](const Vec4& value) { return value.x > value.w; }) ||
         all([](const Vec4& value) { return value.y < -value.w; }) ||
         all([](const Vec4& value) { return value.y > value.w; }) ||
         all([](const Vec4& value) { return value.z < 0.0F; }) ||
         all([](const Vec4& value) { return value.z > value.w; });
}

void ValidateRecord(const GpuGeometry& geometry,
                    const GpuInstance& instance,
                    const GpuDraw& draw) {
  if (geometry.index_type != kGpuGeometryIndexTypeUint32) {
    Throw(GpuDrivenIndexedErrorCode::InvalidRecord,
          "GPU-driven indexed geometry is not uint32 indexed");
  }
  if (geometry.vertex_count == 0 || geometry.index_count == 0 ||
      geometry.index_count % 3U != 0U || draw.primitive_count == 0 ||
      GpuDrawIdentity(draw) == 0) {
    Throw(GpuDrivenIndexedErrorCode::InvalidRecord,
          "GPU-driven indexed candidate has an invalid count or identity");
  }
  if (!IsFinite(geometry.bounds_min) || !IsFinite(geometry.bounds_max) ||
      geometry.bounds_min.w != 0.0F || geometry.bounds_max.w != 0.0F ||
      geometry.bounds_min.x > geometry.bounds_max.x ||
      geometry.bounds_min.y > geometry.bounds_max.y ||
      geometry.bounds_min.z > geometry.bounds_max.z ||
      !IsFinite(instance.transform)) {
    Throw(GpuDrivenIndexedErrorCode::InvalidRecord,
          "GPU-driven indexed candidate has invalid bounds or transform");
  }

  constexpr auto addressable_bytes =
      std::uint64_t{std::numeric_limits<std::uint32_t>::max()} + 1U;
  const auto vertex_end = static_cast<std::uint64_t>(geometry.vertex_offset) +
                          static_cast<std::uint64_t>(geometry.vertex_count) *
                              sizeof(extraction::DrawVertex);
  const auto index_end = static_cast<std::uint64_t>(geometry.index_offset) +
                         static_cast<std::uint64_t>(geometry.index_count) *
                             sizeof(std::uint32_t);
  if (vertex_end > addressable_bytes || index_end > addressable_bytes) {
    Throw(GpuDrivenIndexedErrorCode::UnrepresentableGeometry,
          "GPU-driven geometry range exceeds the ABI v1 address space");
  }

  const auto first_index =
      static_cast<std::uint64_t>(draw.primitive_base) * 3U;
  const auto index_count =
      static_cast<std::uint64_t>(draw.primitive_count) * 3U;
  if (first_index + index_count > geometry.index_count) {
    Throw(GpuDrivenIndexedErrorCode::InvalidRecord,
          "GPU-driven indexed draw range exceeds its geometry");
  }
}

GpuIndexedIndirectCommand MakeCommand(const GpuGeometry& geometry,
                                      const GpuDraw& draw,
                                      std::uint32_t draw_slot) {
  constexpr auto vertex_stride =
      static_cast<std::uint32_t>(sizeof(extraction::DrawVertex));
  constexpr auto index_stride =
      static_cast<std::uint32_t>(sizeof(std::uint32_t));
  if (geometry.vertex_offset % vertex_stride != 0U ||
      geometry.index_offset % index_stride != 0U) {
    Throw(GpuDrivenIndexedErrorCode::UnrepresentableGeometry,
          "GPU-driven geometry arena offsets are not element aligned");
  }
  const auto vertex_offset = geometry.vertex_offset / vertex_stride;
  const auto first_index = geometry.index_offset / index_stride +
                           draw.primitive_base * 3U;
  const auto index_count = draw.primitive_count * 3U;
  if (vertex_offset >
      static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
    Throw(GpuDrivenIndexedErrorCode::UnrepresentableGeometry,
          "GPU-driven vertex offset exceeds the indirect command ABI");
  }
  return {index_count, 1U, first_index,
          static_cast<std::int32_t>(vertex_offset), draw_slot};
}

}  // namespace

GpuDrivenIndexedError::GpuDrivenIndexedError(
    GpuDrivenIndexedErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

GpuDrivenIndexedPlan BuildGpuDrivenIndexedPlan(
    std::span<const std::uint32_t> candidate_draw_slots,
    std::span<const GpuGeometry> geometries,
    std::span<const GpuInstance> instances,
    std::span<const GpuMaterial> materials,
    std::span<const GpuDraw> draws,
    const GpuDrivenIndexedConfig& config) {
  if (!IsFinite(config.view_projection)) {
    Throw(GpuDrivenIndexedErrorCode::InvalidConfiguration,
          "GPU-driven view-projection matrix is not finite");
  }
  if (candidate_draw_slots.size() >
      std::numeric_limits<std::uint32_t>::max()) {
    Throw(GpuDrivenIndexedErrorCode::InvalidConfiguration,
          "GPU-driven candidate count exceeds the command-count ABI");
  }

  GpuDrivenIndexedPlan result;
  result.visible_draw_slots.reserve(candidate_draw_slots.size());
  result.commands.reserve(candidate_draw_slots.size());
  result.counters.candidate_draw_count = candidate_draw_slots.size();

  std::vector<bool> seen_draw_slots(draws.size());
  for (const auto draw_slot : candidate_draw_slots) {
    if (draw_slot >= draws.size()) {
      Throw(GpuDrivenIndexedErrorCode::MissingResidency,
            "GPU-driven candidate names a missing draw slot");
    }
    if (seen_draw_slots[draw_slot]) {
      Throw(GpuDrivenIndexedErrorCode::InvalidCandidate,
            "GPU-driven candidate draw slot is duplicated");
    }
    seen_draw_slots[draw_slot] = true;

    const auto& draw = draws[draw_slot];
    if (draw.geometry_index >= geometries.size() ||
        draw.material_index >= materials.size() ||
        draw.instance_index >= instances.size()) {
      Throw(GpuDrivenIndexedErrorCode::MissingResidency,
            "GPU-driven draw references a missing GPU Scene record");
    }
    const auto& geometry = geometries[draw.geometry_index];
    const auto& instance = instances[draw.instance_index];
    ValidateRecord(geometry, instance, draw);

    if (config.enable_visibility_mask_culling &&
        (instance.visibility_mask & config.visibility_mask) == 0U) {
      ++result.counters.visibility_mask_culled_count;
      continue;
    }
    if (config.enable_frustum_culling &&
        OutsideFrustum(geometry, instance, config.view_projection)) {
      ++result.counters.frustum_culled_count;
      continue;
    }

    result.visible_draw_slots.push_back(draw_slot);
    result.commands.push_back(MakeCommand(geometry, draw, draw_slot));
  }

  result.counters.visible_draw_count = result.visible_draw_slots.size();
  result.counters.indirect_command_count = result.commands.size();
  return result;
}

}  // namespace merlin::render
