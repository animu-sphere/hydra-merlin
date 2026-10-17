#include "materialx-standard-surface.slang"
#include "forward-common.slang"

// The generated module occupies binding 0 for its parameter block and
// bindings 1/2 for the texture/sampler pair. Binding ownership stays in this
// renderer wrapper; the graph-only generated source declares none.
[[vk::binding(31, 0)]]
ConstantBuffer<MaterialConstants> material_constants;

[shader("fragment")]
ForwardFragmentOutput merlin_materialx_standard_surface_forward_fragment(
    ForwardVertexOutput input, bool front_facing : SV_IsFrontFace)
{
    // MaterialX reads the shared surface context and returns a world-space
    // normal; the facing and hemisphere rules still apply to it.
    SurfaceGeometry surface =
        BuildForwardSurface(input, front_facing, material_constants);
    MaterialInputs material_inputs = {};
    material_inputs.normalWorld = surface.shading_normal_base;
    material_inputs.texcoord_0 = input.texture_coordinate;
    MaterialResult material = evaluateMaterial(material_inputs);
    surface.shading_normal =
        ResolveShadingNormal(surface, material.shading_normal);
    return MakeForwardFragmentOutput(
        ShadeForwardSurface(
            surface,
            material_constants,
            float4(material.base_color, input.color.a),
            draw_constants));
}
