#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec4 display_color;
layout(location = 3) in vec2 texcoord;

layout(push_constant) uniform DrawConstants {
  mat4 model_view_projection;
  vec4 base_color;
  uint prim_id;
  uint instance_id;
} draw_constants;

layout(location = 0) out vec4 color;

void main() {
  gl_Position = draw_constants.model_view_projection * vec4(position, 1.0);
  color = draw_constants.base_color * display_color;
}
