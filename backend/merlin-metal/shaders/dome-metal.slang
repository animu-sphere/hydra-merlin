#include "../../../core/merlin-render-backend/shaders/dome-common.slang"

[[vk::binding(0, 0)]] ConstantBuffer<DomeConstants> dome_constants : register(b0);
[[vk::binding(1, 0)]] Texture2D<float4> dome_environment : register(t0);

[shader("vertex")]
DomeVertex merlin_dome_vertex(uint id : SV_VertexID)
{
    return MakeDomeVertex(id);
}

[shader("fragment")]
DomeOutput merlin_dome_fragment(DomeVertex input)
{
    return ShadeDome(input, dome_constants, dome_environment);
}
