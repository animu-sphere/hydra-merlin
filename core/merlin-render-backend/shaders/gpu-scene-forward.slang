// Requires forward-policy.slang; owns table-backed draw/material semantics.
#include "gpu-scene-abi.slang"

[ForceInline]
DrawConstants LoadForwardGpuDrawConstants(float4x4 view_projection, GpuGeometry geometry, GpuInstance instance, GpuMaterial material)
{
    DrawConstants result = (DrawConstants)0;
    result.model_view_projection = mul(view_projection, instance.transform);
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
    if ((geometry.attribute_mask & 4u) == 0u ||
        material.normal_texture_index == kInvalidGpuSceneTableIndex ||
        material.normal_sampler_index == kInvalidGpuSceneTableIndex)
        result.feature_mask &= ~kNormalTextureFlag;
    if ((material.material_class_flags & (1u << 16u)) != 0u)
        result.feature_mask |= kMaskedAlphaFlag;
    result.prim_id = instance.object_id;
    result.instance_id = instance.instance_id;
    result.texture_index = material.base_color_texture_index;
#ifdef MERLIN_SEPARATE_SAMPLER_INDEX
    result.sampler_index = material.base_color_sampler_index;
#endif
    return result;
}

[ForceInline]
MaterialConstants LoadForwardGpuMaterialConstants(GpuMaterial material, MaterialConstants lighting)
{
    lighting.base_color = material.base_color;
    lighting.light_color_alpha_cutoff.a = material.surface_factors.z;
    lighting.normal_map = material.normal_factors;
    lighting.normal_map_resource = uint4(material.normal_texture_index,
        material.normal_sampler_index, 0u, 0u);
    return lighting;
}
