#include "radiance-environment.slang"

// Atlas layout and roughness levels from specular_environment.cpp.
static const float kSpecularRoughness[6] = {0.045f, 0.15f, 0.3f, 0.5f, 0.75f, 1.0f};
static const int kSpecularRow[6] = {0, 256, 384, 448, 480, 512};

float3 SampleSpecularLevel(Texture2D<float4> atlas, float2 uv, int level)
{
    int width = max(64, 512 >> level);
    return SampleRadiance(atlas, uv, int2(width, width / 2), kSpecularRow[level]);
}

float2 LoadEnvironmentBrdf(Texture2D<float4> atlas, int2 texel)
{
    texel = min(texel, int2(63)) + int2(64, 448);
    float4 bytes = round(atlas.Load(int3(texel, 0)) * 255.0f);
    return (bytes.xz * 256.0f + bytes.yw) / 65535.0f;
}

float2 SampleEnvironmentBrdf(Texture2D<float4> atlas, float n_dot_v, float roughness)
{
    float2 coordinate = float2(n_dot_v, roughness) * 63.0f;
    int2 texel = int2(coordinate);
    float2 weight = frac(coordinate);
    float2 top = lerp(LoadEnvironmentBrdf(atlas, texel),
        LoadEnvironmentBrdf(atlas, texel + int2(1, 0)), weight.x);
    float2 bottom = lerp(LoadEnvironmentBrdf(atlas, texel + int2(0, 1)),
        LoadEnvironmentBrdf(atlas, texel + int2(1, 1)), weight.x);
    return lerp(top, bottom, weight.y);
}

float3 EvaluateSpecularEnvironment(Texture2D<float4> atlas, float3 direction,
    float n_dot_v, float roughness, float3 reflectance)
{
    int level = 0;
    for (int next = 1; next < 5; ++next)
    {
        level += roughness >= kSpecularRoughness[next] ? 1 : 0;
    }
    float weight = (roughness - kSpecularRoughness[level]) /
        (kSpecularRoughness[level + 1] - kSpecularRoughness[level]);
    float2 uv = RadianceUv(direction);
    float3 radiance = SampleSpecularLevel(atlas, uv, level);
    if (weight > 0.0f)
        radiance = lerp(radiance, SampleSpecularLevel(atlas, uv, level + 1), weight);
    float2 brdf = SampleEnvironmentBrdf(atlas, n_dot_v, roughness);
    return radiance * (reflectance * brdf.x + brdf.y);
}
