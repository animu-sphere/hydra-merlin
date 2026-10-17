#define MERLIN_EXPLICIT_DRAW_CONSTANTS 1
#include "forward-common.slang"
#include "../../../core/merlin-render-backend/shaders/gpu-scene-forward.slang"

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

[shader("vertex")]
ForwardVertexOutput forward_gpu_scene_vertex(ForwardVertexInput input)
{
    GpuDraw draw = gpu_draws[gpu_scene_constants.draw_slot];
    GpuGeometry geometry = gpu_geometries[draw.geometry_index];
    GpuInstance instance = gpu_instances[draw.instance_index];
    GpuMaterial material = gpu_materials[draw.material_index];
    DrawConstants constants = LoadForwardGpuDrawConstants(
        gpu_scene_constants.view_projection, geometry, instance, material);
    return EvaluateForwardVertex(
        input, LoadForwardGpuMaterialConstants(material, material_constants), constants);
}

[shader("fragment")]
ForwardFragmentOutput forward_gpu_scene_fragment(
    ForwardVertexOutput input, bool front_facing : SV_IsFrontFace)
{
    GpuDraw draw = gpu_draws[gpu_scene_constants.draw_slot];
    GpuGeometry geometry = gpu_geometries[draw.geometry_index];
    GpuInstance instance = gpu_instances[draw.instance_index];
    GpuMaterial material = gpu_materials[draw.material_index];
    DrawConstants constants = LoadForwardGpuDrawConstants(
        gpu_scene_constants.view_projection, geometry, instance, material);
    MaterialConstants effective_material = LoadForwardGpuMaterialConstants(material, material_constants);
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
        ShadeForward(input, front_facing, effective_material, texture_sample,
            constants),
        constants);
}
