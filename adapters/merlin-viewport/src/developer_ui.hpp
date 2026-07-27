#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <merlin/render/backend.hpp>
#include <merlin/vulkan/renderer.hpp>

namespace merlin::viewport {

class Window;

struct DeveloperUiScene {
  bool available{};
  std::uint64_t geometries{};
  std::uint64_t textures{};
  std::uint64_t samplers{};
  std::uint64_t materials{};
  std::uint64_t instances{};
  std::uint64_t lights{};
};

struct DeveloperUiSnapshot {
  std::string_view scene_source{"native"};
  const render::BackendSelection* selection{};
  const render::RendererCapabilities* capabilities{};
  render::RendererStatistics statistics;
  render::FrameTimings timings;
  render::FrameTelemetry telemetry;
  const std::vector<MaterialDiagnostic>* material_diagnostics{};
  DeveloperUiScene scene;
  std::uint64_t frame_index{};
  std::uint64_t host_frame_ns{};
  std::uint32_t width{};
  std::uint32_t height{};
};

struct DeveloperUiActions {
  bool capture_screenshot{};
  bool save_benchmark{};
};

// The viewport talks only to this small host surface. Dear ImGui and its
// platform/renderer backends stay private to developer_ui.cpp.
class DeveloperUi {
 public:
  static std::unique_ptr<DeveloperUi> Create(Window& window);
  virtual ~DeveloperUi() = default;

  virtual void ConfigurePresentation(
      vulkan::PresentationOptions& presentation) = 0;
  [[nodiscard]] virtual DeveloperUiActions DrawFrame(
      const DeveloperUiSnapshot& snapshot) = 0;
  [[nodiscard]] virtual bool WantsKeyboard() const noexcept = 0;
  [[nodiscard]] virtual bool WantsMouse() const noexcept = 0;
};

}  // namespace merlin::viewport
