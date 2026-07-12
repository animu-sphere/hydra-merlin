#version 450

layout(location = 0) in vec4 color;
layout(location = 0) out vec4 out_color;
layout(location = 1) out uint out_prim_id;
layout(location = 2) out uint out_instance_id;

layout(push_constant) uniform DrawConstants {
  mat4 model_view_projection;
  vec4 base_color;
  uint prim_id;
  uint instance_id;
} draw_constants;

void main() {
  out_color = color;
  out_prim_id = draw_constants.prim_id;
  out_instance_id = draw_constants.instance_id;
}
