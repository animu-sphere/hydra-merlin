#include "forward-metal-common.slang"
[[vk::binding(1, 0)]] ConstantBuffer<DrawConstants> draw_constants : register(b1);
[[vk::binding(8, 0)]] Texture2D<float4> base_color_texture : register(t0);
[[vk::binding(9, 0)]] Texture2D<float4> normal_texture : register(t1);
[[vk::binding(10, 0)]] SamplerState base_color_sampler : register(s0);
[[vk::binding(11, 0)]] SamplerState normal_sampler : register(s1);

[shader("vertex")]
ForwardVertexOutput merlin_vertex(ForwardVertexInput input)
{
    return EvaluateMetalVertex(input, draw_constants);
}

[shader("fragment")]
ForwardFragmentOutput merlin_fragment_argument_buffer(
    ForwardVertexOutput input, bool front_facing : SV_IsFrontFace)
{
    float4 texture_sample = float4(1.0f);
    if (HasBaseColorTexture(draw_constants))
        texture_sample = resources.textures[draw_constants.texture_index].Sample(
            resources.samplers[draw_constants.sampler_index], input.texture_coordinate);
    return ShadeMetalForward(input, front_facing, draw_constants, material_constants, texture_sample,
        SampleMetalNormalTexture(input, draw_constants, material_constants));
}

[shader("fragment")]
ForwardFragmentOutput merlin_fragment_conventional(
    ForwardVertexOutput input, bool front_facing : SV_IsFrontFace)
{
    float4 texture_sample = float4(1.0f);
    if (HasBaseColorTexture(draw_constants))
        texture_sample = base_color_texture.Sample(base_color_sampler, input.texture_coordinate);
    float4 normal_sample = float4(0.5f, 0.5f, 1.0f, 1.0f);
    if (HasNormalTexture(draw_constants))
        normal_sample = normal_texture.Sample(normal_sampler, input.texture_coordinate);
    return ShadeMetalForward(input, front_facing, draw_constants, material_constants, texture_sample,
        normal_sample);
}
