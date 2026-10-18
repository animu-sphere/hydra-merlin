// Shared Forward ABI. Keep resource bindings and ordinary-data layout stable;
// tests compare Slang reflection with the C++ upload structures.

static const uint kDisplayColorFlag = 1u;
static const uint kBaseColorTextureFlag = 2u;
static const uint kDirectionalLightFlag = 4u;
static const uint kNormalTextureFlag = 8u;
static const uint kMaskedAlphaFlag = 0x10000000u;
static const uint kSamplerIndexShift = 8u;
static const uint kSamplerIndexMask = 0xffffu;

struct DrawConstants
{
    float4x4 model_view_projection;
    float4 normal_matrix_column0;
    float4 normal_matrix_column1;
    float4 normal_matrix_column2;
    uint feature_mask;
    uint prim_id;
    uint instance_id;
    uint texture_index;
#ifdef MERLIN_SEPARATE_SAMPLER_INDEX
    uint sampler_index;
    uint padding[3];
#endif
};

struct MaterialConstants
{
    float4 base_color;
    float4 light_direction_intensity;
    float4 light_color_alpha_cutoff;
    float4 diffuse_environment[9];
    float4 output_transform;
    // x: +/-1 derivative geometric-normal orientation, y: surface debug view,
    // zw: reserved. See merlin::render::PackForwardSurfacePolicy.
    float4 surface_policy;
    // x: normal-map xy scale, y: green-channel sign (+1 +Y up, -1 flipped),
    // zw: reserved.
    float4 normal_map;
    // x/y: normal texture and sampler indices in the bindless or argument
    // tables of paths that bind them by index; zw: reserved.
    uint4 normal_map_resource;
};

static const float kSurfaceDebugGeometricNormal = 1.0f;
static const float kSurfaceDebugShadingNormalBase = 2.0f;
static const float kSurfaceDebugShadingNormal = 3.0f;
static const float kSurfaceDebugFrontFacing = 4.0f;
static const float kSurfaceDebugTangent = 5.0f;
static const float kSurfaceDebugBitangent = 6.0f;

#ifndef MERLIN_EXPLICIT_DRAW_CONSTANTS
[[vk::push_constant]]
ConstantBuffer<DrawConstants> draw_constants;
#endif

// draw_constants.texture_index and the encoded sampler index are only
// populated when this is set, so resource reads must stay behind it.
#ifndef MERLIN_EXPLICIT_DRAW_CONSTANTS
[ForceInline]
bool HasBaseColorTexture()
{
    return (draw_constants.feature_mask & kBaseColorTextureFlag) != 0u;
}
#endif

[ForceInline]
bool HasBaseColorTexture(DrawConstants draw)
{
    return (draw.feature_mask & kBaseColorTextureFlag) != 0u;
}

// Set only when the material has a normal texture and the geometry has
// texture coordinate set 0; normal-texture reads must stay behind it.
[ForceInline]
bool HasNormalTexture(DrawConstants draw)
{
    return (draw.feature_mask & kNormalTextureFlag) != 0u;
}

#ifndef MERLIN_EXPLICIT_DRAW_CONSTANTS
[ForceInline]
bool HasNormalTexture()
{
    return HasNormalTexture(draw_constants);
}
#endif

struct ForwardVertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float4 display_color : COLOR0;
    [[vk::location(3)]] float2 texcoord : TEXCOORD0;
    // xyz tangent, w handedness; zero when the vertex has no frame.
    [[vk::location(4)]] float4 tangent : TANGENT;
};

struct ForwardVertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float3 shading_normal : TEXCOORD0;
    [[vk::location(2)]] float2 texture_coordinate : TEXCOORD1;
    // Object-space position; its screen derivatives give Ng without a world
    // matrix in the draw constants.
    [[vk::location(3)]] float3 object_position : TEXCOORD2;
    // World tangent and handedness; zero when the vertex has no frame.
    [[vk::location(4)]] float4 tangent : TEXCOORD3;
};

struct ForwardFragmentOutput
{
    [[vk::location(0)]] float4 color : SV_Target0;
    [[vk::location(1)]] uint prim_id : SV_Target1;
    [[vk::location(2)]] uint instance_id : SV_Target2;
};

