#pragma once

float2 RadianceUv(float3 direction)
{
    float azimuth = any(direction.xz != float2(0.0f)) ? atan2(direction.z, direction.x) : 0.0f;
    return float2(frac(azimuth / 6.2831853072f),
        acos(clamp(direction.y, -1.0f, 1.0f)) / 3.1415926536f);
}

float3 LoadRadiance(Texture2D<float4> environment, int2 texel, int2 size, int row_offset)
{
    texel.x = (texel.x + size.x) % size.x;
    texel.y = clamp(texel.y, 0, size.y - 1) + row_offset;
    float4 rgbe = round(environment.Load(int3(texel, 0)) * 255.0f);
    return rgbe.a == 0.0f ? float3(0.0f) : rgbe.rgb * exp2(rgbe.a - 136.0f);
}

float3 SampleRadiance(Texture2D<float4> environment, float2 uv, int2 size, int row_offset)
{
    float2 pixel = uv * float2(size) - 0.5f;
    int2 texel = int2(floor(pixel));
    float2 weight = frac(pixel);
    float3 top = lerp(LoadRadiance(environment, texel, size, row_offset),
        LoadRadiance(environment, texel + int2(1, 0), size, row_offset), weight.x);
    float3 bottom = lerp(LoadRadiance(environment, texel + int2(0, 1), size, row_offset),
        LoadRadiance(environment, texel + int2(1, 1), size, row_offset), weight.x);
    return lerp(top, bottom, weight.y);
}
