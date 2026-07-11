#version 450

layout(location = 0) in vec3 position;

layout(push_constant) uniform DrawConstants {
  mat4 model_view_projection;
  vec4 base_color;
} draw_constants;

layout(location = 0) out vec4 color;

void main() {
  gl_Position = draw_constants.model_view_projection * vec4(position, 1.0);
  color = draw_constants.base_color;
}
