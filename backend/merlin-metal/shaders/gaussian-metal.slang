#include "../../../core/merlin-render-backend/shaders/gaussian-raster-common.slang"

#include "../../../core/merlin-render-backend/shaders/gaussian-raster-abi.slang"

// Raw scalar storage preserves the existing 52-byte prepared stream, without
// relying on Metal's float3 alignment or resource-register-class remapping.
[[vk::binding(0, 0)]] ByteAddressBuffer gaussian_instances : register(t0);
struct GaussianConstants { float2 inverse_extent; };
[[vk::binding(1, 0)]] ConstantBuffer<GaussianConstants> gaussian_constants : register(b1);

struct GaussianVertexOutput
{
    float4 position : SV_Position;
    nointerpolation float2 center_pixels : TEXCOORD0;
    nointerpolation float3 conic : TEXCOORD1;
    nointerpolation float3 radiance : COLOR0;
    nointerpolation float opacity : TEXCOORD2;
    nointerpolation uint resource : RESOURCE_ID;
    nointerpolation uint particle : PARTICLE_ID;
};

[shader("vertex")]
GaussianVertexOutput gaussian_metal_vertex(
    uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    static const float2 corners[6] = {
        float2(-1,-1), float2(1,-1), float2(1,1),
        float2(-1,-1), float2(1,1), float2(-1,1)};
    uint offset = instance * MERLIN_GAUSSIAN_STRIDE;
    float2 center = asfloat(gaussian_instances.Load2(offset + MERLIN_GAUSSIAN_CENTER));
    float radius = asfloat(gaussian_instances.Load(offset + MERLIN_GAUSSIAN_RADIUS));
    float depth = asfloat(gaussian_instances.Load(offset + MERLIN_GAUSSIAN_DEPTH));
    GaussianVertexOutput output;
    float2 delta = corners[vertex] * radius;
    output.center_pixels = center;
    // Preparation uses the supplied projection's NDC axes. Metal's viewport
    // flips both the center and ellipse, matching the mesh path.
    output.position = gaussian_raster_position(center, delta, depth,
        gaussian_constants.inverse_extent);
    output.conic = asfloat(gaussian_instances.Load3(offset + MERLIN_GAUSSIAN_CONIC));
    output.radiance = asfloat(gaussian_instances.Load3(offset + MERLIN_GAUSSIAN_RADIANCE));
    output.opacity = asfloat(gaussian_instances.Load(offset + MERLIN_GAUSSIAN_OPACITY));
    output.resource = gaussian_instances.Load(offset + MERLIN_GAUSSIAN_RESOURCE);
    output.particle = gaussian_instances.Load(offset + MERLIN_GAUSSIAN_PARTICLE);
    return output;
}

struct GaussianFragmentOutput
{
    float4 color : SV_Target0;
    uint prim_id : SV_Target1;
    uint instance_id : SV_Target2;
};

[shader("fragment")]
GaussianFragmentOutput gaussian_metal_fragment(GaussianVertexOutput input)
{
    // Use the same pixel center as compute tiles, without interpolated quad
    // offsets depending on rasterizer subpixel quantization.
    float2 sample_pixels = float2(input.position.x,
        1.0f / gaussian_constants.inverse_extent.y - input.position.y);
    float alpha = gaussian_raster_alpha(sample_pixels - input.center_pixels,
        input.conic, input.opacity);
    GaussianFragmentOutput output;
    output.color = float4(saturate(input.radiance), saturate(alpha));
    output.prim_id = input.resource;
    output.instance_id = input.particle;
    return output;
}
