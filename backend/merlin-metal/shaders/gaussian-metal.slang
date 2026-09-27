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
    float2 delta : TEXCOORD0;
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
    output.delta = corners[vertex] * radius;
    // Preparation uses the supplied projection's NDC axes. Metal's viewport
    // flips both the center and ellipse, matching the mesh path.
    output.position = gaussian_raster_position(center, output.delta, depth,
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
    float alpha = gaussian_raster_alpha(input.delta, input.conic, input.opacity);
    GaussianFragmentOutput output;
    output.color = float4(max(input.radiance, float3(0)), alpha);
    output.prim_id = input.resource;
    output.instance_id = input.particle;
    return output;
}
