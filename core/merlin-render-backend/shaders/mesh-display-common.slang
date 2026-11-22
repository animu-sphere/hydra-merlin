struct MeshDisplayConstants
{
    float4x4 model_view_projection;
    float4 wire_color;
    float2 point_extent;
    uint prim_id;
    uint instance_id;
};

struct MeshDisplayInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR;
};

struct MeshDisplayOutput
{
    float4 position : SV_Position;
    float4 color : COLOR;
};

struct MeshDisplayFragment
{
    float4 color : SV_Target0;
    uint prim_id : SV_Target1;
    uint instance_id : SV_Target2;
};

MeshDisplayOutput MeshDisplayVertex(MeshDisplayInput input, uint vertex_id,
    MeshDisplayConstants constants)
{
    MeshDisplayOutput output;
    output.position = mul(constants.model_view_projection, float4(input.position, 1));
    output.color = constants.wire_color;
    if (constants.point_extent.x > 0)
    {
        // A two-triangle point sprite keeps the same pixel size on both APIs,
        // including Vulkan devices without the optional largePoints feature.
        const float2 corners[6] = {
            float2(-1, -1), float2(1, -1), float2(1, 1),
            float2(-1, -1), float2(1, 1), float2(-1, 1)
        };
        output.position.xy += corners[vertex_id] * constants.point_extent * output.position.w;
        output.color = input.color;
    }
    return output;
}

MeshDisplayFragment MeshDisplayPixel(MeshDisplayOutput input,
    MeshDisplayConstants constants)
{
    MeshDisplayFragment output;
    output.color = input.color;
    output.prim_id = constants.prim_id;
    output.instance_id = constants.instance_id;
    return output;
}
