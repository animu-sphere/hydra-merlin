#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include <merlin/core/shader_contract.hpp>
#include <merlin/core/types.hpp>
#include <merlin/extraction/frame_snapshot.hpp>
#include <merlin/render/gpu_driven.hpp>

namespace merlin::vulkan::shader_abi {

inline constexpr std::uint32_t kVersion = 5;
inline constexpr std::uint32_t kArtifactSchemaVersion = 2;

// Derived rather than spelled out so a schema bump cannot leave the runtime
// loading a directory the build system no longer writes. merlin-vulkan
// static_asserts this against MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION.
[[nodiscard]] inline std::filesystem::path ArtifactDirectory() {
  return std::filesystem::path("shaders") /
         ("v" + std::to_string(kArtifactSchemaVersion));
}

struct alignas(16) DrawConstants {
  Mat4 model_view_projection;
  Vec4 normal_matrix_column0;
  Vec4 normal_matrix_column1;
  Vec4 normal_matrix_column2;
  std::uint32_t feature_mask{};
  std::uint32_t prim_id{};
  std::uint32_t instance_id{};
  std::uint32_t texture_index{};
};

struct alignas(16) MaterialConstants {
  Vec4 base_color;
  Vec4 light_direction_intensity;
  Vec4 light_color_alpha_cutoff;
  std::array<Vec4, 9> diffuse_environment;
};

// Table-backed Forward keeps indexed submission on the CPU while selecting a
// persistent GpuDraw record by physical slot in the shader.
struct alignas(16) GpuSceneDrawConstants {
  Mat4 view_projection;
  std::uint32_t draw_slot{};
  std::uint32_t padding[3]{};
};

inline constexpr std::uint32_t kGpuDrivenVisibilityMaskCulling = 1U << 0U;
inline constexpr std::uint32_t kGpuDrivenFrustumCulling = 1U << 1U;
inline constexpr std::uint32_t kGpuDrivenIndexedVertexStride = 48U;

enum class GpuDrivenCandidateResult : std::uint32_t {
  Visible,
  VisibilityMaskCulled,
  FrustumCulled,
};

struct alignas(16) GpuDrivenIndexedConstants {
  Mat4 view_projection;
  std::uint32_t visibility_mask{~std::uint32_t{}};
  std::uint32_t candidate_count{};
  std::uint32_t flags{kGpuDrivenVisibilityMaskCulling |
                      kGpuDrivenFrustumCulling};
  std::uint32_t vertex_stride{kGpuDrivenIndexedVertexStride};
};

struct alignas(16) GpuDrivenIndexedDispatchCounters {
  std::uint32_t candidate_count{};
  std::uint32_t visible_count{};
  std::uint32_t visibility_mask_culled_count{};
  std::uint32_t frustum_culled_count{};
};

static_assert(sizeof(DrawConstants) == 128);
static_assert(alignof(DrawConstants) == 16);
static_assert(offsetof(DrawConstants, model_view_projection) == 0);
static_assert(offsetof(DrawConstants, normal_matrix_column0) == 64);
static_assert(offsetof(DrawConstants, normal_matrix_column1) == 80);
static_assert(offsetof(DrawConstants, normal_matrix_column2) == 96);
static_assert(offsetof(DrawConstants, feature_mask) == 112);
static_assert(offsetof(DrawConstants, prim_id) == 116);
static_assert(offsetof(DrawConstants, instance_id) == 120);
static_assert(offsetof(DrawConstants, texture_index) == 124);
static_assert(sizeof(MaterialConstants) == 192);
static_assert(alignof(MaterialConstants) == 16);
static_assert(offsetof(MaterialConstants, base_color) == 0);
static_assert(offsetof(MaterialConstants, light_direction_intensity) == 16);
static_assert(offsetof(MaterialConstants, light_color_alpha_cutoff) == 32);
static_assert(offsetof(MaterialConstants, diffuse_environment) == 48);
static_assert(sizeof(GpuSceneDrawConstants) == 80);
static_assert(alignof(GpuSceneDrawConstants) == 16);
static_assert(offsetof(GpuSceneDrawConstants, view_projection) == 0);
static_assert(offsetof(GpuSceneDrawConstants, draw_slot) == 64);
static_assert(sizeof(GpuDrivenIndexedConstants) == 80);
static_assert(alignof(GpuDrivenIndexedConstants) == 16);
static_assert(offsetof(GpuDrivenIndexedConstants, view_projection) == 0);
static_assert(offsetof(GpuDrivenIndexedConstants, visibility_mask) == 64);
static_assert(offsetof(GpuDrivenIndexedConstants, candidate_count) == 68);
static_assert(offsetof(GpuDrivenIndexedConstants, flags) == 72);
static_assert(offsetof(GpuDrivenIndexedConstants, vertex_stride) == 76);
static_assert(sizeof(extraction::DrawVertex) == kGpuDrivenIndexedVertexStride);
static_assert(sizeof(GpuDrivenIndexedDispatchCounters) == 16);
static_assert(alignof(GpuDrivenIndexedDispatchCounters) == 16);
static_assert(offsetof(GpuDrivenIndexedDispatchCounters, candidate_count) == 0);
static_assert(offsetof(GpuDrivenIndexedDispatchCounters, visible_count) == 4);
static_assert(offsetof(GpuDrivenIndexedDispatchCounters,
                       visibility_mask_culled_count) == 8);
static_assert(offsetof(GpuDrivenIndexedDispatchCounters,
                       frustum_culled_count) == 12);
static_assert(sizeof(render::GpuIndexedIndirectCommand) == 20);

enum class ResourceClass {
  CombinedImageSampler,
  Sampler,
  SampledImage,
  UniformBuffer,
  StorageBuffer,
};

struct ResourceBinding {
  std::uint32_t set{};
  std::uint32_t binding{};
  ResourceClass resource_class{};
};

inline constexpr ResourceBinding kConventionalBaseColorTexture{
    0, 0, ResourceClass::CombinedImageSampler};
inline constexpr ResourceBinding kConventionalMaterialConstants{
    0, 31, ResourceClass::UniformBuffer};
inline constexpr ResourceBinding kBindlessSamplers{
    0, 0, ResourceClass::Sampler};
inline constexpr ResourceBinding kBindlessTextures{
    0, 1, ResourceClass::SampledImage};
inline constexpr ResourceBinding kBindlessMaterialConstants{
    1, 0, ResourceClass::UniformBuffer};
inline constexpr ResourceBinding kGpuSceneGeometries{
    1, 1, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGpuSceneInstances{
    1, 2, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGpuSceneMaterials{
    1, 3, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGpuSceneDraws{
    1, 4, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGpuDrivenCandidateDrawSlots{
    2, 0, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGpuDrivenCandidateResults{
    2, 1, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGpuDrivenIndirectCommands{
    2, 2, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGpuDrivenDispatchCounters{
    2, 3, ResourceClass::StorageBuffer};

inline constexpr ShaderCapability kConventionalCapabilities =
    ShaderCapability::MaterialConstants |
    ShaderCapability::BaseColorTexture;
inline constexpr ShaderCapability kBindlessCapabilities =
    kConventionalCapabilities | ShaderCapability::BindlessResources |
    ShaderCapability::NonUniformResourceIndexing;

}  // namespace merlin::vulkan::shader_abi
