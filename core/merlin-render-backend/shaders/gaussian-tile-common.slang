// Resource declarations and packed-record loading belong to the backend.

#include "gaussian-contribution-common.slang"

// Compute an outer bound of the ellipse whose alpha can pass the raster floor.
// Lower the diagonal coefficients and raise the magnitude of the cross term
// to bound rounding in the raster quadratic, then minimize over the other
// coordinate (the Schur complement). Ill-conditioned or unusually large
// footprints retain the exact square. One pixel pads coordinate arithmetic
// including Metal's Y flip. The alpha guard also covers exp/log rounding.
[ForceInline]
bool GaussianContributionExtent(GaussianPreparedRecord record, float2 center,
    inout float2 extent)
{
    float3 conic = record.inverse_conic;
    float radius = record.radius_pixels;
    float2 lower;
    if (!IsFiniteValue(record.opacity) ||
        max(abs(center.x), abs(center.y)) + radius > 1048576.0f ||
        !GaussianContributionLowerBound(conic, radius, lower))
    {
        return true;
    }

    if (record.opacity < kGaussianGuardedAlphaFloor)
    {
        return false;
    }
    float power = min(9.0f, 2.0f * log(record.opacity / kGaussianGuardedAlphaFloor)) +
        kGaussianContributionGuard;
    extent = min(extent, sqrt(power / lower) + float2(1.0f));
    return true;
}

// Mirrors shader_abi::GaussianTileBounds expression for expression, so the
// CPU reference pairs match for identical records.
[ForceInline]
bool TileBounds(GaussianPreparedRecord record, bool adaptive, out GaussianTileRect rect)
{
    rect.x0 = 0u;
    rect.y0 = 0u;
    rect.x1 = 0u;
    rect.y1 = 0u;
    float2 center = TileCenter(record);
    float radius = record.radius_pixels;
    if (!(radius > 0.0f) || !IsFiniteValue(radius) ||
        !IsFiniteValue(center.x) || !IsFiniteValue(center.y))
    {
        return false;
    }
    float2 extent = float2(radius);
    if (adaptive && !GaussianContributionExtent(record, center, extent))
    {
        return false;
    }
    float max_x = float(gaussian_tile_constants.viewport_width - 1u);
    float max_y = float(gaussian_tile_constants.viewport_height - 1u);
    float low_x = ceil((center.x - extent.x) - 0.5f);
    float low_y = ceil((center.y - extent.y) - 0.5f);
    float high_x = floor((center.x + extent.x) - 0.5f);
    float high_y = floor((center.y + extent.y) - 0.5f);
    if (!(high_x >= 0.0f) || !(high_y >= 0.0f) || !(low_x <= max_x) ||
        !(low_y <= max_y) || low_x > high_x || low_y > high_y)
    {
        return false;
    }
    rect.x0 = uint(max(low_x, 0.0f)) / kGaussianTileSize;
    rect.y0 = uint(max(low_y, 0.0f)) / kGaussianTileSize;
    rect.x1 = uint(min(high_x, max_x)) / kGaussianTileSize;
    rect.y1 = uint(min(high_y, max_y)) / kGaussianTileSize;
    return true;
}

[ForceInline]
uint TilePairCount(GaussianTileRect rect)
{
    return (rect.x1 - rect.x0 + 1u) * (rect.y1 - rect.y0 + 1u);
}

// Must match shader_abi::GaussianSortMix and GaussianTilePairHash.
[ForceInline]
uint GaussianTileMix(uint value)
{
    value ^= value >> 16u;
    value *= 0x85EBCA6Bu;
    value ^= value >> 13u;
    value *= 0xC2B2AE35u;
    value ^= value >> 16u;
    return value;
}

[ForceInline]
uint GaussianTilePairHash(uint tile, uint record)
{
    return GaussianTileMix(GaussianTileMix(tile * 0x85EBCA6Bu) ^ (record * 0x9E3779B9u));
}

[ForceInline]
uint StoredPairCount()
{
    return min(gaussian_tile_constants.pair_capacity,
               gaussian_tile_control[kGaussianTileRequestedPairCount]);
}

// Every padded record writes its count, zero past the real records, because
// the block scan reads the whole padded range.
[shader("compute")]
[numthreads(256, 1, 1)]
void gaussian_tile_count(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
    uint index = dispatch_thread_id.x;
    if (index >= gaussian_tile_constants.record_bound)
    {
        return;
    }
    uint count = 0u;
    if (index < gaussian_tile_control[kGaussianTileRecordCount])
    {
        GaussianPreparedRecord record = LoadTilePreparedRecord(index);
        bool adaptive = gaussian_tile_constants.adaptive_bounds != 0u;
        GaussianTileRect rect;
        if (TileBounds(record, adaptive, rect))
        {
            count = TilePairCount(rect);
        }
        uint ignored;
        uint limit = gaussian_tile_constants.record_pair_limit;
        if (count > limit)
        {
            count = limit;
            InterlockedAdd(gaussian_tile_control[kGaussianTileClampedRecordCount], 1u, ignored);
        }
        if (adaptive && TileBounds(record, false, rect))
        {
            uint exact_count = min(TilePairCount(rect), limit);
            if (count < exact_count)
            {
                InterlockedAdd(gaussian_tile_control[kGaussianTileRejectedPairCount],
                    exact_count - count, ignored);
                if (count == 0u)
                {
                    InterlockedAdd(gaussian_tile_control[kGaussianTileCulledRecordCount], 1u, ignored);
                }
            }
        }
        if (count != 0u)
        {
            InterlockedAdd(gaussian_tile_control[kGaussianTileRequestedPairCount], count, ignored);
        }
    }
    gaussian_tile_control[gaussian_tile_constants.offsets_offset + index] = count;
}

