// Composites the verified tile ranges front to back in compute. Selection
// runs after binning: when every requested pair was stored and no record was
// clamped, it zeroes the sorted-stream draw's instance count and publishes
// the frame for tile raster; otherwise the draws keep rasterizing the whole
// sorted stream, so an overflowing frame never loses a pair from the image.
//
// Raster runs one 16x16 workgroup per tile, one thread per pixel, after the
// Mesh and empty Gaussian subpasses. Each batch of up to 256 records is
// staged in workgroup memory from the front of the tile's back-to-front
// range, and a tile stops once every pixel's transmittance is negligible.
// Every record is evaluated with the procedural fragment stage's expressions
// and fixed-function state: the three-sigma square and ellipse, the
// one-UNorm-step alpha floor, clamping to [0, 1] before blending, and the
// LESS_OR_EQUAL Mesh depth test with clipping to [0, 1]. The composite
// C + T * destination equals the back-to-front "over" blend, and the first
// contributing record is the nearest one, which the ID AOVs keep.
//
// Metal binds four buffers, three writable color/ID textures and one sampled
// depth texture, with constants at buffer slot 8.

static const uint kGaussianTileSize = 16u;
static const uint kGaussianTileRasterBatch = kGaussianTileSize * kGaussianTileSize;

// Control words; must match shader_abi::GaussianTileVerification.
static const uint kGaussianTileRequestedPairCount = 1u;
static const uint kGaussianTileClampedRecordCount = 2u;
static const uint kGaussianTileRasterSelected = 10u;

// VkDrawIndirectCommand words of the sorted-stream draw.
static const uint kGaussianRasterDrawInstanceCount = 1u;

// A pixel stops once the remaining records and the destination can change
// its color by less than a quarter of one UNorm step.
static const float kGaussianTileRasterTransmittanceFloor = 1.0f / 1024.0f;

struct GaussianTileRasterConstants
{
    uint tile_count_x;
    uint tile_count_y;
    uint viewport_width;
    uint viewport_height;
    // Word offset of the tile ranges inside the control buffer.
    uint ranges_offset;
    uint pair_capacity;
    uint padding0;
    uint padding1;
};

// Must match gaussian-sort.slang; reflection checks both layouts.
struct GaussianSortElement
{
    uint key_low;
    uint key_high;
    uint value;
};

// Must match gaussian-prepare.slang; reflection checks both layouts.
struct GaussianPreparedRecord
{
    float2 center_pixels;
    float radius_pixels;
    float depth;
    float3 inverse_conic;
    float opacity;
    float3 radiance;
    float sort_key;
    uint resource_id_low;
    uint resource_id_high;
    uint particle_id;
    uint padding;
};

[[vk::binding(8, 0)]] ConstantBuffer<GaussianTileRasterConstants> gaussian_tile_raster_constants : register(b8);

