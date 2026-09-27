#pragma once

// Kept at 64 bytes so it can become the direct input of sorted/tiled raster
// without repacking. Resource + particle identity is a deterministic tie break.
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
