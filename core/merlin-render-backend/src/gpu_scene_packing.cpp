#include <merlin/render/gpu_scene_packing.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <string_view>
#include <utility>

namespace merlin::render {
namespace {

[[noreturn]] void Throw(GpuScenePackingErrorCode code,
                        std::string_view detail) {
  throw GpuScenePackingError(code,
                             "GPU Scene record packing: " +
                                 std::string(detail));
}

std::uint32_t CheckedU32(std::uint64_t value, std::string_view name) {
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    Throw(GpuScenePackingErrorCode::UnrepresentableValue,
          std::string(name) + " exceeds the ABI v1 32-bit range");
  }
  return static_cast<std::uint32_t>(value);
}

void RequirePlan(const GpuSceneResourceUpdatePlan& plan,
                 GpuSceneResourceTable table,
                 const extraction::FrameSnapshot& snapshot) {
  if (plan.table != table) {
    Throw(GpuScenePackingErrorCode::InvalidPlan,
          "resource update plan targets the wrong table");
  }
  if (plan.source_id != snapshot.source_id ||
      plan.revision != snapshot.revision) {
    Throw(GpuScenePackingErrorCode::InvalidPlan,
          "resource update plan source or revision does not match the snapshot");
  }
}

template <typename Record>
struct PendingTable {
  std::vector<std::uint32_t> slots;
  std::vector<Record> records;

  void Reserve(std::size_t count) {
    slots.reserve(count);
    records.reserve(count);
  }

  void Add(std::uint32_t slot, Record record) {
    slots.push_back(slot);
    records.push_back(std::move(record));
  }
};

template <typename Record>
GpuScenePackedUpdate<Record> Finish(
    PendingTable<Record> pending,
    const std::vector<GpuSceneDirtyRange>& expected_ranges) {
  std::vector<std::size_t> order(pending.slots.size());
  std::iota(order.begin(), order.end(), 0U);
  std::sort(order.begin(), order.end(), [&](std::size_t left,
                                             std::size_t right) {
    return pending.slots[left] < pending.slots[right];
  });
  for (std::size_t index = 1; index < order.size(); ++index) {
    if (pending.slots[order[index - 1U]] == pending.slots[order[index]]) {
      Throw(GpuScenePackingErrorCode::InvalidPlan,
            "update plan writes one physical slot more than once");
    }
  }

  GpuScenePackedUpdate<Record> result;
  result.record_count = order.size();
  result.copy_bytes = result.record_count * sizeof(Record);
  for (const auto index : order) {
    const auto slot = pending.slots[index];
    if (result.ranges.empty() ||
        slot != result.ranges.back().first_slot +
                    result.ranges.back().records.size()) {
      result.ranges.push_back({slot, {}});
    }
    result.ranges.back().records.push_back(
        std::move(pending.records[index]));
  }

  if (result.ranges.size() != expected_ranges.size()) {
    Throw(GpuScenePackingErrorCode::InvalidPlan,
          "packed ranges disagree with the update plan dirty ranges");
  }
  for (std::size_t index = 0; index < result.ranges.size(); ++index) {
    if (result.ranges[index].first_slot !=
            expected_ranges[index].first_slot ||
        result.ranges[index].records.size() !=
            expected_ranges[index].slot_count) {
      Throw(GpuScenePackingErrorCode::InvalidPlan,
            "packed ranges disagree with the update plan dirty ranges");
    }
  }
  return result;
}

bool IsFinite(const Vec3& value) noexcept {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

bool IsFinite(const Vec4& value) noexcept {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z) && std::isfinite(value.w);
}

std::array<Vec4, 3> NormalMatrix(const Mat4& transform) noexcept {
  const Vec3 column0{transform.values[0], transform.values[1],
                     transform.values[2]};
  const Vec3 column1{transform.values[4], transform.values[5],
                     transform.values[6]};
  const Vec3 column2{transform.values[8], transform.values[9],
                     transform.values[10]};
  const auto cross = [](const Vec3& left, const Vec3& right) {
    return Vec3{left.y * right.z - left.z * right.y,
                left.z * right.x - left.x * right.z,
                left.x * right.y - left.y * right.x};
  };
  const auto cofactor0 = cross(column1, column2);
  const auto cofactor1 = cross(column2, column0);
  const auto cofactor2 = cross(column0, column1);
  const auto determinant = column0.x * cofactor0.x +
                           column0.y * cofactor0.y +
                           column0.z * cofactor0.z;
  if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-20F) {
    return {Vec4{1.0F, 0.0F, 0.0F, 0.0F},
            Vec4{0.0F, 1.0F, 0.0F, 0.0F},
            Vec4{0.0F, 0.0F, 1.0F, 0.0F}};
  }
  const auto inverse = 1.0F / determinant;
  const auto column = [inverse](const Vec3& value) {
    return Vec4{value.x * inverse, value.y * inverse, value.z * inverse,
                0.0F};
  };
  return {column(cofactor0), column(cofactor1), column(cofactor2)};
}