// Sign of det(M); the inverse-transpose normal matrix shares it.
[ForceInline]
float ForwardTransformHandedness(DrawConstants draw)
{
    return dot(draw.normal_matrix_column0.xyz,
               cross(draw.normal_matrix_column1.xyz,
                   draw.normal_matrix_column2.xyz)) < 0.0f
        ? -1.0f
        : 1.0f;
}

// Surface directions transform with M rather than its inverse-transpose N.
// cof(N) = det(N) * M, and det(N) has the sign of det(M), so the cofactor
// times that sign maps a direction by M up to a positive scale.
[ForceInline]
float3 TransformForwardDirection(float3 direction, DrawConstants draw)
{
    float3 column0 = draw.normal_matrix_column0.xyz;
    float3 column1 = draw.normal_matrix_column1.xyz;
    float3 column2 = draw.normal_matrix_column2.xyz;
    return (cross(column1, column2) * direction.x +
               cross(column2, column0) * direction.y +
               cross(column0, column1) * direction.z) *
        ForwardTransformHandedness(draw);
}

// A mirrored transform reverses the bitangent sense of cross(N, T), so the
// handedness carries det(M) to keep the authored frame.
[ForceInline]
float4 TransformForwardTangent(float4 tangent, DrawConstants draw)
{
    return float4(TransformForwardDirection(tangent.xyz, draw),
        tangent.w * ForwardTransformHandedness(draw));
}

[ForceInline]
ForwardVertexOutput EvaluateForwardVertex(
    ForwardVertexInput input,
    MaterialConstants material,
    DrawConstants draw)
{
    ForwardVertexOutput output;
    output.position = mul(
        draw.model_view_projection,
        float4(input.position, 1.0f));
    output.color = material.base_color;
    if ((draw.feature_mask & kDisplayColorFlag) != 0u)
    {
        output.color *= input.display_color;
    }
    output.shading_normal =
        draw.normal_matrix_column0.xyz * input.normal.x +
        draw.normal_matrix_column1.xyz * input.normal.y +
        draw.normal_matrix_column2.xyz * input.normal.z;
    output.texture_coordinate = input.texcoord;
    output.object_position = input.position;
    output.tangent = TransformForwardTangent(input.tangent, draw);
    return output;
}

#ifndef MERLIN_EXPLICIT_DRAW_CONSTANTS
[ForceInline]
ForwardVertexOutput EvaluateForwardVertex(
    ForwardVertexInput input,
    MaterialConstants material)
{
    return EvaluateForwardVertex(input, material, draw_constants);
}
#endif

// Shared surface context of the surface geometry contract. Ng and Ns_base are
// already flipped toward the visible side on double-sided back faces; Ns is
// the lighting normal after the hemisphere rule. The tangent frame is
// orthonormal to Ns_base with B = cross(Ns_base, T.xyz) * T.w; on back faces
// T and its handedness are negated with Ns_base, so a normal map yields the
// negated front-side normal. tangent and bitangent are zero without a frame.
struct SurfaceGeometry
{
    float3 geometric_normal;
    float3 shading_normal_base;
    float3 shading_normal;
    float4 tangent;
    float3 bitangent;
    float2 texture_coordinate;
    bool front_facing;
    bool has_tangent_frame;
};

[ForceInline]
float3 SafeNormalize(float3 value, float3 fallback)
{
    float length_squared = dot(value, value);
    return length_squared > 1.0e-30f && length_squared < 1.0e30f
        ? value * rsqrt(length_squared)
        : fallback;
}

