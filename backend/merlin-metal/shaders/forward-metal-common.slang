// Native Metal upload/binding ABI; shading policy belongs to Core.
#define MERLIN_EXPLICIT_DRAW_CONSTANTS 1
#define MERLIN_SEPARATE_SAMPLER_INDEX 1
#include "../../../core/merlin-render-backend/shaders/forward-policy.slang"

struct ResourceTable
{
    Texture2D<float4> textures[128];
    SamplerState samplers[32];
};
[[vk::binding(3, 0)]] ParameterBlock<ResourceTable> resources : register(b3);
[[vk::binding(2, 0)]] ConstantBuffer<MaterialConstants> material_constants : register(b2);

[ForceInline]
ForwardVertexOutput EvaluateMetalVertex(ForwardVertexInput input, DrawConstants draw)
{
    // Metal binds material values in the fragment stage. Keep vertex color
    // interpolation independent of those values as in the reference path.
    MaterialConstants neutral = (MaterialConstants)0;
    neutral.base_color = float4(1.0f);
    return EvaluateForwardVertex(input, neutral, draw);
}

[ForceInline]
ForwardFragmentOutput ShadeMetalForward(
    ForwardVertexOutput input, bool front_facing, DrawConstants draw,
    MaterialConstants material, float4 texture_sample, float4 normal_sample)
{
    input.color *= material.base_color;
    return MakeForwardFragmentOutput(ShadeForward(input, front_facing,
        material, texture_sample, normal_sample, draw), draw);
}

// Argument-table normal texture read; the material block carries its slots.
[ForceInline]
float4 SampleMetalNormalTexture(
    ForwardVertexOutput input, DrawConstants draw, MaterialConstants material)
{
    float4 normal_sample = float4(0.5f, 0.5f, 1.0f, 1.0f);
    if (HasNormalTexture(draw))
        normal_sample = resources.textures[material.normal_map_resource.x].Sample(
            resources.samplers[material.normal_map_resource.y], input.texture_coordinate);
    return normal_sample;
}