GpuGeometry PackGeometry(const extraction::GeometryRecord& source,
                         const GpuGeometryPlacement& placement) {
  if (!source.vertices || !source.indices || source.vertices->empty() ||
      source.indices->empty() || source.indices->size() % 3U != 0U) {
    Throw(GpuScenePackingErrorCode::InvalidRecord,
          "geometry payload must contain indexed triangles");
  }
  if (std::any_of(source.indices->begin(), source.indices->end(),
                  [&](std::uint32_t index) {
                    return index >= source.vertices->size();
                  })) {
    Throw(GpuScenePackingErrorCode::InvalidRecord,
          "geometry index is outside the vertex payload");
  }

  auto minimum = source.vertices->front().position;
  auto maximum = minimum;
  if (!IsFinite(minimum)) {
    Throw(GpuScenePackingErrorCode::InvalidRecord,
          "geometry position is not finite");
  }
  for (const auto& vertex : *source.vertices) {
    if (!IsFinite(vertex.position)) {
      Throw(GpuScenePackingErrorCode::InvalidRecord,
            "geometry position is not finite");
    }
    minimum.x = std::min(minimum.x, vertex.position.x);
    minimum.y = std::min(minimum.y, vertex.position.y);
    minimum.z = std::min(minimum.z, vertex.position.z);
    maximum.x = std::max(maximum.x, vertex.position.x);
    maximum.y = std::max(maximum.y, vertex.position.y);
    maximum.z = std::max(maximum.z, vertex.position.z);
  }

  GpuGeometry result;
  result.vertex_offset = CheckedU32(placement.vertex_offset, "vertex offset");
  result.vertex_count = CheckedU32(source.vertices->size(), "vertex count");
  result.index_offset = CheckedU32(placement.index_offset, "index offset");
  result.index_count = CheckedU32(source.indices->size(), "index count");
  const auto vertex_bytes =
      static_cast<std::uint64_t>(result.vertex_count) *
      sizeof(extraction::DrawVertex);
  const auto index_bytes =
      static_cast<std::uint64_t>(result.index_count) * sizeof(std::uint32_t);
  constexpr auto addressable_bytes =
      std::uint64_t{std::numeric_limits<std::uint32_t>::max()} + 1U;
  if (placement.vertex_offset + vertex_bytes > addressable_bytes ||
      placement.index_offset + index_bytes > addressable_bytes) {
    Throw(GpuScenePackingErrorCode::UnrepresentableValue,
          "geometry arena range exceeds the ABI v1 32-bit address space");
  }
  result.index_type = kGpuGeometryIndexTypeUint32;
  if (source.has_normals) {
    result.attribute_mask |= kGpuGeometryHasNormals;
  }
  if (source.has_colors) {
    result.attribute_mask |= kGpuGeometryHasColors;
  }
  if (source.has_texcoords) {
    result.attribute_mask |= kGpuGeometryHasTexcoords;
  }
  result.bounds_min = {minimum.x, minimum.y, minimum.z, 0.0F};
  result.bounds_max = {maximum.x, maximum.y, maximum.z, 0.0F};
  return result;
}

GpuInstance PackInstance(const extraction::InstanceRecord& source,
                         const GpuInstanceIdentity& identity) {
  if (source.instance == 0 || source.mesh == 0) {
    Throw(GpuScenePackingErrorCode::InvalidRecord,
          "instance and mesh identities must be non-zero");
  }
  if (std::any_of(source.transform.values.begin(), source.transform.values.end(),
                  [](float value) { return !std::isfinite(value); })) {
    Throw(GpuScenePackingErrorCode::InvalidRecord,
          "instance transform is not finite");
  }
  GpuInstance result;
  result.transform = source.transform;
  result.normal_matrix_columns = NormalMatrix(source.transform);
  result.object_id = identity.object_id;
  result.instance_id = identity.instance_id;
  result.visibility_mask = source.visible ? identity.visibility_mask : 0U;
  result.flags = identity.flags;
  return result;
}

