#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <merlin/core/types.hpp>
#include <merlin/extraction/frame_snapshot.hpp>

namespace merlin::vulkan::detail {

struct GaussianPreparationOptions {
  std::uint32_t width{};
  std::uint32_t height{};
  // OpenUSD's Gaussian ellipsoid kernel uses a three-sigma extent.
  float sigma_extent{3.0F};
  // A sub-pixel low-pass floor keeps the inverse conic finite and prevents
  // point-sized kernels from disappearing between samples.
  float minimum_variance_pixels{0.25F};
};

struct PreparedGaussian {
  std::uint64_t resource{};
  std::uint32_t particle{};
  Vec2 center_pixels;
  // Inverse screen-space covariance, packed as xx, xy, yy. The raster stage
  // evaluates exp(-0.5 * dot(delta, conic * delta)).
  Vec3 inverse_conic;
  Vec3 radiance;
  float opacity{};
  float radius_pixels{};
  float depth{};
  float sort_key{};
};

struct GaussianPreparationCounters {
  std::uint64_t candidate_count{};
  std::uint64_t visible_count{};
  std::uint64_t hidden_count{};
  std::uint64_t opacity_culled_count{};
  std::uint64_t frustum_culled_count{};
  std::uint64_t invalid_culled_count{};
  std::uint64_t sorted_count{};
};

struct GaussianPreparationResult {
  std::vector<PreparedGaussian> gaussians;
  GaussianPreparationCounters counters;
};

// Evaluates the Graphdeco-compatible real spherical-harmonic basis used by
// OpenUSD Gaussian splats. Coefficients are ordered by increasing degree.
[[nodiscard]] Vec3 EvaluateGaussianRadiance(
    std::span<const Vec3> coefficients, std::uint32_t degree,
    Vec3 camera_to_particle_direction);

// Produces the deterministic CPU reference stream consumed by the MVP Vulkan
// raster path: local-to-camera transform, covariance projection, conservative
// viewport rejection, authored SH appearance, and back-to-front stable sort.
[[nodiscard]] GaussianPreparationResult PrepareGaussianFrame(
    const extraction::FrameSnapshot& snapshot,
    const GaussianPreparationOptions& options);

}  // namespace merlin::vulkan::detail
