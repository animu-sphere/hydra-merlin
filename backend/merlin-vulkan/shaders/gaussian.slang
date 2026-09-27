#include "../../../core/merlin-render-backend/shaders/gaussian-raster-common.slang"

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
    [[vk::location(7)]] uint resource_id : RESOURCE_ID;
    [[vk::location(8)]] uint particle_id : PARTICLE_ID;
};

struct GaussianVertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 delta_pixels : TEXCOORD0;
    [[vk::location(1)]] float3 inverse_conic : TEXCOORD1;
    [[vk::location(2)]] float3 radiance : COLOR0;
    [[vk::location(3)]] float opacity : TEXCOORD2;
};

struct GaussianIdVertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 delta_pixels : TEXCOORD0;
    [[vk::location(1)]] float3 inverse_conic : TEXCOORD1;
    [[vk::location(2)]] float3 radiance : COLOR0;
    [[vk::location(3)]] float opacity : TEXCOORD2;
    [[vk::location(4)]] nointerpolation uint resource_id : RESOURCE_ID;
    [[vk::location(5)]] nointerpolation uint particle_id : PARTICLE_ID;
};

struct GaussianFragmentOutput
{
    float4 color : SV_Target0;
};

struct GaussianIdOutput
{
    uint prim_id : SV_Target0;
    uint instance_id : SV_Target1;
};

[shader("vertex")]
GaussianVertexOutput gaussian_vertex(GaussianVertexInput input)
{
    GaussianVertexOutput output;
    output.delta_pixels = input.corner * input.radius_pixels;
    output.position = gaussian_raster_position(input.center_pixels,
        output.delta_pixels, input.depth, gaussian_constants.inverse_viewport_size);
    output.inverse_conic = input.inverse_conic;
    output.radiance = input.radiance;
    output.opacity = input.opacity;
    return output;
}

[shader("vertex")]
GaussianIdVertexOutput gaussian_id_vertex(GaussianVertexInput input)
{
    GaussianIdVertexOutput output;
    output.delta_pixels = input.corner * input.radius_pixels;
    output.position = gaussian_raster_position(input.center_pixels,
        output.delta_pixels, input.depth, gaussian_constants.inverse_viewport_size);
    output.inverse_conic = input.inverse_conic;
    output.radiance = input.radiance;
    output.opacity = input.opacity;
    output.resource_id = input.resource_id;
    output.particle_id = input.particle_id;
    return output;
}

[shader("fragment")]
GaussianFragmentOutput gaussian_fragment(GaussianVertexOutput input)
{
    float alpha = gaussian_raster_alpha(
        input.delta_pixels, input.inverse_conic, input.opacity);
    GaussianFragmentOutput output;
    output.color =
        float4(max(input.radiance, float3(0.0f, 0.0f, 0.0f)), alpha);
    return output;
}

[shader("fragment")]
GaussianIdOutput gaussian_id_fragment(GaussianIdVertexOutput input)
{
    gaussian_raster_alpha(input.delta_pixels, input.inverse_conic, input.opacity);
    GaussianIdOutput output;
    // The stream is back-to-front and does not write depth. Integer AOV writes
    // therefore retain the nearest contributing Gaussian in front of opaque
    // Mesh depth. primId identifies the Gaussian resource and instanceId is
    // its stable zero-based particle index.
    output.prim_id = input.resource_id;
    output.instance_id = input.particle_id;
    return output;
}
