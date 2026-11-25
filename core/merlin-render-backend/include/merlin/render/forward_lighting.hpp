#pragma once

#include <merlin/extraction/frame_snapshot.hpp>
#include <merlin/render/output_transform.hpp>

namespace merlin::render {

struct ForwardLightingSettings;
struct ForwardDomeLight;
inline constexpr std::size_t kForwardAdditionalLightCount = 7;

struct alignas(16) DomeDrawConstants {
  Mat4 clip_to_environment;
  Vec4 radiance_scale;
};
static_assert(sizeof(DomeDrawConstants) == 80);

[[nodiscard]] DomeDrawConstants PackDomeDrawConstants(const ForwardDomeLight& dome);

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

struct ForwardLighting {
  ForwardDirectionalLighting primary;
  std::array<Vec4, kForwardAdditionalLightCount> additional_directions{};
  std::array<Vec4, kForwardAdditionalLightCount> additional_colors{};
  std::array<Vec4, 9> environment{};
  Mat4 world_to_environment;
  std::array<Vec4, 3> clip_to_view_direction{};
  Vec4 policy{0.15F, 0.85F, 0.0F, 0.0F};
  TextureHandle specular_texture;
  Vec4 specular_environment_scale;
};

[[nodiscard]] ForwardLighting PrepareForwardLighting(
    const extraction::FrameSnapshot& snapshot,
    const std::array<Vec4, 9>& default_environment,
    const ForwardLightingSettings& settings);

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
    const ForwardLighting& lighting,
    const ForwardFramePolicy& frame) {
  Constants result{};
  result.base_color = material.parameters.base_color;
  result.light_direction_intensity = lighting.primary.direction_intensity;
  result.light_color_alpha_cutoff = {lighting.primary.color.x,
      lighting.primary.color.y, lighting.primary.color.z,
      material.parameters.alpha_cutoff};
  result.diffuse_environment = lighting.environment;
  result.output_transform = frame.output_transform;
  result.surface_policy = frame.surface_policy;
  result.normal_map = {material.parameters.normal_scale,
      material.parameters.normal_flip_green ? -1.0F : 1.0F, 0.0F, 0.0F};
  result.light_policy = lighting.policy;
  result.additional_light_directions = lighting.additional_directions;
  result.additional_light_colors = lighting.additional_colors;
  result.world_to_environment = lighting.world_to_environment;
  result.surface_factors = {material.parameters.metallic,
      material.parameters.roughness, 0.0F, 0.0F};
  result.clip_to_view_direction = lighting.clip_to_view_direction;
  result.specular_environment_scale = lighting.specular_environment_scale;
  return result;
}

} // namespace merlin::render
