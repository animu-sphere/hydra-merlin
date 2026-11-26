#include "forward-metal-common.slang"
[[vk::binding(1, 0)]] ConstantBuffer<DrawConstants> draw_constants : register(b1);
[[vk::binding(8, 0)]] Texture2D<float4> base_color_texture : register(t0);
[[vk::binding(9, 0)]] Texture2D<float4> normal_texture : register(t1);
[[vk::binding(10, 0)]] SamplerState base_color_sampler : register(s0);
[[vk::binding(11, 0)]] SamplerState normal_sampler : register(s1);
[[vk::binding(12, 0)]] Texture2D<float4> metallic_texture : register(t2);
[[vk::binding(13, 0)]] Texture2D<float4> roughness_texture : register(t3);
[[vk::binding(14, 0)]] SamplerState metallic_sampler : register(s2);
[[vk::binding(15, 0)]] SamplerState roughness_sampler : register(s3);
[[vk::binding(17, 0)]] Texture2D<float4> emissive_texture : register(t4);
[[vk::binding(18, 0)]] SamplerState emissive_sampler : register(s4);

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
        SampleMetalNormalTexture(input, draw_constants, material_constants),
        SampleMetalScalarTextures(input, draw_constants, material_constants),
        SampleMetalEmissiveTexture(input, draw_constants, material_constants));
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
    float2 scalar_factors = material_constants.surface_factors.xy;
    if ((draw_constants.feature_mask & kMetallicTextureFlag) != 0u)
        scalar_factors.x = metallic_texture.Sample(metallic_sampler, input.texture_coordinate).r;
    if ((draw_constants.feature_mask & kRoughnessTextureFlag) != 0u)
        scalar_factors.y = roughness_texture.Sample(roughness_sampler, input.texture_coordinate).r;
    float3 emissive_sample = DefaultEmissiveSample(material_constants);
    if ((draw_constants.feature_mask & kEmissiveTextureFlag) != 0u)
        emissive_sample = emissive_texture.Sample(emissive_sampler, input.texture_coordinate).rgb;
    return ShadeMetalForward(input, front_facing, draw_constants, material_constants, texture_sample,
        normal_sample, scalar_factors, emissive_sample);
}
