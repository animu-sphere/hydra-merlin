#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <stdexcept>

int main() {
  merlin::RenderWorld world;
  merlin::MeshDescriptor mesh;
  mesh.positions = {{-0.5F, -0.5F, 0.0F}, {0.5F, -0.5F, 0.0F},
                    {0.0F, 0.5F, 0.0F}};
  mesh.indices = {0, 1, 2};
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
  assert(extractor.scene().revision == 1);
  assert(extractor.scene().vertices.size() == 6);
  assert(extractor.scene().indices.size() == 6);
  assert(extractor.scene().draws.size() == 2);
  assert(extractor.scene().draws.front().instance_handle == instance_handle.value());
  assert(extractor.scene().draws.back().instance_handle ==
         second_instance_handle.value());
  assert(extractor.scene().draws.back().transform.values[12] == 0.25F);
  assert(extractor.scene().draws.front().base_color.z == 0.75F);

  instance.visible = false;
  world.UpdateInstance(instance_handle, instance);
  extractor.Apply(world, world.Commit());
  assert(extractor.scene().revision == 2);
  assert(extractor.scene().draws.size() == 1);

  instance.visible = true;
  world.UpdateInstance(instance_handle, instance);
  extractor.Apply(world, world.Commit());
  assert(extractor.scene().draws.size() == 2);

  world.Remove(mesh_handle);
  extractor.Apply(world, world.Commit());
  assert(extractor.scene().draws.empty());

  merlin::CameraDescriptor first_camera;
  first_camera.view.values[12] = 1.0F;
  const auto first_camera_handle = world.CreateCamera(first_camera);
  merlin::CameraDescriptor second_camera;
  second_camera.view.values[12] = 2.0F;
  const auto second_camera_handle = world.CreateCamera(second_camera);
  extractor.Apply(world, world.Commit());
  assert(extractor.scene().view.values[12] == 0.0F);
  extractor.SetActiveCamera(second_camera_handle);
  assert(extractor.scene().view.values[12] == 2.0F);
  extractor.SetActiveCamera(first_camera_handle);
  assert(extractor.scene().view.values[12] == 1.0F);

  bool old_revision_rejected = false;
  try {
    extractor.Apply(world, {1, {}});
  } catch (const std::invalid_argument&) {
    old_revision_rejected = true;
  }
  assert(old_revision_rejected);
  return 0;
}
