#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec4 display_color;
layout(location = 3) in vec2 texcoord;

layout(push_constant) uniform DrawConstants {
  mat4 model_view_projection;
  vec4 base_color;
  vec4 light_direction_intensity;
  vec4 light_color_alpha_cutoff;
  uint feature_mask;
  uint prim_id;
  uint instance_id;
  uint padding;
} draw_constants;

layout(location = 0) out vec4 color;
layout(location = 1) out vec3 shading_normal;
layout(location = 2) out vec2 texture_coordinate;

void main() {
  gl_Position = draw_constants.model_view_projection * vec4(position, 1.0);
  color = draw_constants.base_color;
  if ((draw_constants.feature_mask & 1u) != 0u) {
    color *= display_color;
  }
  shading_normal = normal;
  texture_coordinate = texcoord;
}