// Must run in uniform control flow: Ng uses screen-space derivatives.
[ForceInline]
SurfaceGeometry BuildSurfaceGeometry(
    ForwardVertexOutput input,
    bool front_facing,
    MaterialConstants material,
    DrawConstants draw)
{
    float3 column0 = draw.normal_matrix_column0.xyz;
    float3 column1 = draw.normal_matrix_column1.xyz;
    float3 column2 = draw.normal_matrix_column2.xyz;
    // cross(ddx(p), ddy(p)) is the oriented triangle normal over its signed
    // framebuffer area. The inverse-transpose maps it to world space up to
    // det(M), and the per-frame sign fixes the camera winding/NDC direction,
    // which orients Ng toward the visible side for front and back faces.
    float3 object_derivative_normal = cross(
        ddx(input.object_position), ddy(input.object_position));
    float3 world_derivative_normal = column0 * object_derivative_normal.x +
        column1 * object_derivative_normal.y +
        column2 * object_derivative_normal.z;
    float handedness = dot(column0, cross(column1, column2)) < 0.0f
        ? -1.0f
        : 1.0f;
    float3 interpolated = input.shading_normal;
    SurfaceGeometry surface;
    surface.front_facing = front_facing;
    surface.texture_coordinate = input.texture_coordinate;
    // Back faces of double-sided surfaces flip Ns_base before any later
    // perturbation; single-sided back faces are culled.
    float facing = front_facing ? 1.0f : -1.0f;
    float3 fallback = SafeNormalize(interpolated * facing,
        float3(0.0f, 0.0f, 1.0f));
    surface.geometric_normal = SafeNormalize(
        world_derivative_normal *
            (material.surface_policy.x * handedness),
        fallback);
    surface.shading_normal_base = SafeNormalize(
        interpolated * facing, surface.geometric_normal);
    surface.shading_normal = surface.shading_normal_base;

    // Tangent priority: the interpolated vertex tangent (authored or
    // generated), then the screen-space derivative frame of position and
    // texture coordinate, else no frame. A vertex frame is used only when
    // every corner of the triangle carries the same handedness, which keeps
    // the interpolated |w| at 1; a corner without a frame or of opposite
    // handedness selects the derivative frame for the triangle. Derivatives
    // are taken here, in uniform control flow, whether or not a normal map is
    // bound.
    float2 uv_dx = ddx(input.texture_coordinate);
    float2 uv_dy = ddy(input.texture_coordinate);
    float uv_area = uv_dx.x * uv_dy.y - uv_dy.x * uv_dx.y;
    float uv_sign = uv_area < 0.0f ? -1.0f : 1.0f;
    float3 position_dx = ddx(input.object_position);
    float3 position_dy = ddy(input.object_position);
    float3 derivative_tangent = TransformForwardDirection(
        (position_dx * uv_dy.y - position_dy * uv_dx.y) * uv_sign, draw);
    float3 derivative_bitangent = TransformForwardDirection(
        (position_dy * uv_dx.x - position_dx * uv_dy.x) * uv_sign, draw);
    // Handedness is defined on the front side and negated below with Ns_base.
    float3 front_normal = surface.shading_normal_base * facing;
    float3 tangent = float3(0.0f, 0.0f, 0.0f);
    float tangent_handedness = 0.0f;
    if (abs(input.tangent.w) > 0.999f)
    {
        tangent = input.tangent.xyz;
        tangent_handedness = input.tangent.w < 0.0f ? -1.0f : 1.0f;
    }
    else if (abs(uv_area) > 1.0e-20f)
    {
        tangent = derivative_tangent;
        tangent_handedness = dot(cross(front_normal, derivative_tangent),
                         derivative_bitangent) < 0.0f
            ? -1.0f
            : 1.0f;
    }
    // Gram-Schmidt against Ns_base; a tangent parallel to it has no frame.
    float3 orthogonal = tangent - front_normal * dot(front_normal, tangent);
    float orthogonal_squared = dot(orthogonal, orthogonal);
    surface.has_tangent_frame = tangent_handedness != 0.0f &&
        orthogonal_squared > 1.0e-12f * max(dot(tangent, tangent), 1.0e-30f) &&
        orthogonal_squared < 1.0e30f;
    surface.tangent = float4(0.0f, 0.0f, 0.0f, 0.0f);
    surface.bitangent = float3(0.0f, 0.0f, 0.0f);
    if (surface.has_tangent_frame)
    {
        float3 unit_tangent = orthogonal * rsqrt(orthogonal_squared) * facing;
        float frame_sign = tangent_handedness * facing;
        surface.tangent = float4(unit_tangent, frame_sign);
        surface.bitangent =
            cross(surface.shading_normal_base, unit_tangent) * frame_sign;
    }
    return surface;
}

