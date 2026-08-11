#include <merlin/render/gpu_scene_packing.hpp>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace {

using merlin::AlphaMode;
using merlin::MaterialFeature;
using merlin::Vec3;
using merlin::extraction::DrawRecord;
using merlin::extraction::DrawVertex;
using merlin::extraction::FrameSnapshot;
using merlin::extraction::GeometryRecord;
using merlin::extraction::InstanceRecord;
using merlin::extraction::MaterialRecord;
using merlin::extraction::TextureBindingRecord;
using merlin::render::GpuGeometryPlacement;
using merlin::render::GpuInstanceIdentity;
using merlin::render::GpuMaterialBinding;
using merlin::render::GpuSceneDrawSlots;
using merlin::render::GpuScenePackingError;
using merlin::render::GpuScenePackingErrorCode;
using merlin::render::GpuSceneResourceSlots;
using merlin::render::GpuSceneResourceTable;

template <typename Callback>
void ExpectError(Callback&& callback, GpuScenePackingErrorCode code,
                 std::string_view fragment) {
  try {
    callback();
    assert(false && "expected GpuScenePackingError");
  } catch (const GpuScenePackingError& error) {
    assert(error.code() == code);
    assert(std::string_view(error.what()).find(fragment) !=
           std::string_view::npos);
  }
}

bool Near(float left, float right) {
  return std::abs(left - right) <= 1.0e-6F;
}

FrameSnapshot MakeSnapshot() {
  FrameSnapshot snapshot;
  snapshot.source_id = 9;
  snapshot.revision = 1;

  GeometryRecord geometry;
  geometry.mesh = 101;
  geometry.vertex_revision = 2;
  geometry.index_revision = 3;
  geometry.has_normals = true;
  geometry.has_colors = true;
  geometry.has_texcoords = true;
  geometry.vertices = std::make_shared<const std::vector<DrawVertex>>(
      std::vector<DrawVertex>{{Vec3{-2.0F, 1.0F, 4.0F}},
                              {Vec3{3.0F, -5.0F, 2.0F}},
                              {Vec3{1.0F, 2.0F, -6.0F}}});
  geometry.indices = std::make_shared<const std::vector<std::uint32_t>>(
      std::vector<std::uint32_t>{0, 1, 2});
  snapshot.geometries.assign({geometry});

  MaterialRecord material;
  material.material = 202;
  material.revision = 4;
  material.parameters.base_color = {0.1F, 0.2F, 0.3F, 0.4F};
  material.parameters.metallic = 0.75F;
  material.parameters.roughness = 0.25F;
  material.parameters.alpha_cutoff = 0.6F;
  material.features = MaterialFeature::VertexColor |
                      MaterialFeature::BaseColorTexture;
  material.alpha_mode = AlphaMode::Masked;
  material.double_sided = true;
  material.base_color_texture = TextureBindingRecord{0, 0, 1};
  snapshot.materials.assign({material});

  InstanceRecord instance;
  instance.instance = 303;
  instance.mesh = geometry.mesh;
  instance.material = material.material;
  instance.revision = 5;
  instance.transform.values[0] = 2.0F;
  instance.transform.values[5] = 4.0F;
  instance.transform.values[10] = 8.0F;
  snapshot.instances.assign({instance});

  DrawRecord draw;
  draw.geometry_index = 0;
  draw.material_index = 0;
  draw.instance_index = 0;
  draw.draw = 0x1234567887654321ULL;
  draw.revision = 6;
  snapshot.draws.assign({draw});
  return snapshot;
}

