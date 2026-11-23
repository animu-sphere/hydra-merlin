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
// The kernels bind the sort's set layout, four storage buffers, the Vulkan
// guaranteed per-stage minimum, and a push-constant block within the sort's.

#include "../../../core/merlin-render-backend/shaders/gaussian-tile-abi.slang"

[[vk::push_constant]]
ConstantBuffer<GaussianTileConstants> gaussian_tile_constants;

[[vk::binding(0, 0)]] RWStructuredBuffer<GaussianSortElement> gaussian_tile_source;
[[vk::binding(1, 0)]] RWStructuredBuffer<GaussianSortElement> gaussian_tile_destination;
// Control words, per-record pair offsets and their scan levels, the pair
// histogram and its scan levels, then two words per tile.
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> gaussian_tile_control;
[[vk::binding(3, 0)]] StructuredBuffer<GaussianPreparedRecord> gaussian_tile_records;

GaussianPreparedRecord LoadTilePreparedRecord(uint index)
{
    return gaussian_tile_records[index];
}

float2 TileCenter(GaussianPreparedRecord record)
{
    return record.center_pixels;
}

#include "../../../core/merlin-render-backend/shaders/gaussian-tile-common.slang"
