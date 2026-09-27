// Resource declarations belong to the backend wrapper.
// Inline helpers so independently compiled Metal entry points can link into
// one metallib without duplicate externally visible helper symbols.

groupshared uint gs_sort_bins[kGaussianSortRadixBins];
groupshared uint gs_sort_digits[kGaussianSortWorkgroupSize];
groupshared uint gs_sort_scan[kGaussianSortWorkgroupSize];

// Back to front: a larger sort key must produce a smaller unsigned key. The
// CPU reference compares floats, so -0 and +0 have to share one key.
[ForceInline]
uint GaussianSortKeyHigh(float sort_key)
{
    uint bits = sort_key == 0.0f ? 0u : asuint(sort_key);
    uint ordered = (bits & 0x80000000u) != 0u ? ~bits : (bits | 0x80000000u);
    return ~ordered;
}

// Order-sensitive identity mix shared with the CPU reference checksum.
[ForceInline]
uint GaussianSortMix(uint value)
{
    value ^= value >> 16u;
    value *= 0x85EBCA6Bu;
    value ^= value >> 13u;
    value *= 0xC2B2AE35u;
    value ^= value >> 16u;
    return value;
}

[ForceInline]
uint GaussianSortIdentityHash(uint resource_low, uint resource_high, uint particle)
{
    uint hash = GaussianSortMix(particle * 0x9E3779B9u ^ resource_low);
    return GaussianSortMix(hash ^ resource_high * 0x85EBCA6Bu);
}

[ForceInline]
GaussianSortElement SentinelElement()
{
    GaussianSortElement element;
    element.key_low = 0xFFFFFFFFu;
    element.key_high = 0xFFFFFFFFu;
    element.value = kGaussianSortInvalidValue;
    return element;
}

[ForceInline]
uint Digit(GaussianSortElement element)
{
    uint word = gaussian_sort_constants.digit_word == 0u
        ? element.key_low : element.key_high;
    return (word >> gaussian_sort_constants.digit_shift) & (kGaussianSortRadixBins - 1u);
}

[ForceInline]
uint ActiveElementCount()
{
    uint count = gaussian_sort_constants.element_count;
    if ((gaussian_sort_constants.flags & kGaussianSortDynamicCount) != 0u)
    {
        count = min(count, gaussian_sort_scan[gaussian_sort_constants.count_word]);
    }
    return count;
}

[ForceInline]
bool KeyLess(GaussianSortElement lhs, GaussianSortElement rhs)
{
    return lhs.key_high < rhs.key_high ||
        (lhs.key_high == rhs.key_high && lhs.key_low < rhs.key_low);
}

// One dispatch per resource. Threads past the resource's visible count, and
// the padding appended to the last segment, write sentinels that sort last.
[shader("compute")]
[numthreads(256, 1, 1)]
void gaussian_sort_keys(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
    uint index = dispatch_thread_id.x;
    if (index >= gaussian_sort_constants.element_count)
    {
        return;
    }
    GaussianSortElement element = SentinelElement();
    if (index < gaussian_sort_scan[gaussian_sort_constants.visible_count_offset])
    {
        uint record_index = gaussian_sort_constants.prepared_base + index;
        GaussianPreparedRecord record = LoadSortPreparedRecord(record_index);
        element.key_low = gaussian_sort_constants.candidate_base + record.particle_id;
        element.key_high = GaussianSortKeyHigh(record.sort_key);
        element.value = record_index;
    }
    gaussian_sort_destination[gaussian_sort_constants.candidate_base + index] = element;
}

// Per-workgroup digit counts, stored digit-major so one exclusive scan over
// the whole array yields every workgroup's global scatter base.
[shader("compute")]
[numthreads(256, 1, 1)]
void gaussian_sort_histogram(uint3 group_id : SV_GroupID,
                             uint3 group_thread_id : SV_GroupThreadID,
                             uint3 dispatch_thread_id : SV_DispatchThreadID)
{
    uint lane = group_thread_id.x;
    gs_sort_bins[lane] = 0u;
    GroupMemoryBarrierWithGroupSync();
    if (dispatch_thread_id.x < ActiveElementCount())
    {
        InterlockedAdd(gs_sort_bins[Digit(gaussian_sort_source[dispatch_thread_id.x])],
                       1u);
    }
    GroupMemoryBarrierWithGroupSync();
    gaussian_sort_scan[gaussian_sort_constants.scan_offset +
                       lane * gaussian_sort_constants.block_count + group_id.x] =
        gs_sort_bins[lane];
}

