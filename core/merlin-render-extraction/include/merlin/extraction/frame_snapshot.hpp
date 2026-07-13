#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <merlin/core/types.hpp>

namespace merlin::extraction {

struct DrawVertex {
  Vec3 position;
  Vec3 normal{0.0F, 0.0F, 1.0F};
  Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};
  Vec2 texcoord{};
};

// Snapshot records are keyed by the serialized RenderWorld handle value, which
// packs the slot index and generation, plus the resource revision. A reused
// slot therefore never aliases a retired resource, and consumers may cache GPU
// state per (handle, revision) pair without consulting the RenderWorld.

struct GeometryRecord {
  std::uint64_t mesh{};
  // Vertex and index payloads carry independent revisions so consumers can
  // transfer only the sub-resource that actually changed.
  // The packed vertex payload changes for point or primvar edits.
  std::uint64_t points_revision{};
  std::uint64_t topology_revision{};
  std::shared_ptr<const std::vector<DrawVertex>> vertices;
  std::shared_ptr<const std::vector<std::uint32_t>> indices;
  bool has_normals{};
  bool has_colors{};
  bool has_texcoords{};
};

struct TextureRecord {
  std::uint64_t texture{};
  std::uint64_t revision{};
  std::uint32_t width{};
  std::uint32_t height{};
  TextureFormat format{TextureFormat::Rgba8Unorm};
  std::shared_ptr<const std::vector<std::uint8_t>> pixels;
};

struct SamplerRecord {
  std::uint64_t sampler{};
  std::uint64_t revision{};
  FilterMode min_filter{FilterMode::Linear};
  FilterMode mag_filter{FilterMode::Linear};
  AddressMode address_u{AddressMode::Repeat};
  AddressMode address_v{AddressMode::Repeat};
};

struct TextureBindingRecord {
  std::uint32_t texture_index{};
  std::uint32_t sampler_index{};
  std::uint32_t texcoord_set{};
};

struct MaterialRecord {
  std::uint64_t material{};
  std::uint64_t revision{};
  std::uint64_t parameter_revision{};
  std::uint64_t feature_revision{};
  MaterialParameterBlock parameters;
  std::optional<TextureBindingRecord> base_color_texture;
  AlphaMode alpha_mode{AlphaMode::Opaque};
  bool double_sided{};
  MaterialFeature features{MaterialFeature::None};
};

enum class MaterialFallbackCode {
  MissingTexture,
  MissingSampler,
  UnsupportedAlphaBlend,
};

struct MaterialFallbackRecord {
  std::uint64_t material{};
  MaterialFallbackCode code{};
  std::string message;
};

struct InstanceRecord {
  std::uint64_t instance{};
  std::uint64_t revision{};
  std::uint64_t mesh{};
  std::uint64_t material{};
  Mat4 transform;
  bool visible{true};
};

struct DrawRecord {
  std::uint32_t geometry_index{};
  std::uint32_t material_index{};
  std::uint32_t instance_index{};
  std::uint64_t sort_key{};
};

struct LightRecord {
  std::uint64_t light{};
  std::uint64_t revision{};
  LightType type{LightType::Directional};
  Vec3 color{1.0F, 1.0F, 1.0F};
  float intensity{1.0F};
  Mat4 transform;
};

// Immutable result of one committed RenderWorld revision. Geometry payloads
// are shared between consecutive snapshots when the mesh did not change, so
// holding a snapshot across frame latency does not copy vertex data.
struct FrameSnapshot {
  std::uint64_t revision{};
  std::vector<GeometryRecord> geometries;
  std::vector<TextureRecord> textures;
  std::vector<SamplerRecord> samplers;
  std::vector<MaterialRecord> materials;
  std::vector<MaterialFallbackRecord> material_fallbacks;
  std::vector<InstanceRecord> instances;
  std::vector<DrawRecord> draws;
  std::vector<LightRecord> lights;
  Mat4 view;
  Mat4 projection;
};

}  // namespace merlin::extraction
