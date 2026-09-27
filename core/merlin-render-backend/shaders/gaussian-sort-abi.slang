#pragma once

// Deterministic global sort of GPU-prepared Gaussian records. Each resident
// resource contributes one key segment; an LSD radix sort over 8-bit digits
// then orders the whole frame back to front, and a verification pass checks
// the result against the prepared records it indexes.
//
// The 64-bit key reproduces the CPU reference order exactly: the high word
// maps a larger sort key to a smaller unsigned value, and the low word is the
// frame-global candidate index, assigned by ascending resource identity and
// then particle index. Atomic compaction order therefore never reaches the
// sorted result, and the low word is unique for every real record.
//
// Every kernel uses one portable reduce-then-scan formulation without
// subgroup operations or forward-progress assumptions, and binds at most four
// storage buffers, the Vulkan guaranteed minimum.

static const uint kGaussianSortWorkgroupSize = 256u;
static const uint kGaussianSortRadixBins = 256u;
static const uint kGaussianSortScanElementsPerThread = 4u;
static const uint kGaussianSortScanWorkgroupElements = 1024u;
static const uint kGaussianSortInvalidValue = 0xFFFFFFFFu;
static const uint kGaussianSortDynamicCount = 1u;

// Verification words at the start of the scan/control buffer.
static const uint kGaussianSortSortedCount = 0u;
static const uint kGaussianSortOrderViolationCount = 1u;
static const uint kGaussianSortKeyMismatchCount = 2u;
static const uint kGaussianSortIdentityChecksum = 3u;

struct GaussianSortConstants
{
    // Keys: segment length. Histogram, scatter, and verify: padded key count,
    // or the buffer capacity when flags selects a dynamic count.
    uint element_count;
    uint block_count;
    uint digit_shift;
    uint digit_word;
    uint scan_offset;
    uint scan_count;
    uint scan_sums_offset;
    uint candidate_base;
    uint prepared_base;
    uint visible_count_offset;
    // Histogram and scatter: with kGaussianSortDynamicCount, only the first
    // min(element_count, scan[count_word]) elements participate.
    uint count_word;
    uint flags;
};

struct GaussianSortElement
{
    uint key_low;
    uint key_high;
    uint value;
};

#include "gaussian-prepared-record.slang"
