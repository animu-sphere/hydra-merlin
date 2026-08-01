#include <merlin/core/gaussian.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>

namespace merlin {
namespace {

bool IsFinite(Vec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

bool IsFinite(Quaternion value) {
  return std::isfinite(value.real) && IsFinite(value.imaginary);
}

bool IsFinite(Covariance3 value) {
  return std::isfinite(value.xx) && std::isfinite(value.xy) &&
         std::isfinite(value.xz) && std::isfinite(value.yy) &&
         std::isfinite(value.yz) && std::isfinite(value.zz);
}

double LengthSquared(Quaternion value) {
  const auto real = static_cast<double>(value.real);
  const auto x = static_cast<double>(value.imaginary.x);
  const auto y = static_cast<double>(value.imaginary.y);
  const auto z = static_cast<double>(value.imaginary.z);
  return real * real + x * x + y * y + z * z;
}

Quaternion Normalize(Quaternion value) {
  const auto length_squared = LengthSquared(value);
  if (length_squared <=
      static_cast<double>(std::numeric_limits<float>::min())) {
    return {};
  }
  const auto inverse_length = 1.0 / std::sqrt(length_squared);
  value.real = static_cast<float>(static_cast<double>(value.real) *
                                  inverse_length);
  value.imaginary.x = static_cast<float>(
      static_cast<double>(value.imaginary.x) * inverse_length);
  value.imaginary.y = static_cast<float>(
      static_cast<double>(value.imaginary.y) * inverse_length);
  value.imaginary.z = static_cast<float>(
      static_cast<double>(value.imaginary.z) * inverse_length);
  return value;
}

void Report(GaussianNormalizationResult& result, DiagnosticSink* sink,
            std::string code, DiagnosticSeverity severity,
            DiagnosticDisposition disposition, const std::string& source,
            std::string message, std::string recovery) {
  Diagnostic diagnostic{kDiagnosticSchemaVersion, std::move(code), severity,
                        disposition, source, std::move(message),
                        std::move(recovery)};
  if (sink != nullptr) {
    sink->Report(diagnostic);
  }
  result.diagnostics.push_back(std::move(diagnostic));
}

template <typename Value>
bool HasCompleteArray(const std::vector<Value>& values, std::size_t count) {
  return values.size() >= count;
}

template <typename Value>
void ReportArrayPolicy(GaussianNormalizationResult& result,
                       DiagnosticSink* sink, const GaussianSourceData& source,
                       const std::vector<Value>& values, std::size_t count,
                       const char* semantic, const char* fallback) {
  if (values.size() > count) {
    Report(result, sink, std::string("gaussian.") + semantic + ".truncated",
           DiagnosticSeverity::Warning, DiagnosticDisposition::Ignored,
           source.source,
           std::string(semantic) + " array is longer than positions",
           "truncate-to-particle-count");
  } else if (values.size() < count) {
    Report(result, sink, std::string("gaussian.") + semantic + ".fallback",
           DiagnosticSeverity::Warning, DiagnosticDisposition::Fallback,
           source.source,
           std::string(semantic) + " array is shorter than positions",
           fallback);
  }
}

}  // namespace

Covariance3 EvaluateGaussianCovariance(Quaternion orientation,
                                       Vec3 linear_scale) {
  const auto q = Normalize(orientation);
  const float w = q.real;
  const float x = q.imaginary.x;
  const float y = q.imaginary.y;
  const float z = q.imaginary.z;
  const std::array<std::array<float, 3>, 3> rotation{{
      {{1.0F - 2.0F * (y * y + z * z), 2.0F * (x * y - z * w),
        2.0F * (x * z + y * w)}},
      {{2.0F * (x * y + z * w), 1.0F - 2.0F * (x * x + z * z),
        2.0F * (y * z - x * w)}},
      {{2.0F * (x * z - y * w), 2.0F * (y * z + x * w),
        1.0F - 2.0F * (x * x + y * y)}},
  }};
  const std::array<double, 3> variance{
      static_cast<double>(linear_scale.x) * linear_scale.x,
      static_cast<double>(linear_scale.y) * linear_scale.y,
      static_cast<double>(linear_scale.z) * linear_scale.z};
  const auto element = [&](std::size_t row, std::size_t column) {
    double value{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
      value += static_cast<double>(rotation[row][axis]) * variance[axis] *
               rotation[column][axis];
    }
    return static_cast<float>(value);
  };
  return {element(0, 0), element(0, 1), element(0, 2),
          element(1, 1), element(1, 2), element(2, 2)};
}

GaussianNormalizationResult NormalizeGaussianSource(
    const GaussianSourceData& source, DiagnosticSink* sink) {
  GaussianNormalizationResult result;
  const auto reject = [&](std::string code, std::string message) {
    Report(result, sink, std::move(code), DiagnosticSeverity::Error,
           DiagnosticDisposition::Rejected, source.source, std::move(message),
           "reject-particle-field");
  };

  if (std::any_of(source.positions.begin(), source.positions.end(),
                  [](Vec3 value) { return !IsFinite(value); })) {
    reject("gaussian.positions.non-finite",
           "positions contain a non-finite value");
    return result;
  }

  if (source.positions.empty()) {
    reject("gaussian.positions.empty",
           "particle field contains no positions");
    return result;
  }
  if (source.spherical_harmonics_degree >
      kMaxGaussianSphericalHarmonicsDegree) {
    reject("gaussian.radiance.unsupported-degree",
           "spherical-harmonic degree exceeds the supported MVP range");
    return result;
  }

  const auto count = source.positions.size();
  const auto degree_plus_one =
      static_cast<std::size_t>(source.spherical_harmonics_degree) + 1U;
  if (degree_plus_one >
      std::numeric_limits<std::size_t>::max() / degree_plus_one) {
    reject("gaussian.radiance.layout-overflow",
           "spherical-harmonic degree overflows its element layout");
    return result;
  }
  const auto coefficients_per_particle = degree_plus_one * degree_plus_one;
  if (count != 0U && coefficients_per_particle >
                         std::numeric_limits<std::size_t>::max() / count) {
    reject("gaussian.radiance.layout-overflow",
           "spherical-harmonic layout overflows the particle payload");
    return result;
  }
  const auto expected_coefficients = count * coefficients_per_particle;

  ReportArrayPolicy(result, sink, source, source.orientations, count,
                    "orientation", "identity-orientation");
  ReportArrayPolicy(result, sink, source, source.scales, count, "scale",
                    "unit-linear-scale");
  ReportArrayPolicy(result, sink, source, source.opacities, count, "opacity",
                    "opaque");
  ReportArrayPolicy(result, sink, source,
                    source.spherical_harmonics_coefficients,
                    expected_coefficients, "radiance", "constant-rgb-0.5");

  const bool use_orientations = HasCompleteArray(source.orientations, count);
  const bool use_scales = HasCompleteArray(source.scales, count);
  const bool use_opacities = HasCompleteArray(source.opacities, count);
  const bool use_radiance = HasCompleteArray(
      source.spherical_harmonics_coefficients, expected_coefficients);

  if (use_orientations &&
      std::any_of(source.orientations.begin(),
                  source.orientations.begin() + count,
                  [](Quaternion value) { return !IsFinite(value); })) {
    reject("gaussian.orientation.non-finite",
           "orientations contain a non-finite value");
    return result;
  }
  if (use_scales &&
      std::any_of(source.scales.begin(), source.scales.begin() + count,
                  [](Vec3 value) {
                    return !IsFinite(value) || value.x <= 0.0F ||
                           value.y <= 0.0F || value.z <= 0.0F;
                  })) {
    reject("gaussian.scale.invalid",
           "scales must be finite and strictly positive");
    return result;
  }
  if (use_opacities &&
      std::any_of(source.opacities.begin(), source.opacities.begin() + count,
                  [](float value) { return !std::isfinite(value); })) {
    reject("gaussian.opacity.non-finite",
           "opacities contain a non-finite value");
    return result;
  }
  if (use_radiance && std::any_of(
                          source.spherical_harmonics_coefficients.begin(),
                          source.spherical_harmonics_coefficients.begin() +
                              expected_coefficients,
                          [](Vec3 value) { return !IsFinite(value); })) {
    reject("gaussian.radiance.non-finite",
           "spherical-harmonic coefficients contain a non-finite value");
    return result;
  }

  if (use_orientations &&
      std::any_of(source.orientations.begin(),
                  source.orientations.begin() + count,
                  [](Quaternion value) {
                    return LengthSquared(value) <=
                           static_cast<double>(
                               std::numeric_limits<float>::min());
                  })) {
    Report(result, sink, "gaussian.orientation.zero-length",
           DiagnosticSeverity::Warning, DiagnosticDisposition::Fallback,
           source.source,
           "zero-length orientations use the identity quaternion",
           "identity-orientation");
  }
  if (use_opacities &&
      std::any_of(source.opacities.begin(), source.opacities.begin() + count,
                  [](float value) { return value < 0.0F || value > 1.0F; })) {
    Report(result, sink, "gaussian.opacity.clamped",
           DiagnosticSeverity::Warning, DiagnosticDisposition::Fallback,
           source.source, "opacities outside [0, 1] were clamped",
           "clamp-linear-opacity");
  }

  GaussianDescriptor normalized;
  normalized.label = source.label;
  normalized.positions = source.positions;
  normalized.projection_mode = source.projection_mode;
  normalized.sorting_mode = source.sorting_mode;
  normalized.covariances.reserve(count);
  normalized.opacities.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto orientation =
        use_orientations ? source.orientations[index] : Quaternion{};
    const auto scale = use_scales ? source.scales[index]
                                  : Vec3{1.0F, 1.0F, 1.0F};
    const auto covariance = EvaluateGaussianCovariance(orientation, scale);
    if (!IsFinite(covariance)) {
      reject("gaussian.covariance.non-finite",
             "orientation and scale produce a non-finite covariance");
      return result;
    }
    normalized.covariances.push_back(covariance);
    normalized.opacities.push_back(
        use_opacities ? std::clamp(source.opacities[index], 0.0F, 1.0F)
                      : 1.0F);
  }

  if (use_radiance) {
    normalized.spherical_harmonics_degree =
        source.spherical_harmonics_degree;
    normalized.spherical_harmonics_coefficients.assign(
        source.spherical_harmonics_coefficients.begin(),
        source.spherical_harmonics_coefficients.begin() +
            expected_coefficients);
  } else {
    normalized.spherical_harmonics_degree = 0;
    normalized.spherical_harmonics_coefficients.assign(
        count, Vec3{});
  }
  result.resource = std::move(normalized);
  return result;
}

}  // namespace merlin
