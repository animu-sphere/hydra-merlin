#include "../../../core/merlin-render-backend/shaders/mesh-display-common.slang"

[[vk::push_constant]] ConstantBuffer<MeshDisplayConstants> display_constants;

[shader("vertex")]
MeshDisplayOutput mesh_display_vertex(MeshDisplayInput input, uint vertex_id : SV_VertexID)
{
    return MeshDisplayVertex(input, vertex_id, display_constants);
}

[shader("fragment")]
MeshDisplayFragment mesh_display_fragment(MeshDisplayOutput input)
{
    return MeshDisplayPixel(input, display_constants);
}
