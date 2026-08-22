#include "../../../core/merlin-render-backend/shaders/gpu-scene-abi.slang"

// First Vulkan compute contract for GPU-driven indexed Forward. The initial
// kernel is deliberately deterministic: one invocation walks a native arena
// batch, applies the same conservative culling rules as the CPU oracle, and
// writes a compact VkDrawIndexedIndirectCommand-compatible stream. A later
// parallel prefix-sum implementation can replace the execution strategy
// without changing this resource or command ABI.

static const uint kGpuDrivenVisibilityMaskCulling = 1u << 0u;
static const uint kGpuDrivenFrustumCulling = 1u << 1u;

static const uint kGpuDrivenVisible = 0u;
static const uint kGpuDrivenVisibilityMaskCulled = 1u;
static const uint kGpuDrivenFrustumCulled = 2u;

struct GpuDrivenIndexedConstants
{
    float4x4 view_projection;
    uint visibility_mask;
    uint candidate_count;
    uint flags;
    uint vertex_stride;
};

// Binary-compatible with merlin::render::GpuIndexedIndirectCommand and
// VkDrawIndexedIndirectCommand.
struct GpuIndexedIndirectCommand
{
    uint index_count;
    uint instance_count;
    uint first_index;
    int vertex_offset;
    uint first_instance;
};

struct GpuDrivenIndexedDispatchCounters
{
    uint candidate_count;
    uint visible_count;
    uint visibility_mask_culled_count;
    uint frustum_culled_count;
};

[[vk::push_constant]]
ConstantBuffer<GpuDrivenIndexedConstants> gpu_driven_constants;

[[vk::binding(1, 1)]]
StructuredBuffer<GpuGeometry> gpu_geometries;

[[vk::binding(2, 1)]]
StructuredBuffer<GpuInstance> gpu_instances;

[[vk::binding(4, 1)]]
StructuredBuffer<GpuDraw> gpu_draws;

[[vk::binding(0, 2)]]
StructuredBuffer<uint> candidate_draw_slots;

[[vk::binding(1, 2)]]
RWStructuredBuffer<uint> candidate_results;

[[vk::binding(2, 2)]]
RWStructuredBuffer<GpuIndexedIndirectCommand> indirect_commands;

[[vk::binding(3, 2)]]
RWStructuredBuffer<GpuDrivenIndexedDispatchCounters> dispatch_counters;

bool OutsideFrustum(
    GpuGeometry geometry,
    GpuInstance instance,
    float4x4 view_projection)
{
    bool outside_left = true;
    bool outside_right = true;
    bool outside_bottom = true;
    bool outside_top = true;
    bool outside_near = true;
    bool outside_far = true;
    for (uint corner = 0u; corner < 8u; ++corner)
    {
        float3 local = float3(
            (corner & 1u) == 0u ? geometry.bounds_min.x : geometry.bounds_max.x,
            (corner & 2u) == 0u ? geometry.bounds_min.y : geometry.bounds_max.y,
            (corner & 4u) == 0u ? geometry.bounds_min.z : geometry.bounds_max.z);
        float4 world = mul(instance.transform, float4(local, 1.0f));
        float4 clip = mul(view_projection, world);
        outside_left = outside_left && clip.x < -clip.w;
        outside_right = outside_right && clip.x > clip.w;
        outside_bottom = outside_bottom && clip.y < -clip.w;
        outside_top = outside_top && clip.y > clip.w;
        outside_near = outside_near && clip.z < 0.0f;
        outside_far = outside_far && clip.z > clip.w;
    }
    return outside_left || outside_right || outside_bottom || outside_top ||
        outside_near || outside_far;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void gpu_driven_indexed_compact(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
    if (any(dispatch_thread_id != uint3(0u)))
    {
        return;
    }

    GpuDrivenIndexedDispatchCounters counters;
    counters.candidate_count = gpu_driven_constants.candidate_count;
    counters.visible_count = 0u;
    counters.visibility_mask_culled_count = 0u;
    counters.frustum_culled_count = 0u;

    for (uint candidate_index = 0u;
         candidate_index < gpu_driven_constants.candidate_count;
         ++candidate_index)
    {
        uint draw_slot = candidate_draw_slots[candidate_index];
        GpuDraw draw = gpu_draws[draw_slot];
        GpuGeometry geometry = gpu_geometries[draw.geometry_index];
        GpuInstance instance = gpu_instances[draw.instance_index];

        if ((gpu_driven_constants.flags &
             kGpuDrivenVisibilityMaskCulling) != 0u &&
            (instance.visibility_mask & gpu_driven_constants.visibility_mask) == 0u)
        {
            candidate_results[candidate_index] =
                kGpuDrivenVisibilityMaskCulled;
            ++counters.visibility_mask_culled_count;
            continue;
        }
        if ((gpu_driven_constants.flags & kGpuDrivenFrustumCulling) != 0u &&
            OutsideFrustum(
                geometry, instance, gpu_driven_constants.view_projection))
        {
            candidate_results[candidate_index] = kGpuDrivenFrustumCulled;
            ++counters.frustum_culled_count;
            continue;
        }

        candidate_results[candidate_index] = kGpuDrivenVisible;
        GpuIndexedIndirectCommand command;
        command.index_count = draw.primitive_count * 3u;
        command.instance_count = 1u;
        command.first_index = geometry.index_offset / 4u +
            draw.primitive_base * 3u;
        command.vertex_offset = int(
            geometry.vertex_offset / gpu_driven_constants.vertex_stride);
        command.first_instance = draw_slot;
        indirect_commands[counters.visible_count] = command;
        ++counters.visible_count;
    }

    dispatch_counters[0] = counters;
}
