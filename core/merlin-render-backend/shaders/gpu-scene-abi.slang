// Shader half of merlin::render::gpu_scene_abi.hpp. This source is compiled to
// SPIR-V and Metal during ABI tests; compiler reflection must match every C++
// offset and size before a native backend may upload these records.

static const uint kInvalidGpuSceneTableIndex = 0xffffffffu;

struct GpuGeometry
{
    uint vertex_offset;
    uint vertex_count;
    uint index_offset;
    uint index_count;
    uint index_type;
    uint attribute_mask;
    uint meshlet_offset;
    uint meshlet_count;
    float4 bounds_min;
    float4 bounds_max;
};

struct GpuInstance
{
    float4x4 transform;
    float4 normal_matrix_columns[3];
    uint object_id;
    uint instance_id;
    uint visibility_mask;
    uint flags;
};

struct GpuMaterial
{
    float4 base_color;
    float4 surface_factors;
    uint material_class_flags;
    uint base_color_texture_index;
    uint base_color_sampler_index;
    uint base_color_texcoord_set;
};

struct GpuDraw
{
    uint geometry_index;
    uint material_index;
    uint instance_index;
    uint primitive_base;
    uint primitive_count;
    uint flags;
    uint reserved0;
    uint reserved1;
};
