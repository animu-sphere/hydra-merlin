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

[[vk::push_constant]]
ConstantBuffer<GaussianPrepareConstants> gaussian_prepare_constants;

// Byte-addressed inputs match the existing tightly packed arena payloads:
// float3 positions, six-float covariance, float opacity, and float3 SH terms.
[[vk::binding(0, 3)]] ByteAddressBuffer gaussian_positions;
[[vk::binding(1, 3)]] ByteAddressBuffer gaussian_covariances;
[[vk::binding(2, 3)]] ByteAddressBuffer gaussian_opacities;
[[vk::binding(3, 3)]] ByteAddressBuffer gaussian_radiance;
[[vk::binding(4, 3)]] RWStructuredBuffer<uint> gaussian_candidate_results;
[[vk::binding(5, 3)]] RWStructuredBuffer<GaussianPreparedRecord> gaussian_prepared_records;
[[vk::binding(6, 3)]] RWStructuredBuffer<GaussianPrepareDispatchCounters> gaussian_prepare_counters;

bool IsFinite1(float value)
{
    return !isnan(value) && !isinf(value);
}

bool IsFinite2(float2 value)
{
    return all(!isnan(value)) && all(!isinf(value));
}

bool IsFinite3(float3 value)
{
    return all(!isnan(value)) && all(!isinf(value));
}

float3 SafeNormalize(float3 value)
{
    float length_squared = dot(value, value);
    if (!IsFinite1(length_squared) || length_squared <= kProjectionEpsilon * kProjectionEpsilon)
    {
        return float3(0.0f);
    }
    return value * rsqrt(length_squared);
}

float3 LoadPosition(uint particle)
{
    return gaussian_positions.Load<float3>(particle * 12u);
}

float3x3 LoadCovariance(uint particle)
{
    uint offset = particle * 24u;
    float3 first = gaussian_covariances.Load<float3>(offset);
    float3 second = gaussian_covariances.Load<float3>(offset + 12u);
    return float3x3(
        first.x, first.y, first.z,
        first.y, second.x, second.y,
        first.z, second.y, second.z);
}

float3 LoadCoefficient(uint index)
{
    return gaussian_radiance.Load<float3>(index * 12u);
}

void AddScaled(inout float3 result, uint coefficient, float basis)
{
    result += LoadCoefficient(coefficient) * basis;
}

float3 EvaluateRadiance(uint particle, float3 direction)
{
    static const float c0 = 0.28209479177387814f;
    static const float c1 = 0.4886025119029199f;
    uint first = particle * gaussian_prepare_constants.coefficients_per_particle;
    float3 result = float3(0.5f) + LoadCoefficient(first) * c0;
    uint degree = min(gaussian_prepare_constants.spherical_harmonics_degree, 3u);
    if (degree >= 1u)
    {
        AddScaled(result, first + 1u, -c1 * direction.y);
        AddScaled(result, first + 2u, c1 * direction.z);
        AddScaled(result, first + 3u, -c1 * direction.x);
    }
    float xx = direction.x * direction.x;
    float yy = direction.y * direction.y;
    float zz = direction.z * direction.z;
    if (degree >= 2u)
    {
        AddScaled(result, first + 4u, 1.0925484305920792f * direction.x * direction.y);
        AddScaled(result, first + 5u, 1.0925484305920792f * direction.y * direction.z);
        AddScaled(result, first + 6u, 0.31539156525252005f * (2.0f * zz - xx - yy));
        AddScaled(result, first + 7u, 1.0925484305920792f * direction.x * direction.z);
        AddScaled(result, first + 8u, 0.5462742152960396f * (xx - yy));
    }
    if (degree >= 3u)
    {
        AddScaled(result, first + 9u, -0.5900435899266435f * direction.y * (3.0f * xx - yy));
        AddScaled(result, first + 10u, 2.890611442640554f * direction.x * direction.y * direction.z);
        AddScaled(result, first + 11u, -0.4570457994644658f * direction.y * (4.0f * zz - xx - yy));
        AddScaled(result, first + 12u, 0.3731763325901154f * direction.z * (2.0f * zz - 3.0f * xx - 3.0f * yy));
        AddScaled(result, first + 13u, -0.4570457994644658f * direction.x * (4.0f * zz - xx - yy));
        AddScaled(result, first + 14u, 1.445305721320277f * direction.z * (xx - yy));
        AddScaled(result, first + 15u, -0.5900435899266435f * direction.x * (xx - 3.0f * yy));
    }
    return max(result, float3(0.0f));
}

struct GaussianProjectionJacobian
{
    float3 dx;
    float3 dy;
};