// Exclusive scan of one 1024-element block per workgroup; each block total is
// written to the next level so a later scan and add make the scan global.
[shader("compute")]
[numthreads(256, 1, 1)]
void gaussian_sort_scan_blocks(uint3 group_id : SV_GroupID,
                               uint3 group_thread_id : SV_GroupThreadID)
{
    uint lane = group_thread_id.x;
    uint first = group_id.x * kGaussianSortScanWorkgroupElements +
        lane * kGaussianSortScanElementsPerThread;
    uint values[kGaussianSortScanElementsPerThread];
    uint total = 0u;
    for (uint i = 0u; i < kGaussianSortScanElementsPerThread; ++i)
    {
        uint index = first + i;
        values[i] = index < gaussian_sort_constants.scan_count
            ? gaussian_sort_scan[gaussian_sort_constants.scan_offset + index] : 0u;
        total += values[i];
    }
    gs_sort_scan[lane] = total;
    GroupMemoryBarrierWithGroupSync();
    for (uint offset = 1u; offset < kGaussianSortWorkgroupSize; offset <<= 1u)
    {
        uint addend = lane >= offset ? gs_sort_scan[lane - offset] : 0u;
        GroupMemoryBarrierWithGroupSync();
        gs_sort_scan[lane] += addend;
        GroupMemoryBarrierWithGroupSync();
    }
    uint running = lane == 0u ? 0u : gs_sort_scan[lane - 1u];
    for (uint i = 0u; i < kGaussianSortScanElementsPerThread; ++i)
    {
        uint index = first + i;
        if (index < gaussian_sort_constants.scan_count)
        {
            gaussian_sort_scan[gaussian_sort_constants.scan_offset + index] = running;
        }
        running += values[i];
    }
    if (lane == kGaussianSortWorkgroupSize - 1u)
    {
        gaussian_sort_scan[gaussian_sort_constants.scan_sums_offset + group_id.x] =
            gs_sort_scan[lane];
    }
}

[shader("compute")]
[numthreads(256, 1, 1)]
void gaussian_sort_scan_add(uint3 group_id : SV_GroupID,
                            uint3 group_thread_id : SV_GroupThreadID)
{
    uint addend = gaussian_sort_scan[gaussian_sort_constants.scan_sums_offset + group_id.x];
    for (uint i = 0u; i < kGaussianSortScanElementsPerThread; ++i)
    {
        uint index = group_id.x * kGaussianSortScanWorkgroupElements +
            i * kGaussianSortWorkgroupSize + group_thread_id.x;
        if (index < gaussian_sort_constants.scan_count)
        {
            gaussian_sort_scan[gaussian_sort_constants.scan_offset + index] += addend;
        }
    }
}

// Stable scatter: the destination is the workgroup's global digit base plus
// the number of earlier elements in the same workgroup with the same digit.
[shader("compute")]
[numthreads(256, 1, 1)]
void gaussian_sort_scatter(uint3 group_id : SV_GroupID,
                           uint3 group_thread_id : SV_GroupThreadID,
                           uint3 dispatch_thread_id : SV_DispatchThreadID)
{
    uint lane = group_thread_id.x;
    bool active = dispatch_thread_id.x < ActiveElementCount();
    GaussianSortElement element = SentinelElement();
    uint digit = kGaussianSortRadixBins;
    if (active)
    {
        element = gaussian_sort_source[dispatch_thread_id.x];
        digit = Digit(element);
    }
    gs_sort_digits[lane] = digit;
    gs_sort_bins[lane] = gaussian_sort_scan[
        gaussian_sort_constants.scan_offset +
        lane * gaussian_sort_constants.block_count + group_id.x];
    GroupMemoryBarrierWithGroupSync();
    if (!active)
    {
        return;
    }
    uint rank = 0u;
    for (uint i = 0u; i < lane; ++i)
    {
        rank += gs_sort_digits[i] == digit ? 1u : 0u;
    }
    gaussian_sort_destination[gs_sort_bins[digit] + rank] = element;
}

// Checks strict key order, sentinel placement, and each key against the
// prepared record it indexes, and accumulates an order-sensitive identity
// checksum the CPU reference can reproduce.
[shader("compute")]
[numthreads(256, 1, 1)]
void gaussian_sort_verify(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
    uint index = dispatch_thread_id.x;
    if (index >= gaussian_sort_constants.element_count)
    {
        return;
    }
    GaussianSortElement element = gaussian_sort_source[index];
    bool valid = element.value != kGaussianSortInvalidValue;
    if (index + 1u < gaussian_sort_constants.element_count)
    {
        GaussianSortElement next = gaussian_sort_source[index + 1u];
        bool ordered = valid ? KeyLess(element, next) : !KeyLess(next, element);
        if (!ordered || (!valid && next.value != kGaussianSortInvalidValue))
        {
            InterlockedAdd(gaussian_sort_scan[kGaussianSortOrderViolationCount], 1u);
        }
    }
    if (!valid)
    {
        if (element.key_low != 0xFFFFFFFFu || element.key_high != 0xFFFFFFFFu)
        {
            InterlockedAdd(gaussian_sort_scan[kGaussianSortKeyMismatchCount], 1u);
        }
        return;
    }
    GaussianPreparedRecord record = LoadSortPreparedRecord(element.value);
    if (element.key_high != GaussianSortKeyHigh(record.sort_key) ||
        element.key_low < record.particle_id)
    {
        InterlockedAdd(gaussian_sort_scan[kGaussianSortKeyMismatchCount], 1u);
    }
    InterlockedAdd(gaussian_sort_scan[kGaussianSortSortedCount], 1u);
    InterlockedAdd(gaussian_sort_scan[kGaussianSortIdentityChecksum],
                   (index + 1u) * GaussianSortIdentityHash(
                       record.resource_id_low, record.resource_id_high,
                       record.particle_id));
}
