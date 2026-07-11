#include <merlin/core/render_product.hpp>
#include <merlin/core/render_world.hpp>

#include <cassert>
#include <stdexcept>
#include <string_view>

int main() {
  merlin::RenderWorld world;

  merlin::MeshDescriptor mesh;
  mesh.label = "triangle";
  mesh.positions = {{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F},
                    {0.0F, 1.0F, 0.0F}};
  mesh.indices = {0, 1, 2};
  const auto mesh_handle = world.CreateMesh(mesh);

  bool bad_topology_rejected = false;
  try {
    auto invalid_mesh = mesh;
    invalid_mesh.indices = {0, 1, 9};
    (void)world.CreateMesh(std::move(invalid_mesh));
  } catch (const std::invalid_argument&) {
    bad_topology_rejected = true;
  }
  assert(bad_topology_rejected);

  merlin::MaterialDescriptor material;
  material.label = "fallback";
  const auto material_handle = world.CreateMaterial(material);

  merlin::InstanceDescriptor instance;
  instance.label = "instance";
  instance.mesh = mesh_handle;
  instance.material = material_handle;
  const auto instance_handle = world.CreateInstance(instance);

  auto changes = world.Commit();
  assert(changes.revision == 1);
  assert(changes.changes.size() == 3);
  assert(world.Get(mesh_handle).label == "triangle");
  assert(world.Get(instance_handle).mesh == mesh_handle);

  instance.visible = false;
  world.UpdateInstance(instance_handle, instance);
  changes = world.Commit();
  assert(changes.revision == 2);
  assert(changes.changes.size() == 1);
  assert(changes.changes.front().change_kind == merlin::ChangeKind::Updated);
  assert(!world.Get(instance_handle).visible);

  world.Remove(instance_handle);
  changes = world.Commit();
  assert(changes.revision == 3);
  assert(changes.changes.front().change_kind == merlin::ChangeKind::Removed);
  bool stale_rejected = false;
  try {
    (void)world.Get(instance_handle);
  } catch (const std::invalid_argument&) {
    stale_rejected = true;
  }
  assert(stale_rejected);

  assert(world.Commit().empty());
  assert(world.revision() == 3);
  assert(merlin::AovName(merlin::Aov::PrimId) == std::string_view("primId"));
  const auto depth = merlin::MakeRenderProduct(64, 32, merlin::Aov::Depth);
  assert(depth.format == merlin::PixelFormat::Depth32Float);
  assert(depth.origin == merlin::ImageOrigin::TopLeft);
  assert(depth.color_space == merlin::ColorSpace::NotApplicable);

  merlin::MeshDescriptor transient_mesh = mesh;
  const auto transient = world.CreateMesh(std::move(transient_mesh));
  world.Remove(transient);
  assert(world.Commit().empty());
  assert(world.revision() == 3);
  return 0;
}
