#pragma once

#include <merlin/extraction/frame_snapshot.hpp>
#include <merlin/render/output_transform.hpp>

namespace merlin::render {

// The current Forward profile evaluates one directional light. Select by
// stable identity, never by the dense table slot, which can move on removal.
// With no directional input, retain the unit +Z diagnostic light.
struct ForwardDirectionalLighting {
  Vec4 direction_intensity{0.0F, 0.0F, 1.0F, 1.0F};
  Vec3 color{1.0F, 1.0F, 1.0F};
  std::uint64_t light{};
};

// Directional lights emit along local -Z; the surface-to-source vector is
// transformed +Z. Translation and camera matrices do not affect it. A zero or
// non-finite axis uses +Z. Non-finite/negative color or intensity is rejected.
[[nodiscard]] ForwardDirectionalLighting ExtractForwardDirectionalLighting(
    const extraction::FrameSnapshot& snapshot);

// Column-major draw transforms and inverse-transpose normals. Singular
// transforms retain the diagnostic identity normal basis.
[[nodiscard]] Mat4 MultiplyForwardMatrices(const Mat4& lhs, const Mat4& rhs);
[[nodiscard]] std::array<Vec4, 3> ForwardNormalMatrix(const Mat4& transform);

// Per-frame values packed into every Forward material constant block: the
// output transform and the surface policy (Ng orientation and debug view).
struct ForwardFramePolicy {
  Vec4 output_transform;
  Vec4 surface_policy{1.0F, 0.0F, 0.0F, 0.0F};
};

// Backends supply their ABI type and selected environment; policy packing is
// shared without coupling Core to either native binding layout.
template <typename Constants>
[[nodiscard]] Constants MakeForwardMaterialConstants(
    const extraction::MaterialRecord& material,
    const ForwardDirectionalLighting& lighting,
    const std::array<Vec4, 9>& diffuse_environment,
    const ForwardFramePolicy& frame) {
  Constants result{};
  result.base_color = material.parameters.base_color;
  result.light_direction_intensity = lighting.direction_intensity;
  result.light_color_alpha_cutoff = {lighting.color.x, lighting.color.y,
      lighting.color.z, material.parameters.alpha_cutoff};
  result.diffuse_environment = diffuse_environment;
  result.output_transform = frame.output_transform;
  result.surface_policy = frame.surface_policy;
  result.normal_map = {material.parameters.normal_scale,
      material.parameters.normal_flip_green ? -1.0F : 1.0F, 0.0F, 0.0F};
  return result;
}

} // namespace merlin::render
