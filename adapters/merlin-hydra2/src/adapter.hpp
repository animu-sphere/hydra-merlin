#pragma once

#include "hgi_vulkan_bridge.hpp"
#include "hgi_metal_bridge.hpp"

#include <pxr/pxr.h>

#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/renderDelegate.h>

#include <merlin/render/backend.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace merlin::render {
class Backend;
}

PXR_NAMESPACE_OPEN_SCOPE

struct HdMerlinViewportFrame {
  merlin::render::FrameTimings timings;
  merlin::render::FrameTelemetry telemetry;
  std::vector<merlin::MaterialDiagnostic> material_diagnostics;
  std::uint64_t geometries{};
  std::uint64_t textures{};
  std::uint64_t samplers{};
  std::uint64_t materials{};
  std::uint64_t instances{};
  std::uint64_t lights{};
  HdMerlinHgiVulkanBridgeStatus hgi_vulkan_bridge;
  HdMerlinHgiVulkanBridgeTelemetry hgi_vulkan_telemetry;
  HdMerlinHgiMetalBridgeStatus hgi_metal_bridge;
  HdMerlinHgiMetalBridgeTelemetry hgi_metal_telemetry;
  bool available{};
};

// A renderer submission has one color product. It can omit CPU readback only
// when that product has exactly one RenderBuffer consumer and that consumer
// accepts the native GPU copy.
[[nodiscard]] bool HdMerlinCanUseExclusiveGpuColorCopy(
    std::size_t color_buffer_count,
    std::size_t gpu_copy_candidate_count) noexcept;

class HdMerlinRenderBuffer final : public HdRenderBuffer {
 public:
  explicit HdMerlinRenderBuffer(
      const SdfPath& id,
      std::shared_ptr<HdMerlinHgiVulkanBridge> hgi_vulkan_bridge = {},
      std::shared_ptr<HdMerlinHgiMetalBridge> hgi_metal_bridge = {});
  ~HdMerlinRenderBuffer() override;

  bool Allocate(const GfVec3i& dimensions, HdFormat format,
                bool multi_sampled) override;
  unsigned int GetWidth() const override;
  unsigned int GetHeight() const override;
  unsigned int GetDepth() const override;
  HdFormat GetFormat() const override;
  bool IsMultiSampled() const override;
  void* Map() override;
  void Unmap() override;
  bool IsMapped() const override;
  void Resolve() override;
  bool IsConverged() const override;
  VtValue GetResource(bool multi_sampled) const override;

  bool WriteColor(const std::vector<std::uint8_t>& rgba8,
                  std::uint32_t width, std::uint32_t height);
  bool WriteDepth(const std::vector<float>& depth, std::uint32_t width,
                  std::uint32_t height);
  bool WriteId(const std::vector<std::uint32_t>& ids, std::uint32_t width,
               std::uint32_t height);
  [[nodiscard]] bool CanGpuCopyColor() const;
  [[nodiscard]] bool CopyColor(
      merlin::vulkan::AovImageExport&& source,
      std::shared_ptr<merlin::render::Backend> backend);
  [[nodiscard]] bool CopyColor(
      merlin::metal::AovImageExport&& source,
      std::shared_ptr<merlin::render::Backend> backend);
  void SetConverged(bool converged);

 protected:
  void _Deallocate() override;

 private:
  void UploadHgiTargetLocked();
  void DestroyHgiTargetLocked();

  mutable std::mutex mutex_;
  std::shared_ptr<HdMerlinHgiVulkanBridge> hgi_vulkan_bridge_;
  std::shared_ptr<HdMerlinHgiMetalBridge> hgi_metal_bridge_;
  HgiTextureHandle hgi_vulkan_target_;
  HgiTextureHandle hgi_metal_target_;
  GfVec3i dimensions_{0};
  HdFormat format_{HdFormatInvalid};
  bool multi_sampled_{};
  bool gpu_only_{};
  bool converged_{};
  std::size_t map_count_{};
  std::vector<std::uint8_t> data_;
};

class HdMerlinRenderDelegate final : public HdRenderDelegate {
 public:
  explicit HdMerlinRenderDelegate(const HdRenderSettingsMap& settings = {});
  HdMerlinRenderDelegate(
      std::shared_ptr<merlin::render::Backend> backend,
      const HdRenderSettingsMap& settings = {});
  ~HdMerlinRenderDelegate() override;

  void SetDrivers(const HdDriverVector& drivers) override;
  const TfTokenVector& GetSupportedRprimTypes() const override;
  const TfTokenVector& GetSupportedSprimTypes() const override;
  const TfTokenVector& GetSupportedBprimTypes() const override;
  HdResourceRegistrySharedPtr GetResourceRegistry() const override;
  HdRenderPassSharedPtr CreateRenderPass(
      HdRenderIndex* index, const HdRprimCollection& collection) override;
  HdInstancer* CreateInstancer(HdSceneDelegate* delegate,
                               const SdfPath& id) override;
  void DestroyInstancer(HdInstancer* instancer) override;
  HdRprim* CreateRprim(const TfToken& type_id,
                       const SdfPath& rprim_id) override;
  void DestroyRprim(HdRprim* rprim) override;
  HdSprim* CreateSprim(const TfToken& type_id,
                       const SdfPath& sprim_id) override;
  HdSprim* CreateFallbackSprim(const TfToken& type_id) override;
  void DestroySprim(HdSprim* sprim) override;
  HdBprim* CreateBprim(const TfToken& type_id,
                       const SdfPath& bprim_id) override;
  HdBprim* CreateFallbackBprim(const TfToken& type_id) override;
  void DestroyBprim(HdBprim* bprim) override;
  void SetTerminalSceneIndex(
      const HdSceneIndexBaseRefPtr& terminal_scene_index) override;
  void CommitResources(HdChangeTracker* tracker) override;
  HdAovDescriptor GetDefaultAovDescriptor(const TfToken& name) const override;

  // The standalone Vulkan viewport reflects projection Y to compensate for
  // its positive-height framebuffer viewport. Other Hydra hosts keep the
  // renderer's conventional clockwise default.
  void SetCameraFrontFaceCounterClockwise(bool counter_clockwise);
  // Hydra hosts provide a projection with OpenGL's lower-left image
  // convention. Hgi targets are consumed as top-left images, so their
  // projection needs the corresponding Y reflection.
  void SetHgiProjectionYReflection(bool reflect);
  [[nodiscard]] HdMerlinViewportFrame GetLatestViewportFrame() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  HdResourceRegistrySharedPtr resources_;
};

PXR_NAMESPACE_CLOSE_SCOPE
