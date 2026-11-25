#include "radiance-environment.slang"

struct DomeConstants
{
    float4x4 clip_to_environment;
    float4 radiance_scale;
};

struct DomeVertex
{
    float4 position : SV_Position;
    float2 clip_xy : TEXCOORD0;
};

struct DomeOutput
{
    float4 color : SV_Target0;
    uint prim_id : SV_Target1;
    uint instance_id : SV_Target2;
};

DomeVertex MakeDomeVertex(uint id)
{
    DomeVertex result;
    result.clip_xy = float2(float((id << 1) & 2u), float(id & 2u)) * 2.0f - 1.0f;
    result.position = float4(result.clip_xy, 1.0f, 1.0f);
    return result;
}

DomeOutput ShadeDome(DomeVertex input, DomeConstants constants,
    Texture2D<float4> environment)
{
    float3 direction = normalize(mul(constants.clip_to_environment,
        float4(input.clip_xy, 1.0f, 1.0f)).xyz);
    uint width, height;
    environment.GetDimensions(width, height);
    float3 radiance = SampleRadiance(environment, RadianceUv(direction), int2(width, height), 0);
    DomeOutput result;
    result.color = float4(saturate(radiance * constants.radiance_scale.rgb), 1.0f);
    result.prim_id = 0xffffffffu;
    result.instance_id = 0xffffffffu;
    return result;
}
