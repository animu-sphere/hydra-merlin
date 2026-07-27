#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include <merlin/metal/resource_table.hpp>
#include <merlin/render/backend.hpp>

namespace merlin::metal {

enum class PresentationColorSpace {
  Srgb,
  DisplayP3,
};

// SDR is the v1 presentation contract. The explicit dynamic-range policy keeps
// future HDR support additive instead of inferring it from a layer format.
enum class PresentationDynamicRange {
  Standard,
  Extended,
};

enum class PresentationOverlayPhase {
  Initialize,
  Render,
  Shutdown,
};

struct PresentationOverlayContext {
  PresentationOverlayPhase phase{PresentationOverlayPhase::Initialize};
  std::uintptr_t device{};
  std::uintptr_t command_buffer{};
  std::uintptr_t render_encoder{};
  std::uintptr_t render_pass_descriptor{};
};

struct PresentationOptions {
  using RenderOverlay = void (*)(void *, const PresentationOverlayContext &);

  // Encoded CAMetalLayer*. Native Objective-C types remain private to the
  // Apple presentation adapter and Metal implementation.
  std::uintptr_t layer{};
  bool vsync{true};
  PresentationColorSpace color_space{PresentationColorSpace::Srgb};
  PresentationDynamicRange dynamic_range{
      PresentationDynamicRange::Standard};
  std::uint32_t drawable_count{3};
  void *overlay_user_data{};
  RenderOverlay render_overlay{};
};

struct BackendOptions {
  std::uint32_t texture_capacity{128};
  std::uint32_t sampler_capacity{32};
  std::uint64_t heap_capacity_bytes{64ULL * 1024ULL * 1024ULL};
  std::optional<PresentationOptions> presentation;
};

struct MetalStatistics {
  std::uint32_t frame_context_count{};
  std::uint64_t heap_capacity_bytes{};
  std::uint64_t heap_resident_bytes{};
  std::uint64_t heap_peak_resident_bytes{};
  std::uint64_t heap_allocation_count{};
  std::uint64_t heap_release_count{};
  std::uint64_t heap_exhaustion_count{};
  std::uint64_t argument_buffer_update_count{};
  std::uint64_t argument_buffer_encode_count{};
  std::uint64_t scene_resource_retirements{};
  ResourceTableTelemetry texture_slots;
  ResourceTableTelemetry sampler_slots;
};

class Backend final : public render::Backend {
public:
  Backend(const render::BackendCreateInfo &info, BackendOptions options);
  ~Backend() override;

  Backend(Backend &&) noexcept;
  Backend &operator=(Backend &&) noexcept;
  Backend(const Backend &) = delete;
  Backend &operator=(const Backend &) = delete;

  [[nodiscard]] const render::RendererCapabilities &
  capabilities() const noexcept override;
  [[nodiscard]] render::RendererStatistics statistics() const noexcept override;
  [[nodiscard]] MetalStatistics metal_statistics() const noexcept;
  [[nodiscard]] std::optional<render::PresentationTarget>
  default_presentation_target() const noexcept override;
  void ResizePresentationTarget(render::PresentationTarget target,
                                std::uint32_t width,
                                std::uint32_t height) override;
  [[nodiscard]] render::CompletionToken
  Submit(const render::RenderRequest &request) override;
  [[nodiscard]] bool IsComplete(render::CompletionToken token) const override;
  [[nodiscard]] render::RenderResult
  Resolve(render::CompletionToken token,
          std::chrono::nanoseconds timeout =
              std::chrono::nanoseconds::max()) override;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

class BackendFactory final : public render::BackendFactory {
public:
  explicit BackendFactory(BackendOptions options = {});

  [[nodiscard]] render::BackendKind kind() const noexcept override;
  [[nodiscard]] render::BackendAvailability availability() const override;
  [[nodiscard]] std::unique_ptr<render::Backend>
  Create(const render::BackendCreateInfo &info) const override;

private:
  BackendOptions options_;
};

} // namespace merlin::metal