GpuMaterial PackMaterial(const extraction::MaterialRecord& source,
                         const GpuMaterialBinding& binding) {
  if (!IsFinite(source.parameters.base_color) ||
      !std::isfinite(source.parameters.metallic) ||
      !std::isfinite(source.parameters.roughness) ||
      !std::isfinite(source.parameters.alpha_cutoff)) {
    Throw(GpuScenePackingErrorCode::InvalidRecord,
          "material parameters are not finite");
  }
  const auto feature_flags = static_cast<std::uint32_t>(source.features);
  if ((feature_flags & ~kGpuMaterialFeatureMask) != 0U) {
    Throw(GpuScenePackingErrorCode::InvalidRecord,
          "material feature flags exceed the ABI v1 class mask");
  }
  const bool has_texture = source.base_color_texture.has_value();
  const bool has_packed_texture =
      binding.texture_index != kInvalidGpuSceneTableIndex;
  const bool has_packed_sampler =
      binding.sampler_index != kInvalidGpuSceneTableIndex;
  if (has_texture != has_packed_texture ||
      has_texture != has_packed_sampler) {
    Throw(GpuScenePackingErrorCode::MissingResidency,
          "material texture and sampler residency is incomplete");
  }

  GpuMaterial result;
  result.base_color = source.parameters.base_color;
  result.surface_factors = {source.parameters.metallic,
                            source.parameters.roughness,
                            source.parameters.alpha_cutoff, 0.0F};
  result.material_class_flags = feature_flags;
  if (source.alpha_mode == AlphaMode::Masked) {
    result.material_class_flags |= kGpuMaterialAlphaMasked;
  } else if (source.alpha_mode == AlphaMode::Blended) {
    result.material_class_flags |= kGpuMaterialAlphaBlended;
  }
  if (source.double_sided) {
    result.material_class_flags |= kGpuMaterialDoubleSided;
  }
  result.base_color_texture_index = binding.texture_index;
  result.base_color_sampler_index = binding.sampler_index;
  if (source.base_color_texture) {
    result.base_color_texcoord_set = source.base_color_texture->texcoord_set;
  }
  return result;
}

GpuSceneSlotHandle RequireResident(const GpuSceneResourceSlots& slots,
                                   std::uint64_t resource,
                                   std::string_view name) {
  const auto slot = slots.Find(resource);
  if (!slot) {
    Throw(GpuScenePackingErrorCode::MissingResidency,
          std::string(name) + " resource has no persistent GPU Scene slot");
  }
  return *slot;
}

}  // namespace

