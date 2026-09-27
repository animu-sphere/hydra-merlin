#include "../../../core/merlin-render-backend/shaders/gaussian-sort-abi.slang"
#include "../../../core/merlin-render-backend/shaders/gaussian-raster-abi.slang"

struct GaussianGatherConstants
{
    uint element_count;
    uint count_word;
    uint flags; // Bit 0: use the device-written count, bounded by element_count.
    uint padding;
};

[[vk::binding(4, 0)]] ConstantBuffer<GaussianGatherConstants> gaussian_gather_constants : register(b4);
[[vk::binding(0, 0)]] StructuredBuffer<GaussianSortElement> gaussian_gather_sorted : register(t0);
[[vk::binding(1, 0)]] ByteAddressBuffer gaussian_gather_prepared : register(t1);
[[vk::binding(2, 0)]] RWByteAddressBuffer gaussian_gather_instances : register(u2);
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> gaussian_gather_draw : register(u3);
[[vk::binding(5, 0)]] StructuredBuffer<uint> gaussian_gather_counts : register(t5);

// Same sentinel-boundary algorithm as Vulkan gather, with Metal's scalar
// raster layout. Dispatch at least one group, including for an empty frame.
// Every indirect argument is written on-device; no host reset is necessary.
[shader("compute")]
[numthreads(256, 1, 1)]
void gaussian_metal_gather(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
    uint index = dispatch_thread_id.x;
    uint count = gaussian_gather_constants.element_count;
    if ((gaussian_gather_constants.flags & 1u) != 0u)
        count = min(count, gaussian_gather_counts[gaussian_gather_constants.count_word]);
    if (index == 0u)
    {
        // MTLDrawPrimitivesIndirectArguments: vertexCount, instanceCount,
        // vertexStart, baseInstance. Only the empty case writes count here;
        // the last valid element is its sole writer in a nonempty frame.
        gaussian_gather_draw[0] = 6u;
        gaussian_gather_draw[2] = 0u;
        gaussian_gather_draw[3] = 0u;
        if (count == 0u)
            gaussian_gather_draw[1] = 0u;
        else if (gaussian_gather_sorted[0].value == kGaussianSortInvalidValue)
            gaussian_gather_draw[1] = 0u;
    }
    if (index >= count)
        return;
    uint value = gaussian_gather_sorted[index].value;
    if (value == kGaussianSortInvalidValue)
        return;

    uint source = value * 64u;
    uint target = index * MERLIN_GAUSSIAN_STRIDE;
    gaussian_gather_instances.Store2(target + MERLIN_GAUSSIAN_CENTER,
        gaussian_gather_prepared.Load2(source));
    gaussian_gather_instances.Store3(target + MERLIN_GAUSSIAN_CONIC,
        gaussian_gather_prepared.Load3(source + 16u));
    gaussian_gather_instances.Store3(target + MERLIN_GAUSSIAN_RADIANCE,
        gaussian_gather_prepared.Load3(source + 32u));
    gaussian_gather_instances.Store(target + MERLIN_GAUSSIAN_OPACITY,
        gaussian_gather_prepared.Load(source + 28u));
    gaussian_gather_instances.Store(target + MERLIN_GAUSSIAN_RADIUS,
        gaussian_gather_prepared.Load(source + 8u));
    gaussian_gather_instances.Store(target + MERLIN_GAUSSIAN_DEPTH,
        gaussian_gather_prepared.Load(source + 12u));
    // Picking uses the same low resource word as the CPU raster path.
    gaussian_gather_instances.Store(target + MERLIN_GAUSSIAN_RESOURCE,
        gaussian_gather_prepared.Load(source + 48u));
    gaussian_gather_instances.Store(target + MERLIN_GAUSSIAN_PARTICLE,
        gaussian_gather_prepared.Load(source + 56u));
    if (index + 1u == count)
        gaussian_gather_draw[1] = index + 1u;
    else if (gaussian_gather_sorted[index + 1u].value == kGaussianSortInvalidValue)
        gaussian_gather_draw[1] = index + 1u;
}
