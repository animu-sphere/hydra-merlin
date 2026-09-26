// Groups the verified GPU-sorted Gaussian stream by screen tile. Count finds
// how many 16x16-pixel tiles each raster-order record's conservative square
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
// The kernels bind the sort's set layout, four storage buffers, the Vulkan
// guaranteed per-stage minimum, and a push-constant block within the sort's.

static const uint kGaussianTileSize = 16u;

// Control words; must match shader_abi::GaussianTileVerification.
static const uint kGaussianTileRecordCount = 0u;
static const uint kGaussianTileRequestedPairCount = 1u;
static const uint kGaussianTileClampedRecordCount = 2u;
static const uint kGaussianTileVerifiedPairCount = 3u;
static const uint kGaussianTileOrderViolationCount = 4u;
static const uint kGaussianTilePairMismatchCount = 5u;
static const uint kGaussianTileRangeViolationCount = 6u;
static const uint kGaussianTileOccupiedTileCount = 7u;
static const uint kGaussianTileMaxTilePairCount = 8u;
static const uint kGaussianTileIdentityChecksum = 9u;
// Word 10 is written by gaussian-tile-raster.slang's selection kernel.

struct GaussianTileConstants
{
    // Count and emit: padded sorted record count.
    uint record_bound;
    uint pair_capacity;
    uint tile_count_x;
    uint tile_count_y;
    uint viewport_width;
    uint viewport_height;
    // Word offsets inside the control buffer.
    uint offsets_offset;
    uint ranges_offset;
    // Pairs one record may emit, in row-major tile order.
    uint record_pair_limit;
    uint padding0;
    uint padding1;
    uint padding2;
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

struct GaussianTileRect
{
    uint x0;
    uint y0;
    uint x1;
    uint y1;
};

[[vk::push_constant]]
ConstantBuffer<GaussianTileConstants> gaussian_tile_constants;

[[vk::binding(0, 0)]] RWStructuredBuffer<GaussianSortElement> gaussian_tile_source;
[[vk::binding(1, 0)]] RWStructuredBuffer<GaussianSortElement> gaussian_tile_destination;
// Control words, per-record pair offsets and their scan levels, the pair
// histogram and its scan levels, then two words per tile.
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> gaussian_tile_control;
[[vk::binding(3, 0)]] StructuredBuffer<GaussianPreparedRecord> gaussian_tile_records;

bool IsFiniteValue(float value)
{
    return !isnan(value) && !isinf(value);
}

// Mirrors shader_abi::GaussianTileBounds expression for expression, so the
// CPU reference pairs match for identical records.
bool TileBounds(GaussianPreparedRecord record, out GaussianTileRect rect)
{
    rect.x0 = 0u;
    rect.y0 = 0u;
    rect.x1 = 0u;
    rect.y1 = 0u;
    float2 center = record.center_pixels;
    float radius = record.radius_pixels;
    if (!(radius > 0.0f) || !IsFiniteValue(radius) ||
        !IsFiniteValue(center.x) || !IsFiniteValue(center.y))
    {
        return false;
    }
    float max_x = float(gaussian_tile_constants.viewport_width - 1u);
    float max_y = float(gaussian_tile_constants.viewport_height - 1u);
    float low_x = ceil((center.x - radius) - 0.5f);
    float low_y = ceil((center.y - radius) - 0.5f);
    float high_x = floor((center.x + radius) - 0.5f);
    float high_y = floor((center.y + radius) - 0.5f);
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

// Must match shader_abi::GaussianSortMix and GaussianTilePairHash.
uint GaussianTileMix(uint value)
{
    value ^= value >> 16u;
    value *= 0x85EBCA6Bu;
    value ^= value >> 13u;
    value *= 0xC2B2AE35u;
    value ^= value >> 16u;
    return value;
}

uint GaussianTilePairHash(uint tile, uint record)
{
    return GaussianTileMix(GaussianTileMix(tile * 0x85EBCA6Bu) ^ (record * 0x9E3779B9u));
}

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
    GaussianTileRect rect;
    if (index < gaussian_tile_control[kGaussianTileRecordCount] &&
        TileBounds(gaussian_tile_records[index], rect))
    {
        uint ignored;
        count = (rect.x1 - rect.x0 + 1u) * (rect.y1 - rect.y0 + 1u);
        if (count > gaussian_tile_constants.record_pair_limit)
        {
            count = gaussian_tile_constants.record_pair_limit;
            InterlockedAdd(gaussian_tile_control[kGaussianTileClampedRecordCount], 1u, ignored);
        }
        InterlockedAdd(gaussian_tile_control[kGaussianTileRequestedPairCount], count, ignored);
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
    if (!TileBounds(gaussian_tile_records[index], rect))
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
        matches = TileBounds(gaussian_tile_records[pair.value], rect) &&
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
