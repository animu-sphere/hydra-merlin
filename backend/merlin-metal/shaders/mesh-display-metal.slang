#include "../../../core/merlin-render-backend/shaders/mesh-display-common.slang"

[[vk::binding(1, 0)]] ConstantBuffer<MeshDisplayConstants> display_constants : register(b1);

[shader("vertex")]
MeshDisplayOutput merlin_mesh_display_vertex(MeshDisplayInput input, uint vertex_id : SV_VertexID)
{
    return MeshDisplayVertex(input, vertex_id, display_constants);
}

[shader("fragment")]
MeshDisplayFragment merlin_mesh_display_fragment(MeshDisplayOutput input)
{
    return MeshDisplayPixel(input, display_constants);
}
