// Procedural Gaussian splat rasterization. The CPU reference stage supplies
// screen-space centers, inverse conics, authored SH radiance, opacity, and a
// stable back-to-front order. Six fixed corner vertices expand each instance to
// its conservative three-sigma square; the fragment stage evaluates the
// actual ellipse and falloff.

struct GaussianConstants
{
    float2 inverse_viewport_size;
};

[[vk::push_constant]]
ConstantBuffer<GaussianConstants> gaussian_constants;

struct GaussianVertexInput
{
    [[vk::location(0)]] float2 corner : CORNER;
    [[vk::location(1)]] float2 center_pixels : CENTER;
    [[vk::location(2)]] float3 inverse_conic : CONIC;
    [[vk::location(3)]] float3 radiance : COLOR0;
    [[vk::location(4)]] float opacity : OPACITY;
    [[vk::location(5)]] float radius_pixels : RADIUS;
    [[vk::location(6)]] float depth : DEPTH;
};

struct GaussianVertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 delta_pixels : TEXCOORD0;
    [[vk::location(1)]] float3 inverse_conic : TEXCOORD1;
    [[vk::location(2)]] float3 radiance : COLOR0;
    [[vk::location(3)]] float opacity : TEXCOORD2;
};

[shader("vertex")]
GaussianVertexOutput gaussian_vertex(GaussianVertexInput input)
{
    GaussianVertexOutput output;
    output.delta_pixels = input.corner * input.radius_pixels;
    float2 pixel = input.center_pixels + output.delta_pixels;
    float2 ndc = pixel * (2.0f * gaussian_constants.inverse_viewport_size) -
        float2(1.0f, 1.0f);
    output.position = float4(ndc, input.depth, 1.0f);
    output.inverse_conic = input.inverse_conic;
    output.radiance = input.radiance;
    output.opacity = input.opacity;
    return output;
}

[shader("fragment")]
float4 gaussian_fragment(GaussianVertexOutput input) : SV_Target0
{
    float2 conic_delta = float2(
        input.inverse_conic.x * input.delta_pixels.x +
            input.inverse_conic.y * input.delta_pixels.y,
        input.inverse_conic.y * input.delta_pixels.x +
            input.inverse_conic.z * input.delta_pixels.y);
    float mahalanobis_squared = dot(input.delta_pixels, conic_delta);
    // The generated square is the conservative three-sigma bound. Discard
    // samples outside the actual ellipse and values below one UNorm step.
    if (mahalanobis_squared > 9.0f)
    {
        discard;
    }
    float alpha = input.opacity * exp(-0.5f * mahalanobis_squared);
    if (alpha < (1.0f / 255.0f))
    {
        discard;
    }
    return float4(max(input.radiance, float3(0.0f, 0.0f, 0.0f)), alpha);
}
