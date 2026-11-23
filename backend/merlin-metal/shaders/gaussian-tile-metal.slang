// Groups the verified GPU-sorted Gaussian stream by screen tile. Count finds
// how many 16x16-pixel tiles each raster-order record's conservative bounds
// covers, a block scan turns the counts into pair offsets, and emit writes
// (tile, record) pairs in record order. The sort's histogram, scan, and
// scatter kernels then order the pairs by tile index alone; the scatter is
// stable, so every tile keeps the verified back-to-front order without a
// wider key. Ranges records each tile's [begin, end) pair span, and verify
// checks the grouping against the records it names.
//
// Pairs use the sort element layout: key_low is the row-major tile index,
// key_high is zero, and value is the record's sorted position, which indexes
// the gathered raster-order records. Pairs past the fixed capacity are
// dropped, never written, and every later stage reads only the stored prefix.
//
// Metal uses the sort pipeline's buffer slots with constants at slot 4.
// The source is a packed byte-addressed prepared stream.

#include "../../../core/merlin-render-backend/shaders/gaussian-tile-abi.slang"

[[vk::binding(4, 0)]] ConstantBuffer<GaussianTileConstants> gaussian_tile_constants : register(b4);

[[vk::binding(0, 0)]] RWStructuredBuffer<GaussianSortElement> gaussian_tile_source : register(u0);
[[vk::binding(1, 0)]] RWStructuredBuffer<GaussianSortElement> gaussian_tile_destination : register(u1);
// Control words, per-record pair offsets and their scan levels, the pair
// histogram and its scan levels, then two words per tile.
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> gaussian_tile_control : register(u2);
[[vk::binding(3, 0)]] ByteAddressBuffer gaussian_tile_records : register(t3);

// Metal's packed prepared stream is 64 bytes per record; float3 in a
// StructuredBuffer would change the ABI.
[ForceInline]
GaussianPreparedRecord LoadTilePreparedRecord(uint index)
{
    uint offset = index * 64u;
    float4 geometry = asfloat(gaussian_tile_records.Load4(offset));
    float4 conic = asfloat(gaussian_tile_records.Load4(offset + 16u));
    float4 radiance = asfloat(gaussian_tile_records.Load4(offset + 32u));
    uint4 identity = gaussian_tile_records.Load4(offset + 48u);
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

[ForceInline]
float2 TileCenter(GaussianPreparedRecord record)
{
    // Prepared centers use projection/NDC Y; Metal texture rows start at the top.
    return float2(record.center_pixels.x,
        float(gaussian_tile_constants.viewport_height) - record.center_pixels.y);
}

#include "../../../core/merlin-render-backend/shaders/gaussian-tile-common.slang"
