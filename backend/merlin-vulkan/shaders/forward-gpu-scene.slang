#define MERLIN_EXPLICIT_DRAW_CONSTANTS 1
#include "forward-common.slang"
#include "../../../core/merlin-render-backend/shaders/gpu-scene-abi.slang"

// The table-backed Forward path retains CPU indexed submission for v0.15.0,
// but obtains draw, geometry, instance, and material state from the persistent
// GPU Scene. Only the dense-draw to physical-slot dispatch identity remains a
// per-draw push constant.
struct GpuSceneDrawConstants
{
    float4x4 view_projection;
    uint draw_slot;
    uint3 padding;
};

[[vk::push_constant]]
ConstantBuffer<GpuSceneDrawConstants> gpu_scene_constants;

[[vk::binding(0, 0)]]
SamplerState bindless_samplers[];

[[vk::binding(1, 0)]]
Texture2D bindless_textures[];

[[vk::binding(0, 1)]]
ConstantBuffer<MaterialConstants> material_constants;

[[vk::binding(1, 1)]]
StructuredBuffer<GpuGeometry> gpu_geometries;

[[vk::binding(2, 1)]]
StructuredBuffer<GpuInstance> gpu_instances;

[[vk::binding(3, 1)]]
StructuredBuffer<GpuMaterial> gpu_materials;

[[vk::binding(4, 1)]]
StructuredBuffer<GpuDraw> gpu_draws;

DrawConstants LoadDrawConstants(
    GpuDraw draw,
    GpuGeometry geometry,
    GpuInstance instance,
    GpuMaterial material)
{
    DrawConstants result;
    result.model_view_projection = mul(
        gpu_scene_constants.view_projection, instance.transform);
    result.normal_matrix_column0 = instance.normal_matrix_columns[0];
    result.normal_matrix_column1 = instance.normal_matrix_columns[1];
    result.normal_matrix_column2 = instance.normal_matrix_columns[2];
    result.feature_mask = material.material_class_flags & 0x0000ffffu;
    if ((geometry.attribute_mask & 2u) == 0u)
    {
        result.feature_mask &= ~kDisplayColorFlag;
    }
    if ((geometry.attribute_mask & 4u) == 0u ||
        material.base_color_texture_index == kInvalidGpuSceneTableIndex ||
        material.base_color_sampler_index == kInvalidGpuSceneTableIndex)
    {
        result.feature_mask &= ~kBaseColorTextureFlag;
    }
    if ((material.material_class_flags & (1u << 16u)) != 0u)
    {
        result.feature_mask |= kMaskedAlphaFlag;
    }
    result.prim_id = instance.object_id;
    result.instance_id = instance.instance_id;
    result.texture_index = material.base_color_texture_index;
    return result;
}

MaterialConstants LoadMaterialConstants(GpuMaterial material)
{
    MaterialConstants result = material_constants;
    result.base_color = material.base_color;
    result.light_color_alpha_cutoff.a = material.surface_factors.z;
    return result;
}

[shader("vertex")]
ForwardVertexOutput forward_gpu_scene_vertex(ForwardVertexInput input)
{
    GpuDraw draw = gpu_draws[gpu_scene_constants.draw_slot];
    GpuGeometry geometry = gpu_geometries[draw.geometry_index];
    GpuInstance instance = gpu_instances[draw.instance_index];
    GpuMaterial material = gpu_materials[draw.material_index];
    DrawConstants constants = LoadDrawConstants(
        draw, geometry, instance, material);
    return EvaluateForwardVertex(
        input, LoadMaterialConstants(material), constants);
}

[shader("fragment")]
ForwardFragmentOutput forward_gpu_scene_fragment(ForwardVertexOutput input)
{
    GpuDraw draw = gpu_draws[gpu_scene_constants.draw_slot];
    GpuGeometry geometry = gpu_geometries[draw.geometry_index];
    GpuInstance instance = gpu_instances[draw.instance_index];
    GpuMaterial material = gpu_materials[draw.material_index];
    DrawConstants constants = LoadDrawConstants(
        draw, geometry, instance, material);
    MaterialConstants effective_material = LoadMaterialConstants(material);
    float4 texture_sample = float4(1.0f, 1.0f, 1.0f, 1.0f);
    if (HasBaseColorTexture(constants))
    {
        uint texture_index = NonUniformResourceIndex(
            material.base_color_texture_index);
        uint sampler_index = NonUniformResourceIndex(
            material.base_color_sampler_index);
        texture_sample = bindless_textures[texture_index].Sample(
            bindless_samplers[sampler_index], input.texture_coordinate);
    }
    return MakeForwardFragmentOutput(
        ShadeForward(input, effective_material, texture_sample, constants),
        constants);
}
