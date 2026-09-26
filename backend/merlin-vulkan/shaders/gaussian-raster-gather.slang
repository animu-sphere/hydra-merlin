// Gathers the verified GPU-sorted Gaussian stream into raster order. Each
// thread copies the prepared record its sorted element names, so the
// procedural raster reads the same 64-byte records the preparation wrote,
// back to front, without a CPU-sorted upload. The thread holding the last
// real element publishes the instance count of the indirect draw; sentinels
// sort last, so that position is the number of real records.
//
// The set layout matches the sort kernels': four storage buffers, the Vulkan
// guaranteed per-stage minimum, and a push-constant block within theirs.

static const uint kGaussianSortInvalidValue = 0xFFFFFFFFu;

// VkDrawIndirectCommand words. The host initializes six corner vertices and
// zero instances, so a frame without real records draws nothing.
static const uint kGaussianRasterDrawInstanceCount = 1u;

struct GaussianRasterGatherConstants
{
    // Padded sorted key count.
    uint element_count;
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

[[vk::push_constant]]
ConstantBuffer<GaussianRasterGatherConstants> gaussian_raster_gather_constants;

[[vk::binding(0, 0)]] StructuredBuffer<GaussianSortElement> gaussian_raster_sorted;
[[vk::binding(1, 0)]] StructuredBuffer<GaussianPreparedRecord> gaussian_raster_prepared_records;
[[vk::binding(2, 0)]] RWStructuredBuffer<GaussianPreparedRecord> gaussian_raster_records;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> gaussian_raster_draw;

[shader("compute")]
[numthreads(256, 1, 1)]
void gaussian_raster_gather(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
    uint index = dispatch_thread_id.x;
    uint count = gaussian_raster_gather_constants.element_count;
    if (index >= count)
    {
        return;
    }
    uint value = gaussian_raster_sorted[index].value;
    if (value == kGaussianSortInvalidValue)
    {
        return;
    }
    gaussian_raster_records[index] = gaussian_raster_prepared_records[value];
    if (index + 1u == count ||
        gaussian_raster_sorted[index + 1u].value == kGaussianSortInvalidValue)
    {
        gaussian_raster_draw[kGaussianRasterDrawInstanceCount] = index + 1u;
    }
}