GpuScenePackingError::GpuScenePackingError(GpuScenePackingErrorCode code,
                                           std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

GpuScenePackedUpdate<GpuGeometry> PackGpuGeometryUpdate(
    const extraction::FrameSnapshot& snapshot,
    const GpuSceneResourceUpdatePlan& plan,
    std::span<const GpuGeometryPlacement> placements) {
  RequirePlan(plan, GpuSceneResourceTable::Geometry, snapshot);
  if (placements.size() != snapshot.geometries.size()) {
    Throw(GpuScenePackingErrorCode::InvalidPlan,
          "geometry placement count does not match the snapshot");
  }
  PendingTable<GpuGeometry> pending;
  pending.Reserve(plan.upserts.size());
  for (const auto& upsert : plan.upserts) {
    if (!upsert.slot || upsert.snapshot_index >= snapshot.geometries.size()) {
      Throw(GpuScenePackingErrorCode::InvalidPlan,
            "geometry upsert has an invalid slot or snapshot index");
    }
    const auto& source = snapshot.geometries[upsert.snapshot_index];
    if (source.mesh != upsert.resource ||
        upsert.record_version !=
            GpuSceneResourceVersion{source.vertex_revision,
                                    source.index_revision}) {
      Throw(GpuScenePackingErrorCode::InvalidPlan,
            "geometry upsert identity does not match the snapshot");
    }
    pending.Add(upsert.slot.index,
                PackGeometry(source, placements[upsert.snapshot_index]));
  }
  return Finish(std::move(pending), plan.dirty_ranges);
}

GpuScenePackedUpdate<GpuInstance> PackGpuInstanceUpdate(
    const extraction::FrameSnapshot& snapshot,
    const GpuSceneResourceUpdatePlan& plan,
    std::span<const GpuInstanceIdentity> identities) {
  RequirePlan(plan, GpuSceneResourceTable::Instance, snapshot);
  if (identities.size() != snapshot.instances.size()) {
    Throw(GpuScenePackingErrorCode::InvalidPlan,
          "instance identity count does not match the snapshot");
  }
  PendingTable<GpuInstance> pending;
  pending.Reserve(plan.upserts.size());
  for (const auto& upsert : plan.upserts) {
    if (!upsert.slot || upsert.snapshot_index >= snapshot.instances.size()) {
      Throw(GpuScenePackingErrorCode::InvalidPlan,
            "instance upsert has an invalid slot or snapshot index");
    }
    const auto& source = snapshot.instances[upsert.snapshot_index];
    if (source.instance != upsert.resource ||
        upsert.record_version !=
            GpuSceneResourceVersion{source.revision, 0}) {
      Throw(GpuScenePackingErrorCode::InvalidPlan,
            "instance upsert identity does not match the snapshot");
    }
    pending.Add(upsert.slot.index,
                PackInstance(source, identities[upsert.snapshot_index]));
  }
  return Finish(std::move(pending), plan.dirty_ranges);
}

GpuScenePackedUpdate<GpuMaterial> PackGpuMaterialUpdate(
    const extraction::FrameSnapshot& snapshot,
    const GpuSceneResourceUpdatePlan& plan,
    std::span<const GpuMaterialBinding> bindings) {
  RequirePlan(plan, GpuSceneResourceTable::Material, snapshot);
  if (bindings.size() != snapshot.materials.size()) {
    Throw(GpuScenePackingErrorCode::InvalidPlan,
          "material binding count does not match the snapshot");
  }
  PendingTable<GpuMaterial> pending;
  pending.Reserve(plan.upserts.size());
  for (const auto& upsert : plan.upserts) {
    if (!upsert.slot || upsert.snapshot_index >= snapshot.materials.size()) {
      Throw(GpuScenePackingErrorCode::InvalidPlan,
            "material upsert has an invalid slot or snapshot index");
    }
    const auto& source = snapshot.materials[upsert.snapshot_index];
    if (source.material != upsert.resource ||
        upsert.record_version !=
            GpuSceneResourceVersion{source.revision, 0}) {
      Throw(GpuScenePackingErrorCode::InvalidPlan,
            "material upsert identity does not match the snapshot");
    }
    pending.Add(upsert.slot.index,
                PackMaterial(source, bindings[upsert.snapshot_index]));
  }
  return Finish(std::move(pending), plan.dirty_ranges);
}

GpuScenePackedUpdate<GpuDraw> PackGpuDrawUpdate(
    const extraction::FrameSnapshot& snapshot,
    const GpuSceneDrawUpdatePlan& plan,
    const GpuSceneResourceSlots& geometries,
    const GpuSceneResourceSlots& materials,
    const GpuSceneResourceSlots& instances) {
  if (plan.source_id != snapshot.source_id ||
      plan.revision != snapshot.revision) {
    Throw(GpuScenePackingErrorCode::InvalidPlan,
          "draw update plan source or revision does not match the snapshot");
  }
  if (geometries.table() != GpuSceneResourceTable::Geometry ||
      materials.table() != GpuSceneResourceTable::Material ||
      instances.table() != GpuSceneResourceTable::Instance) {
    Throw(GpuScenePackingErrorCode::InvalidPlan,
          "draw packing received a resource slot table with the wrong kind");
  }
  if (geometries.source_id() != snapshot.source_id ||
      geometries.revision() != snapshot.revision ||
      materials.source_id() != snapshot.source_id ||
      materials.revision() != snapshot.revision ||
      instances.source_id() != snapshot.source_id ||
      instances.revision() != snapshot.revision) {
    Throw(GpuScenePackingErrorCode::MissingResidency,
          "resource slot tables do not represent the packed snapshot");
  }

  PendingTable<GpuDraw> pending;
  pending.Reserve(plan.upserts.size());
  for (const auto& upsert : plan.upserts) {
    if (!upsert.slot || upsert.snapshot_index >= snapshot.draws.size()) {
      Throw(GpuScenePackingErrorCode::InvalidPlan,
            "draw upsert has an invalid slot or snapshot index");
    }
    const auto& source = snapshot.draws[upsert.snapshot_index];
    if (source.draw == 0 || source.draw != upsert.draw ||
        source.revision != upsert.record_revision ||
        source.geometry_index >= snapshot.geometries.size() ||
        source.material_index >= snapshot.materials.size() ||
        source.instance_index >= snapshot.instances.size()) {
      Throw(GpuScenePackingErrorCode::InvalidRecord,
            "draw identity or resource index is invalid");
    }
    const auto& geometry = snapshot.geometries[source.geometry_index];
    if (!geometry.indices || geometry.indices->size() % 3U != 0U) {
      Throw(GpuScenePackingErrorCode::InvalidRecord,
            "draw geometry does not contain indexed triangles");
    }

    GpuDraw record;
    record.geometry_index =
        RequireResident(geometries, geometry.mesh, "geometry").index;
    record.material_index =
        RequireResident(materials,
                        snapshot.materials[source.material_index].material,
                        "material")
            .index;
    record.instance_index =
        RequireResident(instances,
                        snapshot.instances[source.instance_index].instance,
                        "instance")
            .index;
    record.primitive_count =
        CheckedU32(geometry.indices->size() / 3U, "primitive count");
    SetGpuDrawIdentity(record, source.draw);
    pending.Add(upsert.slot.index, record);
  }
  return Finish(std::move(pending), plan.dirty_ranges);
}

}  // namespace merlin::render
