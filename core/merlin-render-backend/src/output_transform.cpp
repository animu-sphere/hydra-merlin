#include <merlin/render/output_transform.hpp>
#include <merlin/render/backend.hpp>

#include <cmath>

namespace merlin::render {

Vec4 PackForwardOutputTransform(const OutputTransformSettings& settings) {
  RendererSettings contract;
  contract.exposure_ev = settings.exposure_ev;
  contract.tone_mapping = settings.tone_mapping;
  if (const auto error = ValidateRendererSettings(contract)) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        "pack Forward output transform", error->code + ": " + error->message);
  }
  return {std::exp2(settings.exposure_ev),
      settings.tone_mapping == ToneMapping::Reinhard ? 1.0F : 0.0F,
      std::exp2(-settings.exposure_ev), 0.0F};
}

Vec3 ApplyForwardOutputTransform(
    Vec3 scene_linear, const OutputTransformSettings& settings) {
  const auto packed = PackForwardOutputTransform(settings);
  const auto transform = [&](float value) {
    // Divide before exposure multiplication in Reinhard to avoid overflowing
    // otherwise finite HDR inputs at positive EV.
    return packed.y == 1.0F ? value / (packed.z + value) : value * packed.x;
  };
  return {transform(scene_linear.x), transform(scene_linear.y),
      transform(scene_linear.z)};
}

} // namespace merlin::render
