#include "distant_light.hpp"

#include <cmath>
#include <limits>
#include <numbers>

namespace merlin::hydra::detail {

std::optional<float> NormalizeDistantLightEnergy(
    const DistantLightEnergy& input) {
  if (!std::isfinite(input.intensity) || input.intensity < 0.0F ||
      !std::isfinite(input.exposure) ||
      !std::isfinite(input.diffuse) || input.diffuse < 0.0F ||
      !std::isfinite(input.angle_degrees) || input.angle_degrees < 0.0F ||
      input.angle_degrees > 180.0F) {
    return std::nullopt;
  }
  if (input.intensity == 0.0F || input.diffuse == 0.0F) {
    return 0.0F;
  }
  double projected_size = 1.0;
  if (!input.normalize && input.angle_degrees != 0.0F) {
    const auto sine = std::sin(static_cast<double>(input.angle_degrees) *
                               std::numbers::pi / 360.0);
    projected_size = std::numbers::pi * sine * sine;
  }
  // E/pi for a Lambertian surface. Log-space evaluation prevents intermediate
  // exp2(exposure) overflow when the final converted response is representable.
  const auto log_response = std::log2(static_cast<double>(input.intensity)) +
                            input.exposure +
                            std::log2(static_cast<double>(input.diffuse)) +
                            std::log2(projected_size) -
                            std::log2(std::numbers::pi);
  const auto response = std::exp2(log_response);
  if (!std::isfinite(response) ||
      response > std::numeric_limits<float>::max()) {
    return std::nullopt;
  }
  return static_cast<float>(response);
}

} // namespace merlin::hydra::detail
