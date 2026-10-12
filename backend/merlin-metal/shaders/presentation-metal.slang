struct PresentationVertexOutput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};
[[vk::binding(8, 0)]] Texture2D<float4> presentation_color : register(t0);
[[vk::binding(10, 0)]] SamplerState presentation_sampler : register(s0);
struct PresentationConstants { uint color_space; };
[[vk::binding(0, 0)]] ConstantBuffer<PresentationConstants> presentation_constants : register(b0);

[shader("vertex")]
PresentationVertexOutput merlin_presentation_vertex(uint vertex_id : SV_VertexID)
{
    static const float2 positions[3] = {
        float2(-1.0f, -1.0f), float2(3.0f, -1.0f), float2(-1.0f, 3.0f)};
    static const float2 texcoords[3] = {
        float2(0.0f, 1.0f), float2(2.0f, 1.0f), float2(0.0f, -1.0f)};
    PresentationVertexOutput output;
    output.position = float4(positions[vertex_id], 0.0f, 1.0f);
    output.texcoord = texcoords[vertex_id];
    return output;
}

[shader("fragment")]
float4 merlin_presentation_fragment(PresentationVertexOutput input) : SV_Target0
{
    float4 result = presentation_color.Sample(presentation_sampler, input.texcoord);
    if (presentation_constants.color_space == 1u)
    {
        result.rgb = float3(
            dot(float3(0.82259287f, 0.17753395f, 0.0f), result.rgb),
            dot(float3(0.03319951f, 0.96678350f, 0.0f), result.rgb),
            dot(float3(0.01708535f, 0.07239572f, 0.91030148f), result.rgb));
    }
    return result;
}
