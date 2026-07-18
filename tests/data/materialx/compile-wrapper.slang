#include "materialx-prototype.slang"

struct MerlinMaterialXTestInput
{
    float4 position : SV_Position;
};

[shader("fragment")]
float4 merlin_materialx_test_fragment(MerlinMaterialXTestInput input)
    : SV_Target
{
    MaterialInputs material_inputs;
    material_inputs.SV_Position = input.position;
    return float4(evaluateMaterial(material_inputs), 1.0f);
}
