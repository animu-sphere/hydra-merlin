#define MERLIN_FORWARD_SCENE_SET 2
#include "forward-common.slang"

[[vk::binding(0, 0)]]
SamplerState bindless_samplers[];

[[vk::binding(1, 0)]]
Texture2D bindless_textures[];

[[vk::binding(0, 1)]]
ConstantBuffer<MaterialConstants> material_constants;

[[vk::binding(0, 2)]] Texture2D<float4> specular_environment;

[shader("vertex")]
ForwardVertexOutput forward_bindless_vertex(ForwardVertexInput input)
{
    return EvaluateForwardVertex(input, material_constants);
}

[shader("fragment")]
ForwardFragmentOutput forward_bindless_fragment(
    ForwardVertexOutput input, bool front_facing : SV_IsFrontFace)
{
    // The tables are PARTIALLY_BOUND: an untextured material leaves both
    // indices at 0, and sampler slot 0 is only written once some material
    // allocates it. Reading it unconditionally would be undefined.
    float4 texture_sample = float4(1.0f, 1.0f, 1.0f, 1.0f);
    if (HasBaseColorTexture())
    {
        uint texture_index =
            NonUniformResourceIndex(draw_constants.texture_index);
        uint sampler_index = NonUniformResourceIndex(
            (draw_constants.feature_mask >> kSamplerIndexShift) &
            kSamplerIndexMask);
        texture_sample = bindless_textures[texture_index].Sample(
            bindless_samplers[sampler_index], input.texture_coordinate);
    }
    // The material block carries the normal texture's table slots; the
    // flag guards them like the base-color indices above.
    float4 normal_sample = float4(0.5f, 0.5f, 1.0f, 1.0f);
    if (HasNormalTexture())
    {
        normal_sample = bindless_textures[NonUniformResourceIndex(
            material_constants.normal_map_resource.x)].Sample(
                bindless_samplers[NonUniformResourceIndex(
                    material_constants.normal_map_resource.y)],
                input.texture_coordinate);
    }
    float2 scalar_factors = material_constants.surface_factors.xy;
    if ((draw_constants.feature_mask & kMetallicTextureFlag) != 0u)
        scalar_factors.x = bindless_textures[NonUniformResourceIndex(
            material_constants.scalar_texture_resources.x)].Sample(
                bindless_samplers[NonUniformResourceIndex(
                    material_constants.scalar_texture_resources.y)], input.texture_coordinate).r;
    if ((draw_constants.feature_mask & kRoughnessTextureFlag) != 0u)
        scalar_factors.y = bindless_textures[NonUniformResourceIndex(
            material_constants.scalar_texture_resources.z)].Sample(
                bindless_samplers[NonUniformResourceIndex(
                    material_constants.scalar_texture_resources.w)], input.texture_coordinate).r;
    float3 emissive_sample = DefaultEmissiveSample(material_constants);
    if ((draw_constants.feature_mask & kEmissiveTextureFlag) != 0u)
        emissive_sample = bindless_textures[NonUniformResourceIndex(
            material_constants.emissive_opacity_texture_resources.x)].Sample(
                bindless_samplers[NonUniformResourceIndex(
                    material_constants.emissive_opacity_texture_resources.y)], input.texture_coordinate).rgb;
    float opacity_sample = 1.0f;
    if ((draw_constants.feature_mask & kOpacityTextureFlag) != 0u)
    {
        uint2 slots = material_constants.emissive_opacity_texture_resources.zw;
        opacity_sample = ForwardOpacity(bindless_textures[NonUniformResourceIndex(slots.x)].Sample(
            bindless_samplers[NonUniformResourceIndex(slots.y)], input.texture_coordinate).rgb);
    }
    return MakeForwardFragmentOutput(ShadeForward(input, front_facing,
        material_constants, texture_sample, normal_sample, scalar_factors,
        emissive_sample, opacity_sample, specular_environment));
}