GaussianProjectionJacobian ProjectionJacobian(float3 camera, float4 clip)
{
    float3 row_x = gaussian_prepare_constants.projection[0].xyz;
    float3 row_y = gaussian_prepare_constants.projection[1].xyz;
    float3 row_w = gaussian_prepare_constants.projection[3].xyz;
    float inverse_w_squared = 1.0f / (clip.w * clip.w);
    float3 dx = (row_x * clip.w - clip.x * row_w) * inverse_w_squared;
    float3 dy = (row_y * clip.w - clip.y * row_w) * inverse_w_squared;

    if (gaussian_prepare_constants.projection_mode == kGaussianProjectionTangential &&
        dot(row_w, row_w) > kProjectionEpsilon * kProjectionEpsilon)
    {
        float distance = length(camera);
        float3 direction = SafeNormalize(camera);
        float3 tangent_x = SafeNormalize(float3(
            1.0f - direction.x * direction.x,
            -direction.x * direction.y,
            -direction.x * direction.z));
        if (dot(tangent_x, tangent_x) <= kProjectionEpsilon * kProjectionEpsilon)
        {
            tangent_x = SafeNormalize(float3(
                -direction.y * direction.x,
                1.0f - direction.y * direction.y,
                -direction.y * direction.z));
        }
        float3 tangent_y = SafeNormalize(cross(tangent_x, direction));
        if (tangent_y.y < 0.0f)
        {
            tangent_y = -tangent_y;
        }
        dx = tangent_x * (max(abs(gaussian_prepare_constants.projection[0][0]), kProjectionEpsilon) / distance);
        dy = tangent_y * (max(abs(gaussian_prepare_constants.projection[1][1]), kProjectionEpsilon) / distance);
    }
    GaussianProjectionJacobian result;
    result.dx = dx;
    result.dy = dy;
    return result;
}

bool ProjectCovariance(float3x3 covariance, GaussianProjectionJacobian jacobian,
                       out float3 inverse_conic, out float radius_pixels)
{
    inverse_conic = float3(0.0f);
    radius_pixels = 0.0f;
    float3 dx = jacobian.dx;
    float3 dy = jacobian.dy;
    float width_scale = gaussian_prepare_constants.viewport_size.x * 0.5f;
    float height_scale = gaussian_prepare_constants.viewport_size.y * 0.5f;
    float xx = dot(dx, mul(covariance, dx)) * width_scale * width_scale +
        gaussian_prepare_constants.minimum_variance_pixels;
    float xy = dot(dx, mul(covariance, dy)) * width_scale * height_scale;
    float yy = dot(dy, mul(covariance, dy)) * height_scale * height_scale +
        gaussian_prepare_constants.minimum_variance_pixels;
    float determinant = xx * yy - xy * xy;
    if (!IsFinite1(xx) || !IsFinite1(xy) || !IsFinite1(yy) ||
        !IsFinite1(determinant) || determinant <= kProjectionEpsilon)
    {
        return false;
    }
    float discriminant = sqrt(max(0.0f, (xx - yy) * (xx - yy) + 4.0f * xy * xy));
    float largest_eigenvalue = 0.5f * (xx + yy + discriminant);
    radius_pixels = gaussian_prepare_constants.sigma_extent * sqrt(max(largest_eigenvalue, 0.0f));
    inverse_conic = float3(yy / determinant, -xy / determinant, xx / determinant);
    return IsFinite1(radius_pixels) && radius_pixels > 0.0f && IsFinite3(inverse_conic);
}

bool ProjectDepthExtent(float3x3 covariance, float3 camera, float4 clip,
                        out float radius)
{
    radius = 0.0f;
    float3 row_z = gaussian_prepare_constants.projection[2].xyz;
    float3 row_w = gaussian_prepare_constants.projection[3].xyz;
    float inverse_w_squared = 1.0f / (clip.w * clip.w);
    float3 dz = (row_z * clip.w - clip.z * row_w) * inverse_w_squared;
    float variance = dot(dz, mul(covariance, dz));
    if (!IsFinite1(variance) || variance < -kProjectionEpsilon)
    {
        return false;
    }
    radius = gaussian_prepare_constants.sigma_extent * sqrt(max(variance, 0.0f));
    return IsFinite1(radius);
}

float3 LocalCameraDirection(float3x3 local_to_camera, float3 camera)
{
    float3 row0 = local_to_camera[0];
    float3 row1 = local_to_camera[1];
    float3 row2 = local_to_camera[2];
    float3 cofactor0 = cross(row1, row2);
    float3 cofactor1 = cross(row2, row0);
    float3 cofactor2 = cross(row0, row1);
    float determinant_value = dot(row0, cofactor0);
    if (!IsFinite1(determinant_value) || abs(determinant_value) <= kProjectionEpsilon)
    {
        return SafeNormalize(camera);
    }
    float inverse_determinant = 1.0f / determinant_value;
    return SafeNormalize(float3(
        dot(cofactor0, camera),
        dot(cofactor1, camera),
        dot(cofactor2, camera)) * inverse_determinant);
}