// Writes each record's pairs at its scanned offset in row-major tile order,
// so the unsorted pair list is record-major and therefore back to front.
[shader("compute")]
[numthreads(256, 1, 1)]
void gaussian_tile_emit(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
    uint index = dispatch_thread_id.x;
    if (index >= gaussian_tile_constants.record_bound ||
        index >= gaussian_tile_control[kGaussianTileRecordCount])
    {
        return;
    }
    GaussianTileRect rect;
    if (!TileBounds(LoadTilePreparedRecord(index), gaussian_tile_constants.adaptive_bounds != 0u, rect))
    {
        return;
    }
    uint position = gaussian_tile_control[gaussian_tile_constants.offsets_offset + index];
    uint remaining = gaussian_tile_constants.record_pair_limit;
    for (uint y = rect.y0; y <= rect.y1; ++y)
    {
        for (uint x = rect.x0; x <= rect.x1; ++x)
        {
            if (remaining == 0u || position >= gaussian_tile_constants.pair_capacity)
            {
                return;
            }
            --remaining;
            GaussianSortElement pair;
            pair.key_low = y * gaussian_tile_constants.tile_count_x + x;
            pair.key_high = 0u;
            pair.value = index;
            gaussian_tile_destination[position] = pair;
            ++position;
        }
    }
}

// The host clears every range to [0, 0), so a tile without pairs stays empty.
[shader("compute")]
[numthreads(256, 1, 1)]
void gaussian_tile_ranges(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
    uint index = dispatch_thread_id.x;
    uint count = StoredPairCount();
    if (index >= count)
    {
        return;
    }
    uint tile = gaussian_tile_source[index].key_low;
    if (tile >= gaussian_tile_constants.tile_count_x * gaussian_tile_constants.tile_count_y)
    {
        return;
    }
    uint range = gaussian_tile_constants.ranges_offset + 2u * tile;
    if (index == 0u || gaussian_tile_source[index - 1u].key_low != tile)
    {
        gaussian_tile_control[range] = index;
    }
    if (index + 1u == count || gaussian_tile_source[index + 1u].key_low != tile)
    {
        gaussian_tile_control[range + 1u] = index + 1u;
    }
}

// Checks strictly increasing (tile, record) order, each pair against the
// record it names and the tile range holding it, and accumulates occupancy
// and an order-sensitive checksum the CPU reference can reproduce.
[shader("compute")]
[numthreads(256, 1, 1)]
void gaussian_tile_verify(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
    uint index = dispatch_thread_id.x;
    uint count = StoredPairCount();
    if (index >= count)
    {
        return;
    }
    uint ignored;
    GaussianSortElement pair = gaussian_tile_source[index];
    if (index + 1u < count)
    {
        GaussianSortElement next = gaussian_tile_source[index + 1u];
        bool ordered = next.key_low > pair.key_low ||
            (next.key_low == pair.key_low && next.value > pair.value);
        if (!ordered)
        {
            InterlockedAdd(gaussian_tile_control[kGaussianTileOrderViolationCount], 1u, ignored);
        }
    }
    uint tile = pair.key_low;
    bool matches = pair.key_high == 0u &&
        tile < gaussian_tile_constants.tile_count_x * gaussian_tile_constants.tile_count_y &&
        pair.value < gaussian_tile_control[kGaussianTileRecordCount];
    if (matches)
    {
        GaussianTileRect rect;
        uint tile_x = tile % gaussian_tile_constants.tile_count_x;
        uint tile_y = tile / gaussian_tile_constants.tile_count_x;
        matches = TileBounds(LoadTilePreparedRecord(pair.value),
                gaussian_tile_constants.adaptive_bounds != 0u, rect) &&
            tile_x >= rect.x0 && tile_x <= rect.x1 &&
            tile_y >= rect.y0 && tile_y <= rect.y1 &&
            (tile_y - rect.y0) * (rect.x1 - rect.x0 + 1u) + (tile_x - rect.x0) <
                gaussian_tile_constants.record_pair_limit;
    }
    if (!matches)
    {
        InterlockedAdd(gaussian_tile_control[kGaussianTilePairMismatchCount], 1u, ignored);
        return;
    }
    uint range = gaussian_tile_constants.ranges_offset + 2u * tile;
    uint begin = gaussian_tile_control[range];
    uint end = gaussian_tile_control[range + 1u];
    if (index < begin || index >= end)
    {
        InterlockedAdd(gaussian_tile_control[kGaussianTileRangeViolationCount], 1u, ignored);
    }
    else if (index == begin)
    {
        InterlockedAdd(gaussian_tile_control[kGaussianTileOccupiedTileCount], 1u, ignored);
        InterlockedMax(gaussian_tile_control[kGaussianTileMaxTilePairCount], end - begin, ignored);
    }
    InterlockedAdd(gaussian_tile_control[kGaussianTileVerifiedPairCount], 1u, ignored);
    InterlockedAdd(gaussian_tile_control[kGaussianTileIdentityChecksum],
                   (index + 1u) * GaussianTilePairHash(tile, pair.value), ignored);
}
