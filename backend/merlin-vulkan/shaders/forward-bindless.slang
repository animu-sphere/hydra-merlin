#include "forward-common.slang"

[[vk::binding(0, 0)]]
SamplerState bindless_samplers[];

[[vk::binding(1, 0)]]
Texture2D bindless_textures[];

[[vk::binding(0, 1)]]
ConstantBuffer<MaterialConstants> material_constants;

[shader("vertex")]
ForwardVertexOutput forward_bindless_vertex(ForwardVertexInput input)
{
    return EvaluateForwardVertex(input, material_constants);
}

[shader("fragment")]
ForwardFragmentOutput forward_bindless_fragment(ForwardVertexOutput input)
{
    uint texture_index = NonUniformResourceIndex(draw_constants.texture_index);
    uint sampler_index = NonUniformResourceIndex(
        (draw_constants.feature_mask >> kSamplerIndexShift) &
        kSamplerIndexMask);
    float4 texture_sample = bindless_textures[texture_index].Sample(
        bindless_samplers[sampler_index], input.texture_coordinate);
    return MakeForwardFragmentOutput(
        ShadeForward(input, material_constants, texture_sample));
}
