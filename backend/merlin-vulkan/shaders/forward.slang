#include "forward-common.slang"

[[vk::binding(0, 0)]]
Sampler2D base_color_texture;

[[vk::binding(1, 0)]]
Sampler2D normal_texture;

// Bindings below this are reserved for generated material resources. The
// conventional and generated Forward variants share this vertex ABI.
[[vk::binding(31, 0)]]
ConstantBuffer<MaterialConstants> material_constants;

[shader("vertex")]
ForwardVertexOutput forward_vertex(ForwardVertexInput input)
{
    return EvaluateForwardVertex(input, material_constants);
}

[shader("fragment")]
ForwardFragmentOutput forward_fragment(
    ForwardVertexOutput input, bool front_facing : SV_IsFrontFace)
{
    float4 texture_sample = float4(1.0f, 1.0f, 1.0f, 1.0f);
    if (HasBaseColorTexture())
    {
        texture_sample = base_color_texture.Sample(input.texture_coordinate);
    }
    float4 normal_sample = float4(0.5f, 0.5f, 1.0f, 1.0f);
    if (HasNormalTexture())
    {
        normal_sample = normal_texture.Sample(input.texture_coordinate);
    }
    return MakeForwardFragmentOutput(ShadeForward(input, front_facing,
        material_constants, texture_sample, normal_sample));
}
