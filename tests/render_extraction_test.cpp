#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>

#include <cassert>
#include <stdexcept>

int main() {
  merlin::RenderWorld world;
  merlin::MeshDescriptor mesh;
  mesh.positions = {{-0.5F, -0.5F, 0.0F}, {0.5F, -0.5F, 0.0F},
                    {0.0F, 0.5F, 0.0F}};
  mesh.indices = {0, 1, 2};
  mesh.normals = {{0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 1.0F},
                  {0.0F, 0.0F, 1.0F}};
  mesh.colors = {{1.0F, 0.0F, 0.0F, 0.5F}, {0.0F, 1.0F, 0.0F, 0.75F},
                 {0.0F, 0.0F, 1.0F, 1.0F}};
  mesh.texcoords = {{0.0F, 0.0F}, {1.0F, 0.0F}, {0.5F, 1.0F}};
  const auto mesh_handle = world.CreateMesh(mesh);
  merlin::MaterialDescriptor material;
  material.base_color = {0.25F, 0.5F, 0.75F, 1.0F};
  const auto material_handle = world.CreateMaterial(material);
  merlin::InstanceDescriptor instance;
  instance.mesh = mesh_handle;
  instance.material = material_handle;
  const auto instance_handle = world.CreateInstance(instance);
  instance.transform.values[12] = 0.25F;
  const auto second_instance_handle = world.CreateInstance(instance);

  merlin::extraction::SceneExtractor extractor;
  extractor.Apply(world, world.Commit());
  const auto first = extractor.snapshot();
  assert(first->revision == 1);

  // Two instances share one mesh: geometry is recorded once and referenced by
  // both draw records.
  assert(first->geometries.size() == 1);
  assert(first->geometries.front().mesh == mesh_handle.value());
  assert(first->geometries.front().vertices->size() == 3);
  assert(first->geometries.front().indices->size() == 3);
  assert(first->geometries.front().vertices->front().normal.z == 1.0F);
  assert(first->geometries.front().vertices->front().color.w == 0.5F);
  assert(first->geometries.front().vertices->back().texcoord.y == 1.0F);
  assert(first->materials.size() == 1);
  assert(first->instances.size() == 2);
  assert(first->draws.size() == 2);
  assert(first->draws.front().geometry_index == 0);
  assert(first->draws.back().geometry_index == 0);
  assert(first->instances[first->draws.front().instance_index].instance ==
         instance_handle.value());
  assert(first->instances[first->draws.back().instance_index].instance ==
         second_instance_handle.value());
  assert(first->instances[first->draws.back().instance_index]
             .transform.values[12] == 0.25F);
  assert(first->materials[first->draws.front().material_index]
             .base_color.z == 0.75F);

  // A transform-only edit must not rebuild geometry payloads: the new snapshot
  // shares the same immutable vertex/index arrays.
  instance.transform.values[12] = 0.5F;
  world.UpdateInstance(second_instance_handle, instance,
                       merlin::ChangeAspect::Transform);
  extractor.Apply(world, world.Commit());
  const auto transformed = extractor.snapshot();
  assert(transformed->revision == 2);
  assert(transformed->geometries.front().vertices ==
         first->geometries.front().vertices);
  assert(transformed->geometries.front().indices ==
         first->geometries.front().indices);
  assert(transformed->geometries.front().points_revision ==
         first->geometries.front().points_revision);
  assert(transformed->instances[transformed->draws.back().instance_index]
             .transform.values[12] == 0.5F);
  // The previous snapshot stays immutable across later applies.
  assert(first->instances[first->draws.back().instance_index]
             .transform.values[12] == 0.25F);

  // Points-only mesh edit refreshes the vertex payload but shares topology.
  mesh.positions[0].x = -0.75F;
  world.UpdateMesh(mesh_handle, mesh, merlin::ChangeAspect::Points);
  extractor.Apply(world, world.Commit());
  const auto moved = extractor.snapshot();
  assert(moved->geometries.front().vertices !=
         transformed->geometries.front().vertices);
  assert(moved->geometries.front().indices ==
         transformed->geometries.front().indices);
  assert(moved->geometries.front().points_revision >
         transformed->geometries.front().points_revision);
  assert(moved->geometries.front().topology_revision ==
         transformed->geometries.front().topology_revision);
  assert(moved->geometries.front().vertices->front().position.x == -0.75F);

  // Primvar-only edits replace the packed vertex payload while retaining
  // topology and position values.
  mesh.colors[0].x = 0.25F;
  world.UpdateMesh(mesh_handle, mesh, merlin::ChangeAspect::Primvars);
  extractor.Apply(world, world.Commit());
  const auto recolored = extractor.snapshot();
  assert(recolored->geometries.front().vertices !=
         moved->geometries.front().vertices);
  assert(recolored->geometries.front().indices ==
         moved->geometries.front().indices);
  assert(recolored->geometries.front().vertices->front().color.x == 0.25F);

  // Visibility-only edit drops the draw but keeps geometry and instance data.
  instance.transform.values[12] = 0.25F;
  instance.visible = false;
  world.UpdateInstance(instance_handle, instance,
                       merlin::ChangeAspect::Visibility |
                           merlin::ChangeAspect::Transform);
  extractor.Apply(world, world.Commit());
  const auto hidden = extractor.snapshot();
  assert(hidden->draws.size() == 1);
  assert(hidden->geometries.size() == 1);
  assert(hidden->instances.size() == 2);
  assert(hidden->geometries.front().vertices ==
         recolored->geometries.front().vertices);

  instance.visible = true;
  world.UpdateInstance(instance_handle, instance);
  extractor.Apply(world, world.Commit());
  assert(extractor.snapshot()->draws.size() == 2);

  // Removing the mesh retires the geometry record and every dependent draw.
  world.Remove(mesh_handle);
  extractor.Apply(world, world.Commit());
  const auto removed = extractor.snapshot();
  assert(removed->geometries.empty());
  assert(removed->draws.empty());
  assert(removed->instances.size() == 2);

  merlin::CameraDescriptor first_camera;
  first_camera.view.values[12] = 1.0F;
  const auto first_camera_handle = world.CreateCamera(first_camera);
  merlin::CameraDescriptor second_camera;
  second_camera.view.values[12] = 2.0F;
  const auto second_camera_handle = world.CreateCamera(second_camera);
  extractor.Apply(world, world.Commit());
  assert(extractor.snapshot()->view.values[12] == 0.0F);
  extractor.SetActiveCamera(second_camera_handle);
  assert(extractor.snapshot()->view.values[12] == 2.0F);
  extractor.SetActiveCamera(first_camera_handle);
  assert(extractor.snapshot()->view.values[12] == 1.0F);

  bool old_revision_rejected = false;
  try {
    extractor.Apply(world, {1, {}});
  } catch (const std::invalid_argument&) {
    old_revision_rejected = true;
  }
  assert(old_revision_rejected);
  return 0;
}
