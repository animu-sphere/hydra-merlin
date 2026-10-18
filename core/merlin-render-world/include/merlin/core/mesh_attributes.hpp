#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <merlin/core/diagnostic.hpp>
#include <merlin/core/types.hpp>

namespace merlin {

// Shared Mesh attribute preparation for the surface geometry contract. Host
// adapters triangulate and resolve their own primvar storage; normal source
// priority, renormalization and degenerate fallback live here so native and
// Hydra meshes follow one policy.

enum class PrimvarInterpolation { Constant,
  Uniform,
  Vertex,
  Varying,
  FaceVarying };

// Flat uses each triangle's geometric normal; Smooth area-weights geometric
// normals over every triangle sharing a point within one primitive.
enum class NormalGeneration { Flat,
  Smooth };

// One triangulated corner. `point` indexes the position array, `face` the
// authored face and `authored_corner` the authored face-vertex. Every three
// consecutive corners form one triangle in rasterized winding order, after any
// leftHanded reversal.
struct MeshCorner {
  std::uint32_t point{};
  std::uint32_t face{};
  std::uint32_t authored_corner{};
};

struct NormalPrimvar {
  PrimvarInterpolation interpolation{PrimvarInterpolation::Vertex};
  std::vector<Vec3> values;
  // Empty means unindexed. Negative or out-of-range entries are malformed.
  std::vector<std::int32_t> indices;
};

struct NormalPreparationResult {
  // One unit normal per corner, or empty when the primitive is rejected.
  std::optional<std::vector<Vec3>> normals;
  // Corners whose authored value was zero-length or non-finite and fell back
  // to the triangle's geometric normal.
  std::size_t degenerate_count{};
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool accepted() const noexcept {
    return normals.has_value();
  }
};

// True when the upper 3x3 of a column-major transform has a negative
// determinant: the transform mirrors surfaces and reverses their winding. A
// singular or non-finite transform is not mirrored.
[[nodiscard]] bool IsTransformMirrored(const Mat4& transform) noexcept;

// Object-space oriented face normal cross(b - a, c - a), normalized. A
// zero-area or non-finite triangle yields the zero vector.
[[nodiscard]] Vec3 TriangleGeometricNormal(Vec3 a, Vec3 b, Vec3 c) noexcept;

// Source priority: authored normals, then renderer-generated normals, then
// the triangle geometric normal. Authored values are renormalized; a degenerate
// authored value falls back to the geometric normal of its corner's triangle
// and is reported once per primitive. A malformed primvar (size, indices or
// interpolation) rejects the primitive. Smooth generation area-weights
// geometric normals over corners that share a point.
[[nodiscard]] NormalPreparationResult PrepareCornerNormals(
    std::span<const Vec3> points, std::span<const MeshCorner> corners,
    const NormalPrimvar* authored, NormalGeneration generation,
    std::string_view source, DiagnosticSink* sink = nullptr);

// Native indexed meshes store one normal per vertex. Missing normals are
// generated smooth over shared vertices; a degenerate authored vertex normal
// falls back to the area-weighted geometric normal of its triangles.
struct VertexNormalPreparation {
  std::vector<Vec3> normals;
  bool generated{};
  std::size_t degenerate_count{};
};

[[nodiscard]] VertexNormalPreparation PrepareVertexNormals(
    std::span<const Vec3> positions, std::span<const std::uint32_t> indices,
    std::span<const Vec3> authored);

// Tangents carry the frame handedness in w: B = cross(N, T.xyz) * T.w. A zero
// tangent (w == 0) means the element has no usable frame; shading then uses
// the screen-space derivative frame.
struct TangentPrimvar {
  PrimvarInterpolation interpolation{PrimvarInterpolation::Vertex};
  // A float3 primvar is stored with w = +1. A negative w is handedness -1;
  // any other w is +1.
  std::vector<Vec4> values;
  std::vector<std::int32_t> indices;
};

struct TangentPreparationResult {
  // One tangent per corner, or empty when the primitive is rejected.
  std::optional<std::vector<Vec4>> tangents;
  // True when the tangents were generated rather than authored.
  bool generated{};
  // Authored corners whose tangent was zero-length, non-finite or parallel to
  // the normal and used the generated tangent instead.
  std::size_t degenerate_count{};
  // Corners left without a frame (zero tangent): no texture coordinates or a
  // degenerate UV mapping on every triangle sharing the element.
  std::size_t unresolved_count{};
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool accepted() const noexcept {
    return tangents.has_value();
  }
};

// Source priority: authored tangents, then generated tangents, then no frame.
// Generation follows MikkTSpace conventions: per-triangle UV derivatives are
// projected onto the corner normal, angle-weighted and accumulated over
// corners that share a point, texture coordinate, normal and handedness. A
// mirrored UV island (opposite handedness) or a UV seam therefore splits the
// accumulation instead of averaging across it. `normals` and `texcoords` hold
// one value per corner; empty texcoords leave every corner without a frame
// unless tangents are authored. A malformed authored primvar rejects the
// primitive with `mesh.tangents.malformed`.
[[nodiscard]] TangentPreparationResult PrepareCornerTangents(
    std::span<const Vec3> points, std::span<const MeshCorner> corners,
    std::span<const Vec3> normals, std::span<const Vec2> texcoords,
    const TangentPrimvar* authored, std::string_view source,
    DiagnosticSink* sink = nullptr);

// Native indexed meshes store one tangent per vertex. A vertex shared by
// triangles of opposite UV handedness cannot hold either frame, so it is left
// without one and counted as a conflict; its triangles shade with the
// derivative frame. Producers that need vertex frames there split vertices on
// mirror seams exactly as they split them on texture seams.
// An all-zero authored vertex tangent means "no frame" and is kept, not
// repaired; any other unusable authored tangent is regenerated.
struct VertexTangentPreparation {
  std::vector<Vec4> tangents;
  bool generated{};
  std::size_t degenerate_count{};
  std::size_t unresolved_count{};
  std::size_t handedness_conflicts{};
};

[[nodiscard]] VertexTangentPreparation PrepareVertexTangents(
    std::span<const Vec3> positions, std::span<const std::uint32_t> indices,
    std::span<const Vec3> normals, std::span<const Vec2> texcoords,
    std::span<const Vec4> authored);

} // namespace merlin
