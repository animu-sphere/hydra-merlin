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
};

struct MaterialConstants
{
    float4 base_color;
    float4 light_direction_intensity;
    float4 light_color_alpha_cutoff;
};

[[vk::push_constant]]
ConstantBuffer<DrawConstants> draw_constants;

// draw_constants.texture_index and the encoded sampler index are only
// populated when this is set, so resource reads must stay behind it.
bool HasBaseColorTexture()
{
    return (draw_constants.feature_mask & kBaseColorTextureFlag) != 0u;
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
};

struct ForwardFragmentOutput
{
    [[vk::location(0)]] float4 color : SV_Target0;
    [[vk::location(1)]] uint prim_id : SV_Target1;
    [[vk::location(2)]] uint instance_id : SV_Target2;
};

ForwardVertexOutput EvaluateForwardVertex(
    ForwardVertexInput input,
    MaterialConstants material)
{
    ForwardVertexOutput output;
    output.position = mul(
        draw_constants.model_view_projection,
        float4(input.position, 1.0f));
    output.color = material.base_color;
    if ((draw_constants.feature_mask & kDisplayColorFlag) != 0u)
    {
        output.color *= input.display_color;
    }
    output.shading_normal =
        draw_constants.normal_matrix_column0.xyz * input.normal.x +
        draw_constants.normal_matrix_column1.xyz * input.normal.y +
        draw_constants.normal_matrix_column2.xyz * input.normal.z;
    output.texture_coordinate = input.texcoord;
    return output;
}

float4 ShadeForward(
    ForwardVertexOutput input,
    MaterialConstants material,
    float4 texture_sample)
{
    float4 shaded = input.color;
    if (HasBaseColorTexture())
    {
        shaded *= texture_sample;
    }
    if ((draw_constants.feature_mask & kDirectionalLightFlag) != 0u)
    {
        float n_dot_l = max(
            dot(normalize(input.shading_normal),
                normalize(material.light_direction_intensity.xyz)),
            0.0f);
        float light_scale =
            0.15f + 0.85f * n_dot_l * material.light_direction_intensity.w;
        shaded.rgb *= material.light_color_alpha_cutoff.rgb * light_scale;
    }
    if ((draw_constants.feature_mask & kMaskedAlphaFlag) != 0u &&
        shaded.a < material.light_color_alpha_cutoff.a)
    {
        discard;
    }
    return shaded;
}

ForwardFragmentOutput MakeForwardFragmentOutput(float4 color)
{
    ForwardFragmentOutput output;
    output.color = color;
    output.prim_id = draw_constants.prim_id;
    output.instance_id = draw_constants.instance_id;
    return output;
}
