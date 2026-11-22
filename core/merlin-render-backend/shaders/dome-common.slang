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

float3 LoadRadiance(Texture2D<float4> environment, int2 texel, int2 size)
{
    texel.x = (texel.x + size.x) % size.x;
    texel.y = clamp(texel.y, 0, size.y - 1);
    float4 rgbe = round(environment.Load(int3(texel, 0)) * 255.0f);
    return rgbe.a == 0.0f ? float3(0.0f) : rgbe.rgb * exp2(rgbe.a - 136.0f);
}

DomeOutput ShadeDome(DomeVertex input, DomeConstants constants,
    Texture2D<float4> environment)
{
    float3 direction = normalize(mul(constants.clip_to_environment,
        float4(input.clip_xy, 1.0f, 1.0f)).xyz);
    float2 uv = float2(frac(atan2(direction.z, direction.x) / 6.2831853072f),
        acos(clamp(direction.y, -1.0f, 1.0f)) / 3.1415926536f);
    uint width, height;
    environment.GetDimensions(width, height);
    int2 size = int2(width, height);
    float2 pixel = uv * float2(size) - 0.5f;
    int2 texel = int2(floor(pixel));
    float2 weight = frac(pixel);
    float3 top = lerp(LoadRadiance(environment, texel, size),
        LoadRadiance(environment, texel + int2(1, 0), size), weight.x);
    float3 bottom = lerp(LoadRadiance(environment, texel + int2(0, 1), size),
        LoadRadiance(environment, texel + int2(1, 1), size), weight.x);
    DomeOutput result;
    result.color = float4(saturate(lerp(top, bottom, weight.y) * constants.radiance_scale.rgb), 1.0f);
    result.prim_id = 0xffffffffu;
    result.instance_id = 0xffffffffu;
    return result;
}
