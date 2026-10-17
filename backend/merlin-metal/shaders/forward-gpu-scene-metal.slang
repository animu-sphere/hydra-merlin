#include "forward-metal-common.slang"
#include "../../../core/merlin-render-backend/shaders/gpu-scene-forward.slang"

struct GpuSceneDrawConstants
{
    float4x4 view_projection;
    uint draw_slot;
    uint padding[3];
};
[[vk::binding(1, 0)]] ConstantBuffer<GpuSceneDrawConstants> gpu_scene_constants : register(b1);
[[vk::binding(4, 0)]] StructuredBuffer<GpuGeometry> gpu_geometries : register(t4);
[[vk::binding(5, 0)]] StructuredBuffer<GpuInstance> gpu_instances : register(t5);
[[vk::binding(6, 0)]] StructuredBuffer<GpuMaterial> gpu_materials : register(t6);
[[vk::binding(7, 0)]] StructuredBuffer<GpuDraw> gpu_draws : register(t7);

[shader("vertex")]
ForwardVertexOutput merlin_vertex_gpu_scene(ForwardVertexInput input)
{
    GpuDraw draw = gpu_draws[gpu_scene_constants.draw_slot];
    return EvaluateMetalVertex(input, LoadForwardGpuDrawConstants(gpu_scene_constants.view_projection,
        gpu_geometries[draw.geometry_index], gpu_instances[draw.instance_index], gpu_materials[draw.material_index]));
}

[shader("fragment")]
ForwardFragmentOutput merlin_fragment_gpu_scene(
    ForwardVertexOutput input, bool front_facing : SV_IsFrontFace)
{
    GpuDraw draw = gpu_draws[gpu_scene_constants.draw_slot];
    GpuMaterial material = gpu_materials[draw.material_index];
    DrawConstants constants = LoadForwardGpuDrawConstants(gpu_scene_constants.view_projection,
        gpu_geometries[draw.geometry_index], gpu_instances[draw.instance_index], material);
    MaterialConstants effective_material = LoadForwardGpuMaterialConstants(material, material_constants);
    float4 texture_sample = float4(1.0f);
    if (HasBaseColorTexture(constants))
        texture_sample = resources.textures[constants.texture_index].Sample(
            resources.samplers[constants.sampler_index], input.texture_coordinate);
    return ShadeMetalForward(
        input, front_facing, constants, effective_material, texture_sample);
}