void RecordRejection(uint particle, uint result, inout uint counter)
{
    gaussian_candidate_results[particle] = result;
    uint ignored;
    InterlockedAdd(counter, 1u, ignored);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void gaussian_prepare_compact(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
    uint particle = dispatch_thread_id.x;
    if (particle >= gaussian_prepare_constants.particle_count)
    {
        return;
    }
    if (particle == 0u)
    {
        gaussian_prepare_counters[0].candidate_count = gaussian_prepare_constants.particle_count;
    }

    float opacity = gaussian_opacities.Load<float>(particle * 4u);
    if (!IsFinite1(opacity) || opacity <= 0.0f)
    {
        RecordRejection(particle, kGaussianCandidateOpacityCulled,
                        gaussian_prepare_counters[0].opacity_culled_count);
        return;
    }

    float3 local_position = LoadPosition(particle);
    float4 camera4 = mul(gaussian_prepare_constants.local_to_camera, float4(local_position, 1.0f));
    float3 camera = camera4.xyz;
    float4 clip = mul(gaussian_prepare_constants.projection, float4(camera, 1.0f));
    if (!IsFinite3(camera) || any(isnan(clip)) || any(isinf(clip)) || clip.w <= kProjectionEpsilon)
    {
        RecordRejection(particle, kGaussianCandidateFrustumCulled,
                        gaussian_prepare_counters[0].frustum_culled_count);
        return;
    }
    float3 ndc = clip.xyz / clip.w;
    float3x3 local_linear = (float3x3)gaussian_prepare_constants.local_to_camera;
    float3x3 covariance = LoadCovariance(particle);
    float3x3 camera_covariance = mul(mul(local_linear, covariance), transpose(local_linear));
    float depth_radius;
    if (!ProjectDepthExtent(camera_covariance, camera, clip, depth_radius))
    {
        RecordRejection(particle, kGaussianCandidateInvalidCulled,
                        gaussian_prepare_counters[0].invalid_culled_count);
        return;
    }
    if (ndc.z + depth_radius < 0.0f || ndc.z - depth_radius > 1.0f)
    {
        RecordRejection(particle, kGaussianCandidateFrustumCulled,
                        gaussian_prepare_counters[0].frustum_culled_count);
        return;
    }
    float3 inverse_conic;
    float radius_pixels;
    if (!ProjectCovariance(camera_covariance, ProjectionJacobian(camera, clip),
                           inverse_conic, radius_pixels))
    {
        RecordRejection(particle, kGaussianCandidateInvalidCulled,
                        gaussian_prepare_counters[0].invalid_culled_count);
        return;
    }

    float2 center_pixels = (ndc.xy * 0.5f + 0.5f) * gaussian_prepare_constants.viewport_size;
    if (!IsFinite2(center_pixels) ||
        center_pixels.x + radius_pixels < 0.0f ||
        center_pixels.x - radius_pixels > gaussian_prepare_constants.viewport_size.x ||
        center_pixels.y + radius_pixels < 0.0f ||
        center_pixels.y - radius_pixels > gaussian_prepare_constants.viewport_size.y)
    {
        RecordRejection(particle, kGaussianCandidateFrustumCulled,
                        gaussian_prepare_counters[0].frustum_culled_count);
        return;
    }

    uint compact_index;
    InterlockedAdd(gaussian_prepare_counters[0].visible_count, 1u, compact_index);
    GaussianPreparedRecord prepared;
    prepared.center_pixels = center_pixels;
    prepared.radius_pixels = radius_pixels;
    prepared.depth = clamp(ndc.z, 0.0f, 1.0f);
    prepared.inverse_conic = inverse_conic;
    prepared.opacity = opacity;
    prepared.radiance = EvaluateRadiance(
        particle, LocalCameraDirection(local_linear, camera));
    prepared.sort_key = gaussian_prepare_constants.sorting_mode == kGaussianSortingCameraDistance
        ? dot(camera, camera) : ndc.z;
    prepared.resource_id_low = gaussian_prepare_constants.resource_id_low;
    prepared.resource_id_high = gaussian_prepare_constants.resource_id_high;
    prepared.particle_id = particle;
    prepared.padding = 0u;
    gaussian_prepared_records[compact_index] = prepared;
    gaussian_candidate_results[particle] = kGaussianCandidateVisible;
}
