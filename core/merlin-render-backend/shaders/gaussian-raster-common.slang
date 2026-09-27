// Shared by the Vulkan and Metal reference raster entry points.
float4 gaussian_raster_position(float2 center, float2 delta, float depth,
    float2 inverse_extent)
{
    float2 ndc = (center + delta) * (2.0f * inverse_extent) - float2(1.0f, 1.0f);
    return float4(ndc, depth, 1.0f);
}

float gaussian_raster_alpha(float2 delta, float3 conic, float opacity)
{
    float2 conic_delta = float2(
        conic.x * delta.x + conic.y * delta.y,
        conic.y * delta.x + conic.z * delta.y);
    float power = dot(delta, conic_delta);
    if (power > 9.0f)
        discard;
    float alpha = opacity * exp(-0.5f * power);
    if (alpha < (1.0f / 255.0f))
        discard;
    return alpha;
}
