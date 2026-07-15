#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec4 color;
layout(location = 1) in vec3 shading_normal;
layout(location = 2) in vec2 texture_coordinate;
layout(location = 0) out vec4 out_color;
layout(location = 1) out uint out_prim_id;
layout(location = 2) out uint out_instance_id;

layout(push_constant) uniform DrawConstants {
  mat4 model_view_projection;
  vec4 normal_matrix_column0;
  vec4 normal_matrix_column1;
  vec4 normal_matrix_column2;
  uint feature_mask;
  uint prim_id;
  uint instance_id;
  uint texture_index;
} draw_constants;

layout(set = 0, binding = 0) uniform sampler bindless_samplers[];
layout(set = 0, binding = 1) uniform texture2D bindless_textures[];
layout(std140, set = 1, binding = 0) uniform MaterialConstants {
  vec4 base_color;
  vec4 light_direction_intensity;
  vec4 light_color_alpha_cutoff;
} material_constants;

void main() {
  vec4 shaded = color;
  if ((draw_constants.feature_mask & 2u) != 0u) {
    uint sampler_index = (draw_constants.feature_mask >> 8u) & 0xffffu;
    shaded *= texture(
        sampler2D(
            bindless_textures[nonuniformEXT(draw_constants.texture_index)],
            bindless_samplers[nonuniformEXT(sampler_index)]),
        texture_coordinate);
  }
  if ((draw_constants.feature_mask & 4u) != 0u) {
    float n_dot_l = max(dot(normalize(shading_normal),
                            normalize(material_constants.light_direction_intensity.xyz)),
                        0.0);
    float light_scale = 0.15 + 0.85 * n_dot_l *
                                  material_constants.light_direction_intensity.w;
    shaded.rgb *= material_constants.light_color_alpha_cutoff.rgb * light_scale;
  }
  if ((draw_constants.feature_mask & 0x10000000u) != 0u &&
      shaded.a < material_constants.light_color_alpha_cutoff.a) {
    discard;
  }
  out_color = shaded;
  out_prim_id = draw_constants.prim_id;
  out_instance_id = draw_constants.instance_id;
}
