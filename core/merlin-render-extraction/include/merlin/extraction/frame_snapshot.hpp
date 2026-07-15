#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <merlin/core/change_set.hpp>
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
  // Source aspects retain independent revisions. The packed vertex payload
  // changes when either points_revision or primvar_revision changes.
  std::uint64_t points_revision{};
  std::uint64_t primvar_revision{};
  std::uint64_t topology_revision{};
  std::uint64_t material_partition_revision{};
  std::uint64_t vertex_revision{};
  std::uint64_t index_revision{};
  // Ranges are relative to the matching base revision. A consumer may apply
  // them only when its resident revision equals the base; otherwise it must
  // fall back to the complete immutable payload.
  std::uint64_t vertex_base_revision{};
  std::uint64_t index_base_revision{};
  std::vector<ElementRange> vertex_ranges;
  std::vector<ElementRange> index_ranges;
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
  std::uint64_t transform_revision{};
  std::uint64_t visibility_revision{};
  std::uint64_t material_binding_revision{};
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

// Incremental resource changes between two snapshots from the same
// SceneExtractor. Upserts name records present in the new snapshot; removals
// name handles that are no longer present. Consumers must use the delta only
// when both source_id and base_revision match their resident snapshot. A
// missing delta, a different source, or a revision gap requires full
// reconciliation.
struct ResourceDelta {
  std::vector<std::uint64_t> upserts;
  std::vector<std::uint64_t> removals;
};

struct SnapshotDelta {
  std::uint64_t base_revision{};
  ResourceDelta geometries;
  ResourceDelta textures;
  ResourceDelta samplers;
  ResourceDelta materials;
  ResourceDelta instances;
  ResourceDelta lights;
  bool camera_changed{};
  bool render_settings_changed{};
};

// Immutable result of one committed RenderWorld revision. Geometry payloads
// are shared between consecutive snapshots when the mesh did not change, so
// holding a snapshot across frame latency does not copy vertex data.
struct FrameSnapshot {
  // Non-zero only for snapshots produced by SceneExtractor. Together with
  // revision it prevents two independent RenderWorld timelines from being
  // mistaken for the same persistent resource stream.
  std::uint64_t source_id{};
  std::uint64_t revision{};
  std::optional<SnapshotDelta> delta;
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
