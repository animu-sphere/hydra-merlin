#pragma once

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
static const uint kGaussianTileRasterSelected = 10u;
static const uint kGaussianTileRejectedPairCount = 11u;
static const uint kGaussianTileCulledRecordCount = 12u;
static const uint kGaussianTileSaturatedCount = 13u;
static const uint kGaussianTileEarlyTerminatedCount = 14u;

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
    uint adaptive_bounds;
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

#include "gaussian-prepared-record.slang"

struct GaussianTileRect
{
    uint x0;
    uint y0;
    uint x1;
    uint y1;
};