[[vk::binding(0, 0)]] StructuredBuffer<GaussianSortElement> gaussian_tile_raster_pairs : register(t0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> gaussian_tile_raster_control : register(u1);
[[vk::binding(2, 0)]] ByteAddressBuffer gaussian_tile_raster_records : register(t2);
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> gaussian_tile_raster_draw : register(u3);
[[vk::binding(4, 0)]] [format("rgba8")] RWTexture2D<float4> gaussian_tile_raster_color : register(u4);
[[vk::binding(5, 0)]] [format("r32ui")] RWTexture2D<uint> gaussian_tile_raster_prim_id : register(u5);
[[vk::binding(6, 0)]] [format("r32ui")] RWTexture2D<uint> gaussian_tile_raster_instance_id : register(u6);
[[vk::binding(7, 0)]] Texture2D<float> gaussian_tile_raster_depth : register(t7);

// Read the packed 64-byte Metal prepared record instead of aligned float3.
[ForceInline]
GaussianPreparedRecord LoadTileRasterRecord(uint index)
{
    uint offset = index * 64u;
    float4 geometry = asfloat(gaussian_tile_raster_records.Load4(offset));
    float4 conic = asfloat(gaussian_tile_raster_records.Load4(offset + 16u));
    float4 radiance = asfloat(gaussian_tile_raster_records.Load4(offset + 32u));
    uint4 identity = gaussian_tile_raster_records.Load4(offset + 48u);
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

// The fields raster reads, staged per batch as explicit vectors, 14 KiB in
// total: within the 16 KiB portable baseline without struct padding.
// Bounds hold center, radius, and depth; shape holds the inverse conic and
// opacity; ids hold the resource and particle identity.
groupshared float4 gs_tile_raster_bounds[kGaussianTileRasterBatch];
groupshared float4 gs_tile_raster_shape[kGaussianTileRasterBatch];
groupshared float4 gs_tile_raster_radiance[kGaussianTileRasterBatch];
groupshared uint2 gs_tile_raster_ids[kGaussianTileRasterBatch];
groupshared uint gs_tile_raster_done_count;

// One thread decides for the frame. The draw arguments were already copied
// for the resolve check, so zeroing the instance count only skips the
// sorted-stream draws.
[shader("compute")]
[numthreads(1, 1, 1)]
void gaussian_tile_raster_select()
{
    bool complete = gaussian_tile_raster_control[kGaussianTileRequestedPairCount] <=
            gaussian_tile_raster_constants.pair_capacity &&
        gaussian_tile_raster_control[kGaussianTileClampedRecordCount] == 0u;
    gaussian_tile_raster_control[kGaussianTileRasterSelected] = complete ? 1u : 0u;
    if (complete)
    {
        gaussian_tile_raster_draw[kGaussianRasterDrawInstanceCount] = 0u;
    }
}

[shader("compute")]
[numthreads(16, 16, 1)]
void gaussian_tile_raster(uint3 group_id : SV_GroupID,
                          uint3 group_thread_id : SV_GroupThreadID,
                          uint group_index : SV_GroupIndex)
{
    // Both exits are uniform across the workgroup, ahead of any barrier.
    if (gaussian_tile_raster_control[kGaussianTileRasterSelected] == 0u)
    {
        return;
    }
    uint tile = group_id.y * gaussian_tile_raster_constants.tile_count_x + group_id.x;
    uint range = gaussian_tile_raster_constants.ranges_offset + 2u * tile;
    uint begin = gaussian_tile_raster_control[range];
    uint end = gaussian_tile_raster_control[range + 1u];
    if (begin >= end)
    {
        return;
    }

    uint2 pixel = group_id.xy * kGaussianTileSize + group_thread_id.xy;
    bool inside = pixel.x < gaussian_tile_raster_constants.viewport_width &&
        pixel.y < gaussian_tile_raster_constants.viewport_height;
    float2 sample_pixels = float2(pixel) + float2(0.5f, 0.5f);
    float mesh_depth = 0.0f;
    if (inside)
    {
        mesh_depth = gaussian_tile_raster_depth.Load(int3(int2(pixel), 0));
    }

    float3 color = float3(0.0f, 0.0f, 0.0f);
    float transmittance = 1.0f;
    bool done = !inside;
    bool identified = false;
    uint prim_id = 0u;
    uint instance_id = 0u;

    // Pairs are back to front, so the front of the range is its end.
    uint cursor = end;
    while (cursor > begin)
    {
        uint count = min(kGaussianTileRasterBatch, cursor - begin);
        // The previous batch's readers and the done-count read are finished.
        GroupMemoryBarrierWithGroupSync();
        if (group_index == 0u)
        {
            gs_tile_raster_done_count = 0u;
        }
        if (group_index < count)
        {
            GaussianPreparedRecord source = LoadTileRasterRecord(
                gaussian_tile_raster_pairs[cursor - 1u - group_index].value);
            gs_tile_raster_bounds[group_index] = float4(source.center_pixels,
                source.radius_pixels, source.depth);
            gs_tile_raster_shape[group_index] = float4(source.inverse_conic,
                source.opacity);
            gs_tile_raster_radiance[group_index] = float4(source.radiance, 0.0f);
            gs_tile_raster_ids[group_index] = uint2(source.resource_id_low,
                source.particle_id);
        }
        GroupMemoryBarrierWithGroupSync();

        for (uint i = 0u; i < count && !done; ++i)
        {
            float4 bounds = gs_tile_raster_bounds[i];
            float2 delta = sample_pixels - bounds.xy;
            // The procedural quad covers only the conservative square, and
            // clipping rejects depth outside [0, 1].
            if (abs(delta.x) > bounds.z || abs(delta.y) > bounds.z ||
                !(bounds.w >= 0.0f) || !(bounds.w <= mesh_depth))
            {
                continue;
            }
            float4 shape = gs_tile_raster_shape[i];
            float2 conic_delta = float2(
                shape.x * delta.x + shape.y * delta.y,
                shape.y * delta.x + shape.z * delta.y);
            float mahalanobis_squared = dot(delta, conic_delta);
            if (mahalanobis_squared > 9.0f)
            {
                continue;
            }
            float alpha = shape.w * exp(-0.5f * mahalanobis_squared);
            if (alpha < (1.0f / 255.0f))
            {
                continue;
            }
            // UNorm blending clamps source values before it blends.
            alpha = saturate(alpha);
            color += transmittance * alpha * saturate(gs_tile_raster_radiance[i].xyz);
            transmittance *= 1.0f - alpha;
            if (!identified)
            {
                identified = true;
                uint2 ids = gs_tile_raster_ids[i];
                prim_id = ids.x;
                instance_id = ids.y;
            }
            done = transmittance < kGaussianTileRasterTransmittanceFloor;
        }

        if (done)
        {
            uint ignored;
            InterlockedAdd(gs_tile_raster_done_count, 1u, ignored);
        }
        GroupMemoryBarrierWithGroupSync();
        if (gs_tile_raster_done_count == kGaussianTileRasterBatch)
        {
            break;
        }
        cursor -= count;
    }

    if (identified)
    {
        float4 destination = gaussian_tile_raster_color[pixel];
        gaussian_tile_raster_color[pixel] = float4(
            color + transmittance * destination.rgb,
            (1.0f - transmittance) + transmittance * destination.a);
        gaussian_tile_raster_prim_id[pixel] = prim_id;
        gaussian_tile_raster_instance_id[pixel] = instance_id;
    }
}
