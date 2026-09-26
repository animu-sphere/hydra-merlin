#include "gaussian_preparation.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace {

bool Near(float lhs, float rhs, float tolerance = 1.0e-4F) {
  return std::abs(lhs - rhs) <= tolerance;
}

merlin::extraction::GaussianRecord MakeRecord(
    std::uint64_t handle, std::vector<merlin::Vec3> positions,
    std::vector<float> opacities,
    merlin::GaussianProjectionMode projection_mode =
        merlin::GaussianProjectionMode::Perspective,
    merlin::GaussianSortingMode sorting_mode =
        merlin::GaussianSortingMode::ZDepth) {
  const auto count = positions.size();
  std::vector<merlin::Covariance3> covariances(
      count, {0.0001F, 0.0F, 0.0F, 0.0001F, 0.0F, 0.0001F});
  std::vector<merlin::Vec3> coefficients(count, {1.0F, 0.0F, 0.0F});
  merlin::extraction::GaussianRecord record;
  record.gaussian = handle;
  record.positions =
      std::make_shared<const std::vector<merlin::Vec3>>(std::move(positions));
  record.covariances =
      std::make_shared<const std::vector<merlin::Covariance3>>(
          std::move(covariances));
  record.opacities =
      std::make_shared<const std::vector<float>>(std::move(opacities));
  record.spherical_harmonics_coefficients =
      std::make_shared<const std::vector<merlin::Vec3>>(
          std::move(coefficients));
  record.projection_mode = projection_mode;
  record.sorting_mode = sorting_mode;
  return record;
}

} // namespace

