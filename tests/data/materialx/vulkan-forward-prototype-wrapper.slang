#include "materialx-prototype.slang"
#include "forward-common.slang"

[[vk::binding(31, 0)]]
ConstantBuffer<MaterialConstants> material_constants;

[shader("fragment")]
ForwardFragmentOutput merlin_materialx_forward_fragment(
    ForwardVertexOutput input, bool front_facing : SV_IsFrontFace)
{
    SurfaceGeometry surface =
        BuildForwardSurface(input, front_facing, material_constants);
    MaterialInputs material_inputs = {};
    material_inputs.SV_Position = input.position;
    float3 base_color = evaluateMaterial(material_inputs);
    return MakeForwardFragmentOutput(
        ShadeForwardSurface(
            surface,
            material_constants,
            float4(base_color, input.color.a),
            draw_constants));
}
