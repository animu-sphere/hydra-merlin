#include "../../../core/merlin-render-backend/shaders/gaussian-prepare-abi.slang"

// Metal uses one explicit buffer namespace for constants and storage.
[[vk::binding(7, 0)]]
ConstantBuffer<GaussianPrepareConstants> gaussian_prepare_constants : register(b7);

// Byte-addressed inputs match the existing tightly packed arena payloads:
// float3 positions, six-float covariance, float opacity, and float3 SH terms.
[[vk::binding(0, 0)]] ByteAddressBuffer gaussian_positions : register(t0);
[[vk::binding(1, 0)]] ByteAddressBuffer gaussian_covariances : register(t1);
[[vk::binding(2, 0)]] ByteAddressBuffer gaussian_opacities : register(t2);
[[vk::binding(3, 0)]] ByteAddressBuffer gaussian_radiance : register(t3);
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> gaussian_candidate_results : register(u4);
[[vk::binding(5, 0)]] RWByteAddressBuffer gaussian_prepared_records : register(u5);
[[vk::binding(6, 0)]] RWStructuredBuffer<GaussianPrepareDispatchCounters> gaussian_prepare_counters : register(u6);

// Raw records keep the Vulkan 64-byte ABI despite Metal float3 alignment.
void StorePreparedRecord(uint index, GaussianPreparedRecord record)
{
    uint offset = index * 64u;
    gaussian_prepared_records.Store4(offset, asuint(float4(record.center_pixels, record.radius_pixels, record.depth)));
    gaussian_prepared_records.Store4(offset + 16u, asuint(float4(record.inverse_conic, record.opacity)));
    gaussian_prepared_records.Store4(offset + 32u, asuint(float4(record.radiance, record.sort_key)));
    gaussian_prepared_records.Store4(offset + 48u, uint4(record.resource_id_low, record.resource_id_high, record.particle_id, record.padding));
}

#include "../../../core/merlin-render-backend/shaders/gaussian-prepare-common.slang"
