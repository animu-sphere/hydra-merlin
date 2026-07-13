#include <merlin/core/render_product.hpp>
#include <merlin/core/render_world.hpp>

#include <algorithm>
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

  bool bad_primvar_rejected = false;
  try {
    auto invalid_mesh = mesh;
    invalid_mesh.colors = {{1.0F, 1.0F, 1.0F, 1.0F}};
    (void)world.CreateMesh(std::move(invalid_mesh));
  } catch (const std::invalid_argument&) {
    bad_primvar_rejected = true;
  }
  assert(bad_primvar_rejected);

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
  const auto mesh_created = std::find_if(
      changes.changes.begin(), changes.changes.end(), [](const auto& change) {
        return change.object_kind == merlin::ObjectKind::Mesh;
      });
  assert(mesh_created != changes.changes.end());
  assert(mesh_created->resource_revision == 1);
  assert(mesh_created->HasAspect(merlin::ChangeAspect::Topology));
  assert(mesh_created->HasAspect(merlin::ChangeAspect::Points));
  assert(mesh_created->HasAspect(merlin::ChangeAspect::Primvars));
  assert(world.resource_revision(mesh_handle) == 1);
  assert(world.resource_revision(instance_handle) == 1);
  assert(world.Get(mesh_handle).label == "triangle");
  assert(world.Get(instance_handle).mesh == mesh_handle);

  instance.visible = false;
  world.UpdateInstance(instance_handle, instance,
                       merlin::ChangeAspect::Visibility);
  changes = world.Commit();
  assert(changes.revision == 2);
  assert(changes.changes.size() == 1);
  assert(changes.changes.front().change_kind == merlin::ChangeKind::Updated);
  assert(changes.changes.front().aspects ==
         merlin::ChangeAspect::Visibility);
  assert(changes.changes.front().resource_revision == 2);
  assert(world.resource_revision(instance_handle) == 2);
  assert(!world.Get(instance_handle).visible);

  instance.transform.values[12] = 0.25F;
  world.UpdateInstance(instance_handle, instance,
                       merlin::ChangeAspect::Transform);
  instance.visible = true;
  world.UpdateInstance(instance_handle, instance,
                       merlin::ChangeAspect::Visibility);
  changes = world.Commit();
  assert(changes.revision == 3);
  assert(changes.changes.size() == 1);
  assert(changes.changes.front().HasAspect(
      merlin::ChangeAspect::Transform));
  assert(changes.changes.front().HasAspect(
      merlin::ChangeAspect::Visibility));
  assert(changes.changes.front().resource_revision == 4);

  world.Remove(instance_handle);
  changes = world.Commit();
  assert(changes.revision == 4);
  assert(changes.changes.front().change_kind == merlin::ChangeKind::Removed);
  assert(changes.changes.front().resource_revision == 5);
  bool stale_rejected = false;
  try {
    (void)world.Get(instance_handle);
  } catch (const std::invalid_argument&) {
    stale_rejected = true;
  }
  assert(stale_rejected);

  bool mismatched_aspect_rejected = false;
  try {
    world.UpdateMesh(mesh_handle, mesh, merlin::ChangeAspect::Transform);
  } catch (const std::invalid_argument&) {
    mismatched_aspect_rejected = true;
  }
  assert(mismatched_aspect_rejected);
  assert(world.resource_revision(mesh_handle) == 1);

  assert(world.Commit().empty());
  assert(world.revision() == 4);
  assert(merlin::AovName(merlin::Aov::PrimId) == std::string_view("primId"));
  const auto depth = merlin::MakeRenderProduct(64, 32, merlin::Aov::Depth);
  assert(depth.format == merlin::PixelFormat::Depth32Float);
  assert(depth.origin == merlin::ImageOrigin::TopLeft);
  assert(depth.color_space == merlin::ColorSpace::NotApplicable);
  assert(merlin::BytesPerPixel(depth.format) == 4);
  assert(merlin::TightRowPitchBytes(depth) == 256);
  assert(merlin::IsCanonicalRenderProduct(depth));
  auto invalid_depth = depth;
  invalid_depth.origin = merlin::ImageOrigin::BottomLeft;
  assert(!merlin::IsCanonicalRenderProduct(invalid_depth));

  merlin::MeshDescriptor transient_mesh = mesh;
  const auto transient = world.CreateMesh(std::move(transient_mesh));
  world.Remove(transient);
  assert(world.Commit().empty());
  assert(world.revision() == 4);

  merlin::RenderWorld settings_world;
  merlin::RenderSettingsDescriptor settings;
  settings.label = "viewport";
  const auto settings_handle =
      settings_world.CreateRenderSettings(settings);
  auto settings_changes = settings_world.Commit();
  assert(settings_changes.changes.size() == 1);
  assert(settings_changes.changes.front().object_kind ==
         merlin::ObjectKind::RenderSettings);
  assert(settings_changes.changes.front().HasAspect(
      merlin::ChangeAspect::RenderSettings));
  assert(settings_changes.changes.front().resource_revision == 1);
  settings.width = 1280;
  settings.height = 720;
  settings_world.UpdateRenderSettings(settings_handle, settings);
  settings_changes = settings_world.Commit();
  assert(settings_changes.changes.front().resource_revision == 2);
  assert(settings_world.resource_revision(settings_handle) == 2);

  merlin::RenderWorld classification_world;
  const auto classification_mesh =
      classification_world.CreateMesh(mesh);
  merlin::MaterialDescriptor first_material;
  const auto first_material_handle =
      classification_world.CreateMaterial(first_material);
  merlin::MaterialDescriptor second_material;
  const auto second_material_handle =
      classification_world.CreateMaterial(second_material);
  merlin::InstanceDescriptor classification_instance;
  classification_instance.mesh = classification_mesh;
  classification_instance.material = first_material_handle;
  const auto classification_instance_handle =
      classification_world.CreateInstance(classification_instance);
  (void)classification_world.Commit();

  first_material.parameters.roughness = 0.25F;
  classification_world.UpdateMaterial(
      first_material_handle, first_material,
      merlin::ChangeAspect::MaterialParameters);
  classification_instance.material = second_material_handle;
  classification_world.UpdateInstance(
      classification_instance_handle, classification_instance,
      merlin::ChangeAspect::MaterialBinding);
  const auto classification_changes = classification_world.Commit();
  assert(classification_changes.changes.size() == 2);
  const auto material_change = std::find_if(
      classification_changes.changes.begin(),
      classification_changes.changes.end(), [](const auto& change) {
        return change.object_kind == merlin::ObjectKind::Material;
      });
  const auto binding_change = std::find_if(
      classification_changes.changes.begin(),
      classification_changes.changes.end(), [](const auto& change) {
        return change.object_kind == merlin::ObjectKind::Instance;
      });
  assert(material_change != classification_changes.changes.end());
  assert(material_change->aspects ==
         merlin::ChangeAspect::MaterialParameters);
  assert(material_change->resource_revision == 2);
  assert(binding_change != classification_changes.changes.end());
  assert(binding_change->aspects == merlin::ChangeAspect::MaterialBinding);
  assert(binding_change->resource_revision == 2);

  merlin::RenderWorld material_world;
  merlin::TextureDescriptor texture;
  texture.width = 2;
  texture.height = 1;
  texture.pixels = {255, 0, 0, 255, 0, 255, 0, 255};
  const auto texture_handle = material_world.CreateTexture(texture);
  merlin::SamplerDescriptor sampler;
  const auto sampler_handle = material_world.CreateSampler(sampler);
  merlin::MaterialDescriptor textured_material;
  textured_material.features |= merlin::MaterialFeature::BaseColorTexture;
  textured_material.base_color_texture =
      merlin::TextureBinding{texture_handle, sampler_handle, 0};
  const auto textured_material_handle =
      material_world.CreateMaterial(textured_material);
  const auto resource_changes = material_world.Commit();
  assert(resource_changes.changes.size() == 3);
  assert(material_world.resource_revision(texture_handle) == 1);
  assert(material_world.resource_revision(sampler_handle) == 1);
  assert(material_world.Get(textured_material_handle).base_color_texture->texture ==
         texture_handle);

  texture.pixels[0] = 128;
  material_world.UpdateTexture(texture_handle, texture);
  sampler.mag_filter = merlin::FilterMode::Nearest;
  material_world.UpdateSampler(sampler_handle, sampler);
  const auto resource_updates = material_world.Commit();
  assert(resource_updates.changes.size() == 2);
  assert(material_world.resource_revision(texture_handle) == 2);
  assert(material_world.resource_revision(sampler_handle) == 2);

  bool bad_texture_rejected = false;
  try {
    merlin::TextureDescriptor invalid_texture;
    invalid_texture.width = 2;
    invalid_texture.height = 2;
    invalid_texture.pixels.resize(3);
    (void)material_world.CreateTexture(std::move(invalid_texture));
  } catch (const std::invalid_argument&) {
    bad_texture_rejected = true;
  }
  assert(bad_texture_rejected);

  bool bad_material_rejected = false;
  try {
    auto invalid_material = textured_material;
    invalid_material.parameters.alpha_cutoff = 2.0F;
    (void)material_world.CreateMaterial(std::move(invalid_material));
  } catch (const std::invalid_argument&) {
    bad_material_rejected = true;
  }
  assert(bad_material_rejected);

  bool missing_binding_rejected = false;
  try {
    merlin::MaterialDescriptor invalid_material;
    invalid_material.features |=
        merlin::MaterialFeature::BaseColorTexture;
    (void)material_world.CreateMaterial(std::move(invalid_material));
  } catch (const std::invalid_argument&) {
    missing_binding_rejected = true;
  }
  assert(missing_binding_rejected);

  bool unsupported_texcoord_rejected = false;
  try {
    auto invalid_material = textured_material;
    invalid_material.base_color_texture->texcoord_set = 1;
    (void)material_world.CreateMaterial(std::move(invalid_material));
  } catch (const std::invalid_argument&) {
    unsupported_texcoord_rejected = true;
  }
  assert(unsupported_texcoord_rejected);
  return 0;
}
