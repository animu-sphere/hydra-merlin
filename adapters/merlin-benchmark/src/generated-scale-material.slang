// Deterministic hand-authored material-function ABI fixture. This exercises
// renderer-owned generated artifact composition without a MaterialX dependency.
#include "forward-common.slang"
[[vk::binding(0, 0)]] ConstantBuffer<float4> tint;
[[vk::binding(31, 0)]] ConstantBuffer<MaterialConstants> material_constants;
float3 evaluateMaterial() { return tint.xyz; }
[shader("fragment")]
ForwardFragmentOutput benchmark_generated_fragment(ForwardVertexOutput input)
{
    return MakeForwardFragmentOutput(ShadeForwardSurface(input, material_constants,
        float4(evaluateMaterial(), input.color.a), input.shading_normal, draw_constants));
}
