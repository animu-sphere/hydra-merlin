#include "../../../core/merlin-render-backend/shaders/gaussian-prepare-abi.slang"

// This block intentionally uses a uniform descriptor: its two matrices make
// it 176 bytes, larger than Vulkan's guaranteed 128-byte push-constant limit.
[[vk::binding(7, 3)]]
ConstantBuffer<GaussianPrepareConstants> gaussian_prepare_constants;

// Byte-addressed inputs match the existing tightly packed arena payloads:
// float3 positions, six-float covariance, float opacity, and float3 SH terms.
[[vk::binding(0, 3)]] ByteAddressBuffer gaussian_positions;
[[vk::binding(1, 3)]] ByteAddressBuffer gaussian_covariances;
[[vk::binding(2, 3)]] ByteAddressBuffer gaussian_opacities;
[[vk::binding(3, 3)]] ByteAddressBuffer gaussian_radiance;
[[vk::binding(4, 3)]] RWStructuredBuffer<uint> gaussian_candidate_results;
[[vk::binding(5, 3)]] RWStructuredBuffer<GaussianPreparedRecord> gaussian_prepared_records;
[[vk::binding(6, 3)]] RWStructuredBuffer<GaussianPrepareDispatchCounters> gaussian_prepare_counters;

void StorePreparedRecord(uint index, GaussianPreparedRecord record)
{
    gaussian_prepared_records[index] = record;
}

#include "../../../core/merlin-render-backend/shaders/gaussian-prepare-common.slang"
