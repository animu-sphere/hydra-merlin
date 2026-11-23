#pragma once

static const float kGaussianContributionGuard = 1.0e-4f;
static const float kGaussianGuardedAlphaFloor =
    (1.0f / 255.0f) * (1.0f - kGaussianContributionGuard);

[ForceInline]
bool IsFiniteValue(float value)
{
    return !isnan(value) && !isinf(value);
}

// Lower the diagonal and raise the cross term to bound rounding in the
// raster quadratic. Positive Schur complements bound the quadratic from
// below on each axis and make center opacity an upper bound on pixel alpha.
[ForceInline]
bool GaussianContributionLowerBound(float3 conic, float radius, out float2 lower)
{
    lower = float2(0.0f);
    if (!(conic.x >= 1.0e-20f && conic.z >= 1.0e-20f) ||
        !IsFiniteValue(conic.y) ||
        !(max(max(conic.x, conic.z), abs(conic.y)) * max(1.0f, radius * radius) < 1.0e30f))
    {
        return false;
    }
    float2 diagonal = conic.xz * (1.0f - kGaussianContributionGuard);
    float cross_term = abs(conic.y) * (1.0f + kGaussianContributionGuard);
    float2 quotient = cross_term / diagonal.yx;
    lower = (diagonal - cross_term * quotient) * (1.0f - kGaussianContributionGuard);
    return lower.x > 0.0f && lower.y > 0.0f && !any(isinf(lower));
}
