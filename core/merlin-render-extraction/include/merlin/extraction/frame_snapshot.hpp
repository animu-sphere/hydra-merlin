#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <merlin/core/types.hpp>

namespace merlin::extraction {

struct DrawVertex {
  Vec3 position;
};

// Snapshot records are keyed by the serialized RenderWorld handle value, which
// packs the slot index and generation, plus the resource revision. A reused
// slot therefore never aliases a retired resource, and consumers may cache GPU
// state per (handle, revision) pair without consulting the RenderWorld.

struct GeometryRecord {
  std::uint64_t mesh{};
  // Vertex and index payloads carry independent revisions so consumers can
  // transfer only the sub-resource that actually changed.
  std::uint64_t points_revision{};
  std::uint64_t topology_revision{};
  std::shared_ptr<const std::vector<DrawVertex>> vertices;
  std::shared_ptr<const std::vector<std::uint32_t>> indices;
};

struct MaterialRecord {
  std::uint64_t material{};
  std::uint64_t revision{};
  Vec4 base_color{1.0F, 1.0F, 1.0F, 1.0F};
  float metallic{};
  float roughness{0.5F};
  AlphaMode alpha_mode{AlphaMode::Opaque};
  bool double_sided{};
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

// Immutable result of one committed RenderWorld revision. Geometry payloads
// are shared between consecutive snapshots when the mesh did not change, so
// holding a snapshot across frame latency does not copy vertex data.
struct FrameSnapshot {
  std::uint64_t revision{};
  std::vector<GeometryRecord> geometries;
  std::vector<MaterialRecord> materials;
  std::vector<InstanceRecord> instances;
  std::vector<DrawRecord> draws;
  Mat4 view;
  Mat4 projection;
};

}  // namespace merlin::extraction
