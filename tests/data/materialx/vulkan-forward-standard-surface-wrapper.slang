#include "materialx-standard-surface.slang"
#include "forward-common.slang"

// The generated module occupies binding 0 for its parameter block and
// bindings 1/2 for the texture/sampler pair. Binding ownership stays in this
// renderer wrapper; the graph-only generated source declares none.
[[vk::binding(31, 0)]]
ConstantBuffer<MaterialConstants> material_constants;

[shader("fragment")]
ForwardFragmentOutput merlin_materialx_standard_surface_forward_fragment(
    ForwardVertexOutput input)
{
    MaterialInputs material_inputs = {};
    material_inputs.normalWorld = normalize(input.shading_normal);
    material_inputs.texcoord_0 = input.texture_coordinate;
    MaterialResult material = evaluateMaterial(material_inputs);
    return MakeForwardFragmentOutput(
        ShadeForwardSurface(
            input,
            material_constants,
            float4(material.base_color, input.color.a),
            material.shading_normal));
}