// Linear, non-color decode of a tangent-space normal texel: n = 2t - 1, the
// green channel signed by the material convention, xy scaled.
[ForceInline]
float3 DecodeForwardNormalTexel(float4 texel, MaterialConstants material)
{
    float3 normal = texel.xyz * 2.0f - 1.0f;
    normal.y *= material.normal_map.y;
    normal.xy *= material.normal_map.x;
    return SafeNormalize(normal, float3(0.0f, 0.0f, 1.0f));
}

// Applies a tangent-space normal through the surface frame. Without a frame
// normal mapping is disabled and Ns_base is kept.
[ForceInline]
float3 ApplyTangentSpaceNormal(SurfaceGeometry surface, float3 normal)
{
    if (!surface.has_tangent_frame)
        return surface.shading_normal_base;
    return SafeNormalize(normal.x * surface.tangent.xyz +
            normal.y * surface.bitangent +
            normal.z * surface.shading_normal_base,
        surface.shading_normal_base);
}

// Facing and hemisphere rules for any candidate shading normal, including a
// world-space normal returned by a material: a normal pointing behind Ng is
// bent back to the Ng hemisphere instead of lighting the surface from behind.
[ForceInline]
float3 ResolveShadingNormal(SurfaceGeometry surface, float3 candidate)
{
    float3 normal = SafeNormalize(candidate, surface.shading_normal_base);
    float alignment = dot(normal, surface.geometric_normal);
    if (alignment < 0.0f)
    {
        normal = SafeNormalize(
            normal - (alignment - 1.0e-3f) * surface.geometric_normal,
            surface.geometric_normal);
    }
    return normal;
}

[ForceInline]
SurfaceGeometry BuildForwardSurface(
    ForwardVertexOutput input,
    bool front_facing,
    MaterialConstants material,
    DrawConstants draw)
{
    SurfaceGeometry surface =
        BuildSurfaceGeometry(input, front_facing, material, draw);
    surface.shading_normal =
        ResolveShadingNormal(surface, surface.shading_normal_base);
    return surface;
}

// The base surface plus the material's tangent-space normal map, after the
// facing and hemisphere rules.
[ForceInline]
SurfaceGeometry BuildForwardSurface(
    ForwardVertexOutput input,
    bool front_facing,
    MaterialConstants material,
    float4 normal_sample,
    DrawConstants draw)
{
    SurfaceGeometry surface =
        BuildSurfaceGeometry(input, front_facing, material, draw);
    float3 candidate = surface.shading_normal_base;
    if (HasNormalTexture(draw))
    {
        candidate = ApplyTangentSpaceNormal(
            surface, DecodeForwardNormalTexel(normal_sample, material));
    }
    surface.shading_normal = ResolveShadingNormal(surface, candidate);
    return surface;
}

[ForceInline]
float3 EvaluateSurfaceDebugView(SurfaceGeometry surface, float view)
{
    if (view == kSurfaceDebugGeometricNormal)
        return surface.geometric_normal * 0.5f + 0.5f;
    if (view == kSurfaceDebugShadingNormalBase)
        return surface.shading_normal_base * 0.5f + 0.5f;
    if (view == kSurfaceDebugShadingNormal)
        return surface.shading_normal * 0.5f + 0.5f;
    if (view == kSurfaceDebugTangent)
        return surface.tangent.xyz * 0.5f + 0.5f;
    if (view == kSurfaceDebugBitangent)
        return surface.bitangent * 0.5f + 0.5f;
    return surface.front_facing ? float3(1.0f, 1.0f, 1.0f)
                                : float3(0.0f, 0.0f, 0.0f);
}

