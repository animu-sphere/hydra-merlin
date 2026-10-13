#pragma once

#include <merlin/core/types.hpp>

namespace merlin::render {

enum class ToneMapping { None,
  Reinhard,
  Aces };

// Forward scene-linear RGB -> 2^EV exposure -> tone mapping -> linear Color
// AOV. Encoding (e.g. sRGB) belongs to presentation. Alpha is not transformed.
// Applies to lit Forward surfaces; diagnostic colors and Gaussian keep their
// existing output policy. ACES is reserved and rejected.
struct OutputTransformSettings {
  float exposure_ev{};
  ToneMapping tone_mapping{ToneMapping::Reinhard};

  friend constexpr bool operator==(const OutputTransformSettings&,
      const OutputTransformSettings&) = default;
};

// Validates finite EV in [-32, 32] and None/Reinhard before GPU work.
// x: exposure multiplier, y: 0=None/1=Reinhard, z: inverse exposure, w: reserved.
[[nodiscard]] Vec4 PackForwardOutputTransform(
    const OutputTransformSettings& settings);

// CPU reference; does not quantize/clamp to an attachment or encode for display.
[[nodiscard]] Vec3 ApplyForwardOutputTransform(
    Vec3 scene_linear, const OutputTransformSettings& settings);

} // namespace merlin::render
