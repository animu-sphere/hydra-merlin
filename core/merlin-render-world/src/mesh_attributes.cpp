#include <merlin/core/mesh_attributes.hpp>

#include <array>
#include <cmath>
#include <string>

namespace merlin {
namespace {

bool IsFinite(const Vec3& value) noexcept {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

// Normalizes in double precision so finite float extremes neither overflow
// nor underflow; returns nullopt for zero-length or non-finite input.
std::optional<Vec3> Normalized(const Vec3& value) noexcept {
  if (!IsFinite(value)) {
    return std::nullopt;
  }
  const double length = std::hypot(static_cast<double>(value.x),
      static_cast<double>(value.y), static_cast<double>(value.z));
  if (!(length > 0.0) || !std::isfinite(length)) {
    return std::nullopt;
  }
  const Vec3 result{static_cast<float>(value.x / length),
      static_cast<float>(value.y / length), static_cast<float>(value.z / length)};
  if (!IsFinite(result) ||
      (result.x == 0.0F && result.y == 0.0F && result.z == 0.0F)) {
    return std::nullopt;
  }
  return result;
}

// Unnormalized cross(b - a, c - a) in double precision; its length is twice
// the triangle area, which smooth generation uses as the weight.
std::array<double, 3> AreaNormal(const Vec3& a, const Vec3& b,
    const Vec3& c) noexcept {
  const double e0x = double(b.x) - a.x;
  const double e0y = double(b.y) - a.y;
  const double e0z = double(b.z) - a.z;
  const double e1x = double(c.x) - a.x;
  const double e1y = double(c.y) - a.y;
  const double e1z = double(c.z) - a.z;
  return {e0y * e1z - e0z * e1y, e0z * e1x - e0x * e1z,
      e0x * e1y - e0y * e1x};
}

Vec3 ToUnit(const std::array<double, 3>& value) noexcept {
  const double length = std::hypot(value[0], value[1], value[2]);
  if (!(length > 0.0) || !std::isfinite(length)) {
    return {};
  }
  return {static_cast<float>(value[0] / length),
      static_cast<float>(value[1] / length),
      static_cast<float>(value[2] / length)};
}

std::optional<std::size_t> AuthoredElement(const NormalPrimvar& primvar,
    const MeshCorner& corner) noexcept {
  std::size_t element{};
  switch (primvar.interpolation) {
  case PrimvarInterpolation::Constant:
    element = 0;
    break;
  case PrimvarInterpolation::Uniform:
    element = corner.face;
    break;
  case PrimvarInterpolation::Vertex:
  case PrimvarInterpolation::Varying:
    element = corner.point;
    break;
  case PrimvarInterpolation::FaceVarying:
    element = corner.authored_corner;
    break;
  default:
    return std::nullopt;
  }
  if (!primvar.indices.empty()) {
    if (element >= primvar.indices.size() || primvar.indices[element] < 0) {
      return std::nullopt;
    }
    element = static_cast<std::size_t>(primvar.indices[element]);
  }
  if (element >= primvar.values.size()) {
    return std::nullopt;
  }
  return element;
}

// Per-corner geometric normals plus, for smooth generation, the area-weighted
// sum of incident triangle normals per point.
std::vector<Vec3> GenerateCornerNormals(std::span<const Vec3> points,
    std::span<const MeshCorner> corners, NormalGeneration generation) {
  std::vector<Vec3> result(corners.size());
  if (generation == NormalGeneration::Flat) {
    for (std::size_t triangle = 0; triangle + 2U < corners.size();
        triangle += 3U) {
      const auto normal = ToUnit(AreaNormal(points[corners[triangle].point],
          points[corners[triangle + 1U].point],
          points[corners[triangle + 2U].point]));
      result[triangle] = result[triangle + 1U] = result[triangle + 2U] =
          normal;
    }
    return result;
  }
  std::vector<std::array<double, 3>> sums(points.size(),
      std::array<double, 3>{});
  for (std::size_t triangle = 0; triangle + 2U < corners.size();
      triangle += 3U) {
    const auto weighted = AreaNormal(points[corners[triangle].point],
        points[corners[triangle + 1U].point],
        points[corners[triangle + 2U].point]);
    if (!std::isfinite(weighted[0]) || !std::isfinite(weighted[1]) ||
        !std::isfinite(weighted[2])) {
      continue;
    }
    for (std::size_t local = 0; local < 3U; ++local) {
      auto& sum = sums[corners[triangle + local].point];
      sum[0] += weighted[0];
      sum[1] += weighted[1];
      sum[2] += weighted[2];
    }
  }
  for (std::size_t corner = 0; corner < corners.size(); ++corner) {
    result[corner] = ToUnit(sums[corners[corner].point]);
  }
  // A point whose incident triangles cancel keeps a usable direction from its
  // own triangle rather than shading with a zero normal.
  for (std::size_t triangle = 0; triangle + 2U < corners.size();
      triangle += 3U) {
    for (std::size_t local = 0; local < 3U; ++local) {
      auto& normal = result[triangle + local];
      if (normal.x == 0.0F && normal.y == 0.0F && normal.z == 0.0F) {
        normal = ToUnit(AreaNormal(points[corners[triangle].point],
            points[corners[triangle + 1U].point],
            points[corners[triangle + 2U].point]));
      }
    }
  }
  return result;
}

void Emit(NormalPreparationResult& result, DiagnosticSink* sink,
    Diagnostic diagnostic) {
  if (sink != nullptr) {
    sink->Report(diagnostic);
  }
  result.diagnostics.push_back(std::move(diagnostic));
}

} // namespace

bool IsTransformMirrored(const Mat4& transform) noexcept {
  const auto& m = transform.values;
  // Evaluated in double so large finite scales keep their sign.
  const double determinant =
      double(m[0]) * (double(m[5]) * m[10] - double(m[6]) * m[9]) -
      double(m[4]) * (double(m[1]) * m[10] - double(m[2]) * m[9]) +
      double(m[8]) * (double(m[1]) * m[6] - double(m[2]) * m[5]);
  return std::isfinite(determinant) && determinant < 0.0;
}

Vec3 TriangleGeometricNormal(Vec3 a, Vec3 b, Vec3 c) noexcept {
  return ToUnit(AreaNormal(a, b, c));
}

NormalPreparationResult PrepareCornerNormals(std::span<const Vec3> points,
    std::span<const MeshCorner> corners, const NormalPrimvar* authored,
    NormalGeneration generation, std::string_view source,
    DiagnosticSink* sink) {
  NormalPreparationResult result;
  const auto reject = [&](std::string message) {
    Emit(result, sink,
        {kDiagnosticSchemaVersion, "mesh.normals.malformed",
            DiagnosticSeverity::Warning, DiagnosticDisposition::Rejected,
            std::string(source), std::move(message), "reject-prim"});
    return std::move(result);
  };
  if (corners.size() % 3U != 0U) {
    return reject("triangulated corners are not a multiple of three");
  }
  for (const auto& corner : corners) {
    if (corner.point >= points.size()) {
      return reject("corner references a point outside the position array");
    }
  }
  if (authored == nullptr) {
    result.normals = GenerateCornerNormals(points, corners, generation);
    return result;
  }

  std::vector<Vec3> normals(corners.size());
  std::vector<Vec3> geometric;
  for (std::size_t corner = 0; corner < corners.size(); ++corner) {
    const auto element = AuthoredElement(*authored, corners[corner]);
    if (!element) {
      return reject(
          "normals primvar type, interpolation, or indices are malformed");
    }
    if (const auto normal = Normalized(authored->values[*element])) {
      normals[corner] = *normal;
      continue;
    }
    if (geometric.empty()) {
      geometric =
          GenerateCornerNormals(points, corners, NormalGeneration::Flat);
    }
    normals[corner] = geometric[corner];
    ++result.degenerate_count;
  }
  if (result.degenerate_count != 0U) {
    Emit(result, sink,
        {kDiagnosticSchemaVersion, "mesh.normals.degenerate",
            DiagnosticSeverity::Warning, DiagnosticDisposition::Fallback,
            std::string(source),
            std::to_string(result.degenerate_count) +
                " authored normal corner(s) are zero-length or non-finite",
            "use-geometric-normal"});
  }
  result.normals = std::move(normals);
  return result;
}

VertexNormalPreparation PrepareVertexNormals(std::span<const Vec3> positions,
    std::span<const std::uint32_t> indices, std::span<const Vec3> authored) {
  VertexNormalPreparation result;
  result.normals.resize(positions.size());
  std::vector<bool> repaired(positions.size(), authored.empty());
  if (!authored.empty()) {
    for (std::size_t vertex = 0; vertex < positions.size(); ++vertex) {
      if (const auto normal = Normalized(authored[vertex])) {
        result.normals[vertex] = *normal;
      } else {
        repaired[vertex] = true;
        ++result.degenerate_count;
      }
    }
    if (result.degenerate_count == 0U) {
      return result;
    }
  }
  std::vector<MeshCorner> corners;
  corners.reserve(indices.size());
  for (std::size_t i = 0; i < indices.size(); ++i) {
    corners.push_back({indices[i], static_cast<std::uint32_t>(i / 3U),
        static_cast<std::uint32_t>(i)});
  }
  const auto generated =
      GenerateCornerNormals(positions, corners, NormalGeneration::Smooth);
  for (std::size_t corner = 0; corner < corners.size(); ++corner) {
    const auto vertex = corners[corner].point;
    if (repaired[vertex]) {
      result.normals[vertex] = generated[corner];
    }
  }
  result.generated = authored.empty();
  return result;
}

} // namespace merlin