[ForceInline]
float4 ShadeForwardSurface(
    SurfaceGeometry surface,
    MaterialConstants material,
    float4 base_color,
    DrawConstants draw)
{
    float4 shaded = base_color;
    if (material.surface_policy.y != 0.0f)
    {
        // Unlit diagnostic RGB; alpha keeps coverage and masking unchanged.
        shaded.rgb = EvaluateSurfaceDebugView(
            surface, material.surface_policy.y);
    }
    else if ((draw.feature_mask & kDirectionalLightFlag) != 0u)
    {
        float3 normal = surface.shading_normal;
        float x = normal.x;
        float y = normal.y;
        float z = normal.z;
        float3 light_scale =
            material.diffuse_environment[0].rgb * 0.2820947918f +
            material.diffuse_environment[1].rgb * (0.4886025119f * y) +
            material.diffuse_environment[2].rgb * (0.4886025119f * z) +
            material.diffuse_environment[3].rgb * (0.4886025119f * x) +
            material.diffuse_environment[4].rgb *
                (1.0925484306f * x * y) +
            material.diffuse_environment[5].rgb *
                (1.0925484306f * y * z) +
            material.diffuse_environment[6].rgb *
                (0.3153915653f * (3.0f * z * z - 1.0f)) +
            material.diffuse_environment[7].rgb *
                (1.0925484306f * x * z) +
            material.diffuse_environment[8].rgb *
                (0.5462742153f * (x * x - y * y));
        // Treat the packaged environment as ambient fill rather than adding
        // its full HDR irradiance on top of usdview's camera light. Keeping
        // the shared 0.15/0.85 split prevents camera-facing
        // albedo from clipping to white in an UNorm color target.
        light_scale =
            0.15f * max(light_scale, float3(0.0f, 0.0f, 0.0f));
        float n_dot_l = max(
            dot(normal,
                normalize(material.light_direction_intensity.xyz)),
            0.0f);
        light_scale += material.light_color_alpha_cutoff.rgb *
            (0.85f * n_dot_l * material.light_direction_intensity.w);
        shaded.rgb *= light_scale;
        // RGB remains linear. Exposure is 2^EV; Reinhard divides by inverse
        // exposure to avoid overflowing finite HDR inputs at positive EV.
        // Presentation alone applies display encoding. Alpha is unchanged.
        if (material.output_transform.y == 1.0f)
        {
            shaded.rgb = shaded.rgb / (material.output_transform.z + shaded.rgb);
        }
        else
        {
            shaded.rgb *= material.output_transform.x;
        }
    }
    if ((draw.feature_mask & kMaskedAlphaFlag) != 0u &&
        shaded.a < material.light_color_alpha_cutoff.a)
    {
        discard;
    }
    return shaded;
}

[ForceInline]
float4 ShadeForward(
    ForwardVertexOutput input,
    bool front_facing,
    MaterialConstants material,
    float4 texture_sample,
    float4 normal_sample,
    DrawConstants draw)
{
    SurfaceGeometry surface = BuildForwardSurface(
        input, front_facing, material, normal_sample, draw);
    float4 base_color = input.color;
    if (HasBaseColorTexture(draw))
    {
        base_color *= texture_sample;
    }
    return ShadeForwardSurface(surface, material, base_color, draw);
}

#ifndef MERLIN_EXPLICIT_DRAW_CONSTANTS
[ForceInline]
SurfaceGeometry BuildForwardSurface(
    ForwardVertexOutput input,
    bool front_facing,
    MaterialConstants material)
{
    return BuildForwardSurface(input, front_facing, material, draw_constants);
}

[ForceInline]
float4 ShadeForward(
    ForwardVertexOutput input,
    bool front_facing,
    MaterialConstants material,
    float4 texture_sample,
    float4 normal_sample)
{
    return ShadeForward(input, front_facing, material, texture_sample,
        normal_sample, draw_constants);
}
#endif

[ForceInline]
ForwardFragmentOutput MakeForwardFragmentOutput(
    float4 color,
    DrawConstants draw)
{
    ForwardFragmentOutput output;
    output.color = saturate(color);
    output.prim_id = draw.prim_id;
    output.instance_id = draw.instance_id;
    return output;
}

#ifndef MERLIN_EXPLICIT_DRAW_CONSTANTS
[ForceInline]
ForwardFragmentOutput MakeForwardFragmentOutput(float4 color)
{
    return MakeForwardFragmentOutput(color, draw_constants);
}
#endif