int main() {
  using merlin::vulkan::detail::EvaluateGaussianRadiance;
  using merlin::vulkan::detail::PrepareGaussianFrame;
  using merlin::vulkan::detail::SelectGaussianSortingPolicy;

  // Degree-zero authored radiance follows the real SH normalization and the
  // 3DGS +0.5 display bias without prematurely clamping HDR values.
  const std::vector<merlin::Vec3> degree_zero{{1.0F, 0.0F, -4.0F}};
  const auto constant = EvaluateGaussianRadiance(
      degree_zero, 0, {0.0F, 0.0F, 1.0F});
  assert(Near(constant.x, 0.7820948F));
  assert(Near(constant.y, 0.5F));
  assert(Near(constant.z, 0.0F));

  // Degree-one Z is directional and uses the OpenUSD/Graphdeco coefficient
  // order. Looking in the opposite direction reverses only that term.
  std::vector<merlin::Vec3> degree_one(4);
  degree_one[2] = {1.0F, 1.0F, 1.0F};
  const auto positive_z = EvaluateGaussianRadiance(
      degree_one, 1, {0.0F, 0.0F, 1.0F});
  const auto negative_z = EvaluateGaussianRadiance(
      degree_one, 1, {0.0F, 0.0F, -1.0F});
  assert(positive_z.x > 0.5F);
  assert(negative_z.x < 0.5F);

  merlin::extraction::FrameSnapshot snapshot;
  snapshot.gaussians.push_back(MakeRecord(
      7, {{0.0F, 0.0F, 0.2F}, {0.0F, 0.0F, 0.8F}, {0.0F, 0.0F, 0.5F}, {4.0F, 0.0F, 0.5F}},
      {1.0F, 1.0F, 0.0F, 1.0F}));
  const auto prepared = PrepareGaussianFrame(snapshot, {100, 80});
  assert(prepared.counters.candidate_count == 4);
  assert(prepared.counters.visible_count == 2);
  assert(prepared.counters.opacity_culled_count == 1);
  assert(prepared.counters.frustum_culled_count == 1);
  assert(prepared.counters.invalid_culled_count == 0);
  assert(prepared.counters.sorted_count == 2);
  assert(prepared.gaussians.size() == 2);
  // Vulkan depth increases toward the far plane, so back-to-front output is
  // descending depth with a deterministic handle/particle tie break.
  assert(prepared.gaussians[0].particle == 1);
  assert(prepared.gaussians[1].particle == 0);
  assert(prepared.gaussians[0].depth > prepared.gaussians[1].depth);
  assert(Near(prepared.gaussians[0].center_pixels.x, 50.0F));
  assert(Near(prepared.gaussians[0].center_pixels.y, 40.0F));
  assert(prepared.gaussians[0].radius_pixels > 0.0F);
  assert(prepared.gaussians[0].inverse_conic.x > 0.0F);
  assert(Near(prepared.gaussians[0].radiance.x, 0.7820948F));

  // Visibility suppresses the entire resource before touching its arrays.
  auto hidden = MakeRecord(9, {{0.0F, 0.0F, 0.5F}}, {1.0F});
  hidden.visible = false;
  snapshot.gaussians.push_back(std::move(hidden));
  const auto with_hidden = PrepareGaussianFrame(snapshot, {100, 80});
  assert(with_hidden.counters.candidate_count == 5);
  assert(with_hidden.counters.hidden_count == 1);
  assert(with_hidden.gaussians.size() == 2);

  // Camera-distance sorting remains global and back-to-front. The farther
  // off-axis particle wins even when its normalized device depth ties.
  merlin::extraction::FrameSnapshot distance_snapshot;
  distance_snapshot.gaussians.push_back(MakeRecord(
      11, {{0.0F, 0.0F, 0.5F}, {0.5F, 0.0F, 0.5F}}, {1.0F, 1.0F},
      merlin::GaussianProjectionMode::Tangential,
      merlin::GaussianSortingMode::CameraDistance));
  const auto by_distance =
      PrepareGaussianFrame(distance_snapshot, {100, 100});
  assert(by_distance.gaussians.size() == 2);
  assert(by_distance.gaussians[0].particle == 1);
  assert(by_distance.gaussians[0].sort_key >
         by_distance.gaussians[1].sort_key);

  // Tangential footprints under an orthographic projection must not acquire
  // perspective distance attenuation.
  merlin::extraction::FrameSnapshot orthographic_snapshot;
  orthographic_snapshot.gaussians.push_back(MakeRecord(
      12, {{0.0F, 0.0F, 0.2F}, {0.0F, 0.0F, 0.8F}}, {1.0F, 1.0F},
      merlin::GaussianProjectionMode::Tangential));
  const auto orthographic =
      PrepareGaussianFrame(orthographic_snapshot, {100, 100});
  assert(orthographic.gaussians.size() == 2);
  assert(Near(orthographic.gaussians[0].radius_pixels,
      orthographic.gaussians[1].radius_pixels));

  // Kernels are clipped by their center against the near plane, like Mesh
  // geometry, even when their three-sigma bound crosses into the frustum.
  // Far-plane bounds stay conservative.
  merlin::extraction::FrameSnapshot depth_boundary_snapshot;
  depth_boundary_snapshot.gaussians.push_back(MakeRecord(
      13, {{0.0F, 0.0F, -0.01F}, {0.0F, 0.0F, -0.04F},
              {0.0F, 0.0F, 1.01F}},
      {1.0F, 1.0F, 1.0F}));
  const auto depth_boundary =
      PrepareGaussianFrame(depth_boundary_snapshot, {100, 100});
  assert(depth_boundary.gaussians.size() == 1);
  assert(depth_boundary.gaussians[0].particle == 2);
  assert(depth_boundary.gaussians[0].depth == 1.0F);
  assert(depth_boundary.counters.frustum_culled_count == 2);

  // Under perspective, a kernel between the eye and the near plane would
  // project to a view-filling footprint, so it is culled rather than drawn.
  // Vulkan-depth perspective looking down -Z: unit focal length, near 0.1,
  // far 100, column-major.
  merlin::extraction::FrameSnapshot perspective_snapshot;
  constexpr float kNear = 0.1F;
  constexpr float kFar = 100.0F;
  perspective_snapshot.projection.values = {
      1.0F, 0.0F, 0.0F, 0.0F,
      0.0F, 1.0F, 0.0F, 0.0F,
      0.0F, 0.0F, kFar / (kNear - kFar), -1.0F,
      0.0F, 0.0F, kNear * kFar / (kNear - kFar), 0.0F};
  perspective_snapshot.gaussians.push_back(MakeRecord(
      14, {{0.0F, 0.0F, -0.05F}, {0.0F, 0.0F, -1.0F}}, {1.0F, 1.0F}));
  const auto perspective =
      PrepareGaussianFrame(perspective_snapshot, {100, 100});
  assert(perspective.gaussians.size() == 1);
  assert(perspective.gaussians[0].particle == 1);
  assert(perspective.counters.frustum_culled_count == 1);

  // Off-axis kernels beyond the 1.3x guard band share one clamped Jacobian,
  // so moving further off axis no longer stretches their footprint.
  auto guard_band = MakeRecord(
      15, {{2.0F, 0.0F, -1.0F}, {3.0F, 0.0F, -1.0F}}, {1.0F, 1.0F});
  guard_band.covariances =
      std::make_shared<const std::vector<merlin::Covariance3>>(
          2, merlin::Covariance3{1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F});
  merlin::extraction::FrameSnapshot guard_band_snapshot;
  guard_band_snapshot.projection = perspective_snapshot.projection;
  guard_band_snapshot.gaussians.push_back(std::move(guard_band));
  const auto guarded = PrepareGaussianFrame(guard_band_snapshot, {100, 100});
  assert(guarded.gaussians.size() == 2);
  assert(Near(guarded.gaussians[0].radius_pixels,
      guarded.gaussians[1].radius_pixels));

  // Per-resource Z-depth and camera-distance values are incomparable. Mixed
  // policy frames use one diagnosed Z-depth fallback for global composition.
  merlin::extraction::FrameSnapshot mixed_sorting_snapshot;
  mixed_sorting_snapshot.projection.values[0] = 0.1F;
  mixed_sorting_snapshot.gaussians.push_back(MakeRecord(
      21, {{0.0F, 0.0F, 0.9F}}, {1.0F},
      merlin::GaussianProjectionMode::Perspective,
      merlin::GaussianSortingMode::ZDepth));
  mixed_sorting_snapshot.gaussians.push_back(MakeRecord(
      22, {{2.0F, 0.0F, 0.2F}}, {1.0F},
      merlin::GaussianProjectionMode::Perspective,
      merlin::GaussianSortingMode::CameraDistance));
  const auto mixed_sorting =
      PrepareGaussianFrame(mixed_sorting_snapshot, {100, 100});
  assert(mixed_sorting.gaussians.size() == 2);
  assert(mixed_sorting.counters.sorting_policy_fallback_count == 2);
  assert(mixed_sorting.gaussians[0].resource == 21);
  assert(mixed_sorting.gaussians[1].resource == 22);

  // Every consumer of a frame selects its key domain through this one policy,
  // so the GPU dispatch cannot key its records against a different mode.
  const auto mixed_policy = SelectGaussianSortingPolicy(mixed_sorting_snapshot);
  assert(mixed_policy.mode == merlin::GaussianSortingMode::ZDepth);
  assert(mixed_policy.fallback_resource_count == 2);

  // A uniformly authored frame keeps its authored domain and diagnoses nothing.
  merlin::extraction::FrameSnapshot uniform_sorting_snapshot;
  uniform_sorting_snapshot.gaussians.push_back(MakeRecord(
      23, {{0.0F, 0.0F, 0.9F}}, {1.0F},
      merlin::GaussianProjectionMode::Perspective,
      merlin::GaussianSortingMode::CameraDistance));
  uniform_sorting_snapshot.gaussians.push_back(MakeRecord(
      24, {{2.0F, 0.0F, 0.2F}}, {1.0F},
      merlin::GaussianProjectionMode::Perspective,
      merlin::GaussianSortingMode::CameraDistance));
  const auto uniform_policy =
      SelectGaussianSortingPolicy(uniform_sorting_snapshot);
  assert(uniform_policy.mode == merlin::GaussianSortingMode::CameraDistance);
  assert(uniform_policy.fallback_resource_count == 0);

  // A hidden resource authors no policy: it never reaches a sorted stream.
  auto hidden_record = MakeRecord(
      25, {{0.0F, 0.0F, 0.5F}}, {1.0F},
      merlin::GaussianProjectionMode::Perspective,
      merlin::GaussianSortingMode::ZDepth);
  hidden_record.visible = false;
  auto hidden_sorting_snapshot = uniform_sorting_snapshot;
  hidden_sorting_snapshot.gaussians.push_back(std::move(hidden_record));
  const auto hidden_policy =
      SelectGaussianSortingPolicy(hidden_sorting_snapshot);
  assert(hidden_policy.mode == merlin::GaussianSortingMode::CameraDistance);
  assert(hidden_policy.fallback_resource_count == 0);

  return 0;
}
