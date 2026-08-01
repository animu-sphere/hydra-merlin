#include "gaussian_preparation.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <memory>
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

}  // namespace

int main() {
  using merlin::vulkan::detail::EvaluateGaussianRadiance;
  using merlin::vulkan::detail::PrepareGaussianFrame;

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
      7, {{0.0F, 0.0F, 0.2F}, {0.0F, 0.0F, 0.8F},
          {0.0F, 0.0F, 0.5F}, {4.0F, 0.0F, 0.5F}},
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

  return 0;
}
