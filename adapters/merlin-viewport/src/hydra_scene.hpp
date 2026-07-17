#pragma once

#include <cstdint>
#include <filesystem>

#include <merlin/render/backend.hpp>

namespace merlin::viewport {

struct HydraViewportOptions {
  std::filesystem::path stage;
  std::filesystem::path executable;
  std::filesystem::path screenshot;
  std::filesystem::path benchmark;
  std::uint32_t width{1280};
  std::uint32_t height{720};
  std::uint64_t frame_limit{};
  render::BackendRequest backend{render::BackendRequest::Automatic};
  bool validation{};
  bool vsync{true};
  bool visible{true};
  bool reference_check{};
  bool resize_test{};
};

int RunHydraViewport(const HydraViewportOptions& options);

}  // namespace merlin::viewport
