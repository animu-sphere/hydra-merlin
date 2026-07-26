#include "materialx-prototype.slang"
#include "forward-common.slang"

[[vk::binding(1, 0)]]
ConstantBuffer<MaterialConstants> material_constants;

[shader("fragment")]
ForwardFragmentOutput merlin_materialx_forward_fragment(
    ForwardVertexOutput input)
{
    MaterialInputs material_inputs = {};
    material_inputs.SV_Position = input.position;
    float3 base_color = evaluateMaterial(material_inputs);
    return MakeForwardFragmentOutput(
        ShadeForwardSurface(
            input,
            material_constants,
            float4(base_color, input.color.a),
            input.shading_normal));
}
