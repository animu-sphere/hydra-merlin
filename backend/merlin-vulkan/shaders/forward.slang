#include "forward-common.slang"

[[vk::binding(0, 0)]]
Sampler2D base_color_texture;

[[vk::binding(1, 0)]]
ConstantBuffer<MaterialConstants> material_constants;

[shader("vertex")]
ForwardVertexOutput forward_vertex(ForwardVertexInput input)
{
    return EvaluateForwardVertex(input, material_constants);
}

[shader("fragment")]
ForwardFragmentOutput forward_fragment(ForwardVertexOutput input)
{
    float4 texture_sample = base_color_texture.Sample(input.texture_coordinate);
    return MakeForwardFragmentOutput(
        ShadeForward(input, material_constants, texture_sample));
}
