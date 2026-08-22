#define MERLIN_EXPLICIT_DRAW_CONSTANTS 1
#include "forward-common.slang"
#include "../../../core/merlin-render-backend/shaders/gpu-scene-abi.slang"

// GPU-driven Forward obtains the persistent draw slot from firstInstance and
// carries it flat to the fragment stage so both stages resolve one identity.
struct GpuDrivenForwardConstants
{
    float4x4 view_projection;
};

struct GpuDrivenForwardVertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float4 display_color : COLOR0;
    [[vk::location(3)]] float2 texcoord : TEXCOORD0;
    uint draw_slot : SV_InstanceID;
};

struct GpuDrivenForwardVertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float3 shading_normal : TEXCOORD0;
    [[vk::location(2)]] float2 texture_coordinate : TEXCOORD1;
    [[vk::location(3)]] nointerpolation uint draw_slot : TEXCOORD2;
};

[[vk::push_constant]]
ConstantBuffer<GpuDrivenForwardConstants> gpu_driven_forward_constants;

[[vk::binding(0, 0)]] SamplerState bindless_samplers[];
[[vk::binding(1, 0)]] Texture2D bindless_textures[];
[[vk::binding(0, 1)]] ConstantBuffer<MaterialConstants> material_constants;
[[vk::binding(1, 1)]] StructuredBuffer<GpuGeometry> gpu_geometries;
[[vk::binding(2, 1)]] StructuredBuffer<GpuInstance> gpu_instances;
[[vk::binding(3, 1)]] StructuredBuffer<GpuMaterial> gpu_materials;
[[vk::binding(4, 1)]] StructuredBuffer<GpuDraw> gpu_draws;

DrawConstants LoadGpuDrivenDrawConstants(uint draw_slot)
{
    GpuDraw draw = gpu_draws[draw_slot];
    GpuGeometry geometry = gpu_geometries[draw.geometry_index];
    GpuInstance instance = gpu_instances[draw.instance_index];
    GpuMaterial material = gpu_materials[draw.material_index];
    DrawConstants result;
    result.model_view_projection = mul(
        gpu_driven_forward_constants.view_projection, instance.transform);
    result.normal_matrix_column0 = instance.normal_matrix_columns[0];
    result.normal_matrix_column1 = instance.normal_matrix_columns[1];
    result.normal_matrix_column2 = instance.normal_matrix_columns[2];
    result.feature_mask = material.material_class_flags & 0x0000ffffu;
    if ((geometry.attribute_mask & 2u) == 0u)
        result.feature_mask &= ~kDisplayColorFlag;
    if ((geometry.attribute_mask & 4u) == 0u ||
        material.base_color_texture_index == kInvalidGpuSceneTableIndex ||
        material.base_color_sampler_index == kInvalidGpuSceneTableIndex)
        result.feature_mask &= ~kBaseColorTextureFlag;
    if ((material.material_class_flags & (1u << 16u)) != 0u)
        result.feature_mask |= kMaskedAlphaFlag;
    result.prim_id = instance.object_id;
    result.instance_id = instance.instance_id;
    result.texture_index = material.base_color_texture_index;
    return result;
}

MaterialConstants LoadGpuDrivenMaterial(uint draw_slot)
{
    GpuMaterial material = gpu_materials[gpu_draws[draw_slot].material_index];
    MaterialConstants result = material_constants;
    result.base_color = material.base_color;
    result.light_color_alpha_cutoff.a = material.surface_factors.z;
    return result;
}

[shader("vertex")]
GpuDrivenForwardVertexOutput forward_gpu_driven_vertex(
    GpuDrivenForwardVertexInput input)
{
    DrawConstants draw = LoadGpuDrivenDrawConstants(input.draw_slot);
    ForwardVertexInput conventional_input;
    conventional_input.position = input.position;
    conventional_input.normal = input.normal;
    conventional_input.display_color = input.display_color;
    conventional_input.texcoord = input.texcoord;
    ForwardVertexOutput evaluated = EvaluateForwardVertex(
        conventional_input, LoadGpuDrivenMaterial(input.draw_slot), draw);
    GpuDrivenForwardVertexOutput output;
    output.position = evaluated.position;
    output.color = evaluated.color;
    output.shading_normal = evaluated.shading_normal;
    output.texture_coordinate = evaluated.texture_coordinate;
    output.draw_slot = input.draw_slot;
    return output;
}

[shader("fragment")]
ForwardFragmentOutput forward_gpu_driven_fragment(
    GpuDrivenForwardVertexOutput input)
{
    DrawConstants draw = LoadGpuDrivenDrawConstants(input.draw_slot);
    MaterialConstants material = LoadGpuDrivenMaterial(input.draw_slot);
    ForwardVertexOutput conventional_input;
    conventional_input.position = input.position;
    conventional_input.color = input.color;
    conventional_input.shading_normal = input.shading_normal;
    conventional_input.texture_coordinate = input.texture_coordinate;
    GpuMaterial gpu_material =
        gpu_materials[gpu_draws[input.draw_slot].material_index];
    float4 texture_sample = float4(1.0f);
    if (HasBaseColorTexture(draw))
    {
        texture_sample = bindless_textures[NonUniformResourceIndex(
            gpu_material.base_color_texture_index)].Sample(
                bindless_samplers[NonUniformResourceIndex(
                    gpu_material.base_color_sampler_index)],
                input.texture_coordinate);
    }
    return MakeForwardFragmentOutput(
        ShadeForward(conventional_input, material, texture_sample, draw), draw);
}
