#pragma once

#include <memory>

#include <merlin/core/change_set.hpp>
#include <merlin/core/types.hpp>

namespace merlin {

class RenderWorld {
 public:
  RenderWorld();
  ~RenderWorld();

  RenderWorld(RenderWorld&&) noexcept;
  RenderWorld& operator=(RenderWorld&&) noexcept;
  RenderWorld(const RenderWorld&) = delete;
  RenderWorld& operator=(const RenderWorld&) = delete;

  MeshHandle CreateMesh(MeshDescriptor descriptor);
  MaterialHandle CreateMaterial(MaterialDescriptor descriptor);
  InstanceHandle CreateInstance(InstanceDescriptor descriptor);
  CameraHandle CreateCamera(CameraDescriptor descriptor);
  LightHandle CreateLight(LightDescriptor descriptor);
  RenderSettingsHandle CreateRenderSettings(
      RenderSettingsDescriptor descriptor);

  void UpdateMesh(
      MeshHandle handle, MeshDescriptor descriptor,
      ChangeAspect aspects = DefaultChangeAspects(ObjectKind::Mesh));
  void UpdateMaterial(
      MaterialHandle handle, MaterialDescriptor descriptor,
      ChangeAspect aspects = DefaultChangeAspects(ObjectKind::Material));
  void UpdateInstance(
      InstanceHandle handle, InstanceDescriptor descriptor,
      ChangeAspect aspects = DefaultChangeAspects(ObjectKind::Instance));
  void UpdateCamera(
      CameraHandle handle, CameraDescriptor descriptor,
      ChangeAspect aspects = DefaultChangeAspects(ObjectKind::Camera));
  void UpdateLight(
      LightHandle handle, LightDescriptor descriptor,
      ChangeAspect aspects = DefaultChangeAspects(ObjectKind::Light));
  void UpdateRenderSettings(
      RenderSettingsHandle handle, RenderSettingsDescriptor descriptor,
      ChangeAspect aspects =
          DefaultChangeAspects(ObjectKind::RenderSettings));

  void Remove(MeshHandle handle);
  void Remove(MaterialHandle handle);
  void Remove(InstanceHandle handle);
  void Remove(CameraHandle handle);
  void Remove(LightHandle handle);
  void Remove(RenderSettingsHandle handle);

  [[nodiscard]] const MeshDescriptor& Get(MeshHandle handle) const;
  [[nodiscard]] const MaterialDescriptor& Get(MaterialHandle handle) const;
  [[nodiscard]] const InstanceDescriptor& Get(InstanceHandle handle) const;
  [[nodiscard]] const CameraDescriptor& Get(CameraHandle handle) const;
  [[nodiscard]] const LightDescriptor& Get(LightHandle handle) const;
  [[nodiscard]] const RenderSettingsDescriptor& Get(
      RenderSettingsHandle handle) const;

  [[nodiscard]] std::uint64_t resource_revision(MeshHandle handle) const;
  [[nodiscard]] std::uint64_t resource_revision(MaterialHandle handle) const;
  [[nodiscard]] std::uint64_t resource_revision(InstanceHandle handle) const;
  [[nodiscard]] std::uint64_t resource_revision(CameraHandle handle) const;
  [[nodiscard]] std::uint64_t resource_revision(LightHandle handle) const;
  [[nodiscard]] std::uint64_t resource_revision(
      RenderSettingsHandle handle) const;

  [[nodiscard]] ChangeSet Commit();
  [[nodiscard]] std::uint64_t revision() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace merlin
