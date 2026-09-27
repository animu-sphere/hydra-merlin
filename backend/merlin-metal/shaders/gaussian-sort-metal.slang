#include "../../../core/merlin-render-backend/shaders/gaussian-sort-abi.slang"

[[vk::binding(4, 0)]]
ConstantBuffer<GaussianSortConstants> gaussian_sort_constants : register(b4);

[[vk::binding(0, 0)]] RWStructuredBuffer<GaussianSortElement> gaussian_sort_source : register(u0);
[[vk::binding(1, 0)]] RWStructuredBuffer<GaussianSortElement> gaussian_sort_destination : register(u1);
// Verification words, per-resource visible counts, then the digit-major
// histogram and its scan levels.
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> gaussian_sort_scan : register(u2);
[[vk::binding(3, 0)]] ByteAddressBuffer gaussian_sort_prepared_records : register(t3);

// Match the raw 64-byte stream written by Metal preparation.
[ForceInline]
GaussianPreparedRecord LoadSortPreparedRecord(uint index)
{
    uint offset = index * 64u;
    float4 geometry = asfloat(gaussian_sort_prepared_records.Load4(offset));
    float4 conic = asfloat(gaussian_sort_prepared_records.Load4(offset + 16u));
    float4 radiance = asfloat(gaussian_sort_prepared_records.Load4(offset + 32u));
    uint4 identity = gaussian_sort_prepared_records.Load4(offset + 48u);
    GaussianPreparedRecord record;
    record.center_pixels = geometry.xy;
    record.radius_pixels = geometry.z;
    record.depth = geometry.w;
    record.inverse_conic = conic.xyz;
    record.opacity = conic.w;
    record.radiance = radiance.xyz;
    record.sort_key = radiance.w;
    record.resource_id_low = identity.x;
    record.resource_id_high = identity.y;
    record.particle_id = identity.z;
    record.padding = identity.w;
    return record;
}

#include "../../../core/merlin-render-backend/shaders/gaussian-sort-common.slang"
