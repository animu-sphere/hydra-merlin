#pragma once

#include <memory>

#include <merlin/render/backend.hpp>
#include <merlin/vulkan/renderer.hpp>

namespace merlin::vulkan {

// Optional Vulkan-native bridge capability implemented by the Vulkan backend.
// Host adapters discover it with dynamic_cast without adding native resource
// handles to the backend-neutral Core contract.
class AovImageExporter {
 public:
  virtual ~AovImageExporter() = default;
  [[nodiscard]] virtual AovImageExport AcquireAovImage(
      render::CompletionToken token, Aov aov) = 0;
  virtual void ReleaseAovImage(AovImageLease lease) = 0;
};

struct BackendFactoryOptions {
  RendererOptions renderer;
  ShaderPaths shaders;
};

// Composes the Vulkan implementation into the backend-neutral renderer
// contract. Native presentation configuration remains in RendererOptions and
// never enters Core.
class BackendFactory final : public render::BackendFactory {
 public:
  explicit BackendFactory(BackendFactoryOptions options);
  ~BackendFactory() override;

  BackendFactory(BackendFactory&&) noexcept;
  BackendFactory& operator=(BackendFactory&&) noexcept;
  BackendFactory(const BackendFactory&) = delete;
  BackendFactory& operator=(const BackendFactory&) = delete;

  [[nodiscard]] render::BackendKind kind() const noexcept override;
  [[nodiscard]] render::BackendAvailability availability() const override;
  [[nodiscard]] std::unique_ptr<render::Backend> Create(
      const render::BackendCreateInfo& info) const override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace merlin::vulkan
