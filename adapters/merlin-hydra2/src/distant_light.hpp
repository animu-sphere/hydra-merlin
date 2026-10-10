#pragma once

#include <optional>

namespace merlin::hydra::detail {

// UsdLux distant radiance becomes the face-on unit-albedo Lambert response
// consumed by Core. Forward keeps a center-direction approximation; angular
// diameters above 180 degrees need a broader illumination model.
struct DistantLightEnergy {
  float intensity{1.0F};
  float exposure{};
  float angle_degrees{0.53F};
  float diffuse{1.0F};
  bool normalize{};
};

// Reject non-finite/negative energy, an angle outside [0, 180], or overflow.
// A zero-angle delta light uses the UsdLux size-factor-1 convention.
[[nodiscard]] std::optional<float> NormalizeDistantLightEnergy(
    const DistantLightEnergy& input);

} // namespace merlin::hydra::detail
