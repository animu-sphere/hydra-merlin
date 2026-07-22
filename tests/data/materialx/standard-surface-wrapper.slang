#include "materialx-standard-surface.slang"

struct MerlinMaterialXStandardSurfaceTestInput
{
    float3 normal_world : NORMAL;
    float2 texcoord_0 : TEXCOORD0;
};

[shader("fragment")]
float4 merlin_materialx_standard_surface_test_fragment(
    MerlinMaterialXStandardSurfaceTestInput input) : SV_Target
{
    MaterialInputs material_inputs = {};
    material_inputs.normalWorld = input.normal_world;
    material_inputs.texcoord_0 = input.texcoord_0;
    MaterialResult material = evaluateMaterial(material_inputs);
    float coverage = material.metalness + material.specular_roughness;
    coverage += dot(material.shading_normal, float3(0.0f, 0.0f, 0.0f));
    return float4(material.base_color + coverage * 0.0f, 1.0f);
}
