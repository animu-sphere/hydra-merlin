#include "gpu-scene-abi.slang"

[[vk::binding(0, 0)]]
StructuredBuffer<GpuGeometry> gpu_geometries;
[[vk::binding(1, 0)]]
StructuredBuffer<GpuInstance> gpu_instances;
[[vk::binding(2, 0)]]
StructuredBuffer<GpuMaterial> gpu_materials;
[[vk::binding(3, 0)]]
StructuredBuffer<GpuDraw> gpu_draws;
[[vk::binding(4, 0)]]
RWStructuredBuffer<uint> gpu_scene_abi_sink;

// Read every field so optimized reflection cannot discard part of the ABI.
[shader("compute")]
[numthreads(1, 1, 1)]
void gpu_scene_abi_test(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
    uint index = dispatch_thread_id.x;
    GpuGeometry geometry = gpu_geometries[index];
    GpuInstance instance = gpu_instances[index];
    GpuMaterial material = gpu_materials[index];
    GpuDraw draw = gpu_draws[index];

    uint value = geometry.vertex_offset + geometry.vertex_count +
        geometry.index_offset + geometry.index_count + geometry.index_type +
        geometry.attribute_mask + geometry.meshlet_offset +
        geometry.meshlet_count + asuint(geometry.bounds_min.x) +
        asuint(geometry.bounds_max.x);
    value += asuint(instance.transform[0][0]) +
        asuint(instance.normal_matrix_columns[0].x) +
        asuint(instance.normal_matrix_columns[1].x) +
        asuint(instance.normal_matrix_columns[2].x) + instance.object_id +
        instance.instance_id + instance.visibility_mask + instance.flags;
    value += asuint(material.base_color.x) +
        asuint(material.surface_factors.x) + material.material_class_flags +
        material.base_color_texture_index + material.base_color_sampler_index +
        material.base_color_texcoord_set;
    value += draw.geometry_index + draw.material_index + draw.instance_index +
        draw.primitive_base + draw.primitive_count + draw.flags +
        draw.reserved0 + draw.reserved1;
    gpu_scene_abi_sink[index] = value;
}
