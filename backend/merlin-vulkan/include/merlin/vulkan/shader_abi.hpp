#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include <merlin/core/shader_contract.hpp>
#include <merlin/core/types.hpp>

namespace merlin::vulkan::shader_abi {

inline constexpr std::uint32_t kVersion = 2;
inline constexpr std::uint32_t kArtifactSchemaVersion = 1;

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

enum class ResourceClass {
  CombinedImageSampler,
  Sampler,
  SampledImage,
  UniformBuffer,
};

struct ResourceBinding {
  std::uint32_t set{};
  std::uint32_t binding{};
  ResourceClass resource_class{};
};

inline constexpr ResourceBinding kConventionalBaseColorTexture{
    0, 0, ResourceClass::CombinedImageSampler};
inline constexpr ResourceBinding kConventionalMaterialConstants{
    0, 1, ResourceClass::UniformBuffer};
inline constexpr ResourceBinding kBindlessSamplers{
    0, 0, ResourceClass::Sampler};
inline constexpr ResourceBinding kBindlessTextures{
    0, 1, ResourceClass::SampledImage};
inline constexpr ResourceBinding kBindlessMaterialConstants{
    1, 0, ResourceClass::UniformBuffer};

inline constexpr ShaderCapability kConventionalCapabilities =
    ShaderCapability::MaterialConstants |
    ShaderCapability::BaseColorTexture;
inline constexpr ShaderCapability kBindlessCapabilities =
    kConventionalCapabilities | ShaderCapability::BindlessResources |
    ShaderCapability::NonUniformResourceIndexing;

}  // namespace merlin::vulkan::shader_abi
