#pragma once

// First GPU-driven Gaussian preparation contract. One dispatch processes one
// resident Gaussian resource directly from its four device-local attribute
// ranges, classifies every particle, and atomically compacts visible records.
// A later radix-sort/tile slice consumes the stable sort key and identity.

static const uint kGaussianProjectionPerspective = 0u;
static const uint kGaussianProjectionTangential = 1u;
static const uint kGaussianSortingZDepth = 0u;
static const uint kGaussianSortingCameraDistance = 1u;

static const uint kGaussianCandidateVisible = 0u;
static const uint kGaussianCandidateOpacityCulled = 1u;
static const uint kGaussianCandidateFrustumCulled = 2u;
static const uint kGaussianCandidateInvalidCulled = 3u;

static const float kProjectionEpsilon = 1.0e-6f;
// Matches the CPU reference: the 3DGS guard band bounds the perspective
// Jacobian of close off-axis kernels.
static const float kJacobianGuardBand = 1.3f;

struct GaussianPrepareConstants
{
    float4x4 local_to_camera;
    float4x4 projection;
    float2 viewport_size;
    float sigma_extent;
    float minimum_variance_pixels;
    uint resource_id_low;
    uint resource_id_high;
    uint particle_count;
    uint coefficients_per_particle;
    uint spherical_harmonics_degree;
    uint projection_mode;
    uint sorting_mode;
    uint padding;
};

#include "gaussian-prepared-record.slang"

struct GaussianPrepareDispatchCounters
{
    uint candidate_count;
    uint visible_count;
    uint opacity_culled_count;
    uint frustum_culled_count;
    uint invalid_culled_count;
    uint padding0;
    uint padding1;
    uint padding2;
};
