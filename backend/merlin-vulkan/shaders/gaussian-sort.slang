#include "../../../core/merlin-render-backend/shaders/gaussian-sort-abi.slang"

[[vk::push_constant]]
ConstantBuffer<GaussianSortConstants> gaussian_sort_constants;

[[vk::binding(0, 0)]] RWStructuredBuffer<GaussianSortElement> gaussian_sort_source;
[[vk::binding(1, 0)]] RWStructuredBuffer<GaussianSortElement> gaussian_sort_destination;
// Verification words, per-resource visible counts, then the digit-major
// histogram and its scan levels.
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> gaussian_sort_scan;
[[vk::binding(3, 0)]] StructuredBuffer<GaussianPreparedRecord> gaussian_sort_prepared_records;

GaussianPreparedRecord LoadSortPreparedRecord(uint index)
{
    return gaussian_sort_prepared_records[index];
}

#include "../../../core/merlin-render-backend/shaders/gaussian-sort-common.slang"
