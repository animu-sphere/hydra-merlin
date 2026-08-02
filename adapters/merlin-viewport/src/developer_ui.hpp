#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <merlin/render/backend.hpp>
#ifdef MERLIN_VIEWPORT_ENABLE_VULKAN
#include <merlin/vulkan/renderer.hpp>
#endif
#ifdef MERLIN_VIEWPORT_ENABLE_METAL
#include <merlin/metal/backend.hpp>
#endif

namespace merlin::viewport {

class Window;

struct DeveloperUiScene {
  bool available{};
  std::uint64_t geometries{};
  std::uint64_t gaussians{};
  std::uint64_t textures{};
  std::uint64_t samplers{};
  std::uint64_t materials{};
  std::uint64_t instances{};
  std::uint64_t lights{};
};

struct DeveloperUiGaussian {
  bool available{};
  std::uint64_t resources{};
  std::uint64_t particles{};
  std::uint64_t visible_resources{};
  std::array<std::uint64_t, 4> spherical_harmonics_degree_resources{};
  std::uint64_t perspective_resources{};
  std::uint64_t tangential_resources{};
  std::uint64_t z_depth_resources{};
  std::uint64_t camera_distance_resources{};
};

struct DeveloperUiCamera {
  bool available{};
  bool perspective{};
  std::string_view controller;
  std::string_view up_axis;
  std::array<double, 3> position{};
  std::array<double, 3> target{};
  double yaw_degrees{};
  double pitch_degrees{};
  double distance{};
  double vertical_fov_degrees{};
  double near_plane{};
  double far_plane{};
  double aspect_ratio{};
};

struct DeveloperUiSnapshot {
  std::string_view scene_source{"native"};
  std::string_view scene_path;
  const render::BackendSelection* selection{};
  const render::RendererCapabilities* capabilities{};
  render::RendererStatistics statistics;
  render::FrameTimings timings;
  render::FrameTelemetry telemetry;
  const std::vector<MaterialDiagnostic>* material_diagnostics{};
  DeveloperUiScene scene;
  DeveloperUiGaussian gaussian;
  DeveloperUiCamera camera;
  bool can_load_usd{};
  std::uint64_t frame_index{};
  std::uint64_t host_frame_ns{};
  std::uint32_t width{};
  std::uint32_t height{};
};

struct DeveloperUiActions {
  bool capture_screenshot{};
  bool save_benchmark{};
  std::optional<std::filesystem::path> load_usd;
};

// The viewport talks only to this small host surface. Dear ImGui and its
// platform/renderer backends stay private to developer_ui.cpp.
class DeveloperUi {
 public:
  static std::unique_ptr<DeveloperUi> Create(Window& window);
  virtual ~DeveloperUi() = default;

#ifdef MERLIN_VIEWPORT_ENABLE_VULKAN
  virtual void ConfigurePresentation(
      vulkan::PresentationOptions& presentation) = 0;
#endif
#ifdef MERLIN_VIEWPORT_ENABLE_METAL
  virtual void ConfigurePresentation(
      metal::PresentationOptions& presentation) = 0;
#endif
  [[nodiscard]] virtual DeveloperUiActions DrawFrame(
      const DeveloperUiSnapshot& snapshot) = 0;
  [[nodiscard]] virtual bool WantsKeyboard() const noexcept = 0;
  [[nodiscard]] virtual bool WantsMouse() const noexcept = 0;
};

}  // namespace merlin::viewport
