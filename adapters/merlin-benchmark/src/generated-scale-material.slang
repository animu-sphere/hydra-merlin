// Deterministic hand-authored material-function ABI fixture. This exercises
// renderer-owned generated artifact composition without a MaterialX dependency.
#include "forward-common.slang"
[[vk::binding(0, 0)]] ConstantBuffer<float4> tint;
[[vk::binding(31, 0)]] ConstantBuffer<MaterialConstants> material_constants;
float3 evaluateMaterial() { return tint.xyz; }
[shader("fragment")]
ForwardFragmentOutput benchmark_generated_fragment(
    ForwardVertexOutput input, bool front_facing : SV_IsFrontFace)
{
    SurfaceGeometry surface =
        BuildForwardSurface(input, front_facing, material_constants);
    return MakeForwardFragmentOutput(ShadeForwardSurface(surface, material_constants,
        float4(evaluateMaterial(), input.color.a), draw_constants));
}
