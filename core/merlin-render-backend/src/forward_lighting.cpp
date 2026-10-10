#include <merlin/render/forward_lighting.hpp>
#include <merlin/render/backend.hpp>

#include <cmath>

namespace merlin::render {

ForwardDirectionalLighting ExtractForwardDirectionalLighting(
    const extraction::FrameSnapshot& snapshot) {
  const extraction::LightRecord* selected = nullptr;
  for (const auto& light : snapshot.lights) {
    if (light.type == LightType::Directional &&
        (selected == nullptr || light.light < selected->light)) {
      selected = &light;
    }
  }
  ForwardDirectionalLighting result;
  if (selected == nullptr) {
    return result;
  }
  const auto nonnegative_finite = [](float value) {
    return std::isfinite(value) && value >= 0.0F;
  };
  if (!nonnegative_finite(selected->intensity) ||
      !nonnegative_finite(selected->color.x) ||
      !nonnegative_finite(selected->color.y) ||
      !nonnegative_finite(selected->color.z)) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        "extract Forward directional lighting",
        "Directional light " + std::to_string(selected->light) +
            " requires finite, nonnegative color and intensity.");
  }
  // Normalize in double precision so finite float axes at either end of the
  // range do not overflow/underflow when squared.
  const double x = selected->transform.values[8];
  const double y = selected->transform.values[9];
  const double z = selected->transform.values[10];
  const auto length = std::hypot(x, y, z);
  if (std::isfinite(length) && length > 0.0) {
    result.direction_intensity = {static_cast<float>(x / length),
        static_cast<float>(y / length), static_cast<float>(z / length),
        selected->intensity};
  } else {
    result.direction_intensity.w = selected->intensity;
  }
  result.color = selected->color;
  result.light = selected->light;
  return result;
}

} // namespace merlin::render
