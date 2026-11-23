#pragma once

#include <merlin/core/gaussian.hpp>
#include <merlin/extraction/frame_snapshot.hpp>

#include <cstdint>
#include <memory>

namespace merlin::test {

inline constexpr std::uint32_t kAdaptiveWidth = 96;
inline constexpr std::uint32_t kAdaptiveHeight = 80;
inline constexpr int kAdaptiveCameraSteps = 8;

inline GaussianDescriptor AdaptiveGaussianFixture() {
  GaussianDescriptor gaussian;
  gaussian.label = "adaptive-tile-bounds";
  gaussian.positions = {{0.17708333F, 0.0125F, 0.45F},
      {-0.37F, 0.34F, 0.6F}, {0.62F, -0.46F, 0.52F},
      {-0.54F, -0.2F, 0.2F}, {0.17708333F, 0.0125F, 0.1F},
      {0.92F, 0.64F, 0.35F}};
  gaussian.covariances = {{0.04F, 0.0045F, 0.0012F, 0.0011F, 0, 0.002F},
      {0.004F, -0.006F, 0, 0.05F, 0, 0.002F},
      {0.02F, 0, 0, 0.02F, 0, 0.002F},
      {0.03F, 0, 0, 0.03F, 0, 0.002F},
      {0.024F, 0, 0, 0.012F, 0, 0.002F},
      {0.07F, 0.016F, 0, 0.011F, 0, 0.002F}};
  gaussian.opacities = {0.023F, 0.8F, 0.01F, 0.003F, 1.0F / 255.0F, 1.0F};
  gaussian.spherical_harmonics_coefficients = {{1, -1, 0}, {-1, 1, 0},
      {0, 0, 1}, {1, 1, 0}, {1, -1, 1}, {0, 1, 1}};
  return gaussian;
}

inline std::shared_ptr<const extraction::FrameSnapshot> AdaptiveCameraFrame(
    const extraction::FrameSnapshot& snapshot, int step) {
  auto frame = std::make_shared<extraction::FrameSnapshot>(snapshot);
  frame->view.values[12] = static_cast<float>(step) / 96.0F;
  frame->view.values[13] = -static_cast<float>(step) / 128.0F;
  return frame;
}

} // namespace merlin::test