void TestPackedRecordsAndRanges() {
  auto snapshot = MakeSnapshot();
  GpuSceneResourceSlots geometries(GpuSceneResourceTable::Geometry, 4);
  GpuSceneResourceSlots materials(GpuSceneResourceTable::Material, 4);
  GpuSceneResourceSlots instances(GpuSceneResourceTable::Instance, 4);
  GpuSceneDrawSlots draws(4);

  const auto geometry_plan = geometries.Apply(snapshot, 0, 0);
  const auto material_plan = materials.Apply(snapshot, 0, 0);
  const auto instance_plan = instances.Apply(snapshot, 0, 0);
  const auto draw_plan = draws.Apply(snapshot, 0, 0);

  const std::vector placements{GpuGeometryPlacement{64, 256}};
  const std::vector identities{
      GpuInstanceIdentity{17, 23, 0x00ff00ffU, 9}};
  const std::vector bindings{GpuMaterialBinding{7, 11}};

  const auto packed_geometry = merlin::render::PackGpuGeometryUpdate(
      snapshot, geometry_plan, placements);
  assert(packed_geometry.ranges.size() == 1);
  assert(packed_geometry.ranges[0].first_slot == 0);
  assert(packed_geometry.record_count == 1);
  assert(packed_geometry.copy_bytes == sizeof(merlin::render::GpuGeometry));
  const auto& geometry = packed_geometry.ranges[0].records[0];
  assert(geometry.vertex_offset == 64);
  assert(geometry.vertex_count == 3);
  assert(geometry.index_offset == 256);
  assert(geometry.index_count == 3);
  assert(geometry.index_type ==
         merlin::render::kGpuGeometryIndexTypeUint32);
  assert(geometry.attribute_mask ==
         (merlin::render::kGpuGeometryHasNormals |
          merlin::render::kGpuGeometryHasColors |
          merlin::render::kGpuGeometryHasTexcoords));
  assert(Near(geometry.bounds_min.x, -2.0F));
  assert(Near(geometry.bounds_min.y, -5.0F));
  assert(Near(geometry.bounds_min.z, -6.0F));
  assert(Near(geometry.bounds_max.x, 3.0F));
  assert(Near(geometry.bounds_max.y, 2.0F));
  assert(Near(geometry.bounds_max.z, 4.0F));
  assert(geometry.bounds_min.w == 0.0F);
  assert(geometry.bounds_max.w == 0.0F);

  const auto packed_instance = merlin::render::PackGpuInstanceUpdate(
      snapshot, instance_plan, identities);
  const auto& instance = packed_instance.ranges[0].records[0];
  assert(instance.object_id == 17);
  assert(instance.instance_id == 23);
  assert(instance.visibility_mask == 0x00ff00ffU);
  assert(instance.flags == 9);
  assert(Near(instance.normal_matrix_columns[0].x, 0.5F));
  assert(Near(instance.normal_matrix_columns[1].y, 0.25F));
  assert(Near(instance.normal_matrix_columns[2].z, 0.125F));
  assert(instance.normal_matrix_columns[0].w == 0.0F);

  const auto packed_material = merlin::render::PackGpuMaterialUpdate(
      snapshot, material_plan, bindings);
  const auto& material = packed_material.ranges[0].records[0];
  assert(Near(material.base_color.x, 0.1F));
  assert(Near(material.surface_factors.x, 0.75F));
  assert(Near(material.surface_factors.y, 0.25F));
  assert(Near(material.surface_factors.z, 0.6F));
  assert(material.surface_factors.w == 0.0F);
  assert((material.material_class_flags &
          merlin::render::kGpuMaterialAlphaMasked) != 0U);
  assert((material.material_class_flags &
          merlin::render::kGpuMaterialDoubleSided) != 0U);
  assert(material.base_color_texture_index == 7);
  assert(material.base_color_sampler_index == 11);
  assert(material.base_color_texcoord_set == 1);

  const auto packed_draw = merlin::render::PackGpuDrawUpdate(
      snapshot, draw_plan, geometries, materials, instances);
  const auto& draw = packed_draw.ranges[0].records[0];
  assert(draw.geometry_index == geometries.Find(101)->index);
  assert(draw.material_index == materials.Find(202)->index);
  assert(draw.instance_index == instances.Find(303)->index);
  assert(draw.primitive_base == 0);
  assert(draw.primitive_count == 1);
  assert(merlin::render::GpuDrawIdentity(draw) ==
         0x1234567887654321ULL);
}

void TestStaticUpdateCopiesNothing() {
  auto snapshot = MakeSnapshot();
  GpuSceneResourceSlots geometries(GpuSceneResourceTable::Geometry, 2);
  (void)geometries.Apply(snapshot, 0, 0);
  const auto plan = geometries.Apply(snapshot, 0, 0);
  const std::vector placements{GpuGeometryPlacement{0, 0}};
  const auto packed =
      merlin::render::PackGpuGeometryUpdate(snapshot, plan, placements);
  assert(packed.ranges.empty());
  assert(packed.record_count == 0);
  assert(packed.copy_bytes == 0);
}

void TestPackingFailures() {
  auto snapshot = MakeSnapshot();
  GpuSceneResourceSlots geometries(GpuSceneResourceTable::Geometry, 2);
  GpuSceneResourceSlots materials(GpuSceneResourceTable::Material, 2);
  GpuSceneResourceSlots instances(GpuSceneResourceTable::Instance, 2);
  GpuSceneDrawSlots draws(2);
  const auto geometry_plan = geometries.Apply(snapshot, 0, 0);
  const auto material_plan = materials.Apply(snapshot, 0, 0);
  (void)instances.Apply(snapshot, 0, 0);
  const auto draw_plan = draws.Apply(snapshot, 0, 0);

  const std::vector too_large{
      GpuGeometryPlacement{std::uint64_t{1} << 32U, 0}};
  ExpectError(
      [&] {
        (void)merlin::render::PackGpuGeometryUpdate(snapshot, geometry_plan,
                                                     too_large);
      },
      GpuScenePackingErrorCode::UnrepresentableValue, "vertex offset");

  const std::vector overflowing_range{GpuGeometryPlacement{
      std::numeric_limits<std::uint32_t>::max() - 15ULL, 0}};
  ExpectError(
      [&] {
        (void)merlin::render::PackGpuGeometryUpdate(snapshot, geometry_plan,
                                                     overflowing_range);
      },
      GpuScenePackingErrorCode::UnrepresentableValue, "arena range");

  const std::vector missing_binding{GpuMaterialBinding{}};
  ExpectError(
      [&] {
        (void)merlin::render::PackGpuMaterialUpdate(snapshot, material_plan,
                                                     missing_binding);
      },
      GpuScenePackingErrorCode::MissingResidency, "texture and sampler");

  GpuSceneResourceSlots missing_materials(GpuSceneResourceTable::Material, 2);
  ExpectError(
      [&] {
        (void)merlin::render::PackGpuDrawUpdate(
            snapshot, draw_plan, geometries, missing_materials, instances);
      },
      GpuScenePackingErrorCode::MissingResidency, "resource slot tables");
}

}  // namespace

int main() {
  TestPackedRecordsAndRanges();
  TestStaticUpdateCopiesNothing();
  TestPackingFailures();
  std::cout << "GPU Scene packing tests passed\n";
  return 0;
}
