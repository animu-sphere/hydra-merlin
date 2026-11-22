#include "../../../core/merlin-render-backend/shaders/dome-common.slang"

[[vk::push_constant]] ConstantBuffer<DomeConstants> dome_constants;
[[vk::binding(0, 0)]] Texture2D<float4> dome_environment;

[shader("vertex")]
DomeVertex dome_vertex(uint id : SV_VertexID)
{
    return MakeDomeVertex(id);
}

[shader("fragment")]
DomeOutput dome_fragment(DomeVertex input)
{
    return ShadeDome(input, dome_constants, dome_environment);
}
