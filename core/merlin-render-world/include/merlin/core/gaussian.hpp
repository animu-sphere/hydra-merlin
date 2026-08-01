#pragma once

#include <optional>
#include <string>
#include <vector>

#include <merlin/core/diagnostic.hpp>
#include <merlin/core/types.hpp>

namespace merlin {

// Typed source payload produced by a Hydra adapter after float/half precision
// selection. Empty optional arrays are treated as unauthored. Non-empty arrays
// follow OpenUSD's standard truncate-or-fallback length policy.
struct GaussianSourceData {
  std::string label;
  std::string source;
  std::vector<Vec3> positions;
  std::vector<Quaternion> orientations;
  std::vector<Vec3> scales;
  std::vector<float> opacities;
  std::uint32_t spherical_harmonics_degree{3};
  std::vector<Vec3> spherical_harmonics_coefficients;
  GaussianProjectionMode projection_mode{GaussianProjectionMode::Perspective};
  GaussianSortingMode sorting_mode{GaussianSortingMode::ZDepth};
};

struct GaussianNormalizationResult {
  std::optional<GaussianDescriptor> resource;
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool accepted() const noexcept { return resource.has_value(); }
};

// OpenUSD Gaussian splats carry the Graphdeco-style real SH coefficients used
// by the standard display formula (0.5 + evaluated SH). A zero DC coefficient
// therefore produces the schema's missing-radiance fallback RGB of 0.5.
inline constexpr float kSphericalHarmonicY00 = 0.28209479177387814F;
inline constexpr std::uint32_t kMaxGaussianSphericalHarmonicsDegree = 3;

[[nodiscard]] Covariance3 EvaluateGaussianCovariance(
    Quaternion orientation, Vec3 linear_scale);

[[nodiscard]] GaussianNormalizationResult NormalizeGaussianSource(
    const GaussianSourceData& source, DiagnosticSink* sink = nullptr);

}  // namespace merlin
