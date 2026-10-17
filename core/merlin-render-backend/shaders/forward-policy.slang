// Shared Forward ABI. Keep resource bindings and ordinary-data layout stable;
// tests compare Slang reflection with the C++ upload structures.

static const uint kDisplayColorFlag = 1u;
static const uint kBaseColorTextureFlag = 2u;
static const uint kDirectionalLightFlag = 4u;
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
};

static const float kSurfaceDebugGeometricNormal = 1.0f;
static const float kSurfaceDebugShadingNormalBase = 2.0f;
static const float kSurfaceDebugShadingNormal = 3.0f;
static const float kSurfaceDebugFrontFacing = 4.0f;

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

struct ForwardVertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float4 display_color : COLOR0;
    [[vk::location(3)]] float2 texcoord : TEXCOORD0;
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
};

struct ForwardFragmentOutput
{
    [[vk::location(0)]] float4 color : SV_Target0;
    [[vk::location(1)]] uint prim_id : SV_Target1;
    [[vk::location(2)]] uint instance_id : SV_Target2;
};

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
// the lighting normal after the hemisphere rule.
struct SurfaceGeometry
{
    float3 geometric_normal;
    float3 shading_normal_base;
    float3 shading_normal;
    float2 texture_coordinate;
    bool front_facing;
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
    return surface;
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

[ForceInline]
float3 EvaluateSurfaceDebugView(SurfaceGeometry surface, float view)
{
    if (view == kSurfaceDebugGeometricNormal)
        return surface.geometric_normal * 0.5f + 0.5f;
    if (view == kSurfaceDebugShadingNormalBase)
        return surface.shading_normal_base * 0.5f + 0.5f;
    if (view == kSurfaceDebugShadingNormal)
        return surface.shading_normal * 0.5f + 0.5f;
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
    DrawConstants draw)
{
    SurfaceGeometry surface =
        BuildForwardSurface(input, front_facing, material, draw);
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
    float4 texture_sample)
{
    return ShadeForward(
        input, front_facing, material, texture_sample, draw_constants);
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
