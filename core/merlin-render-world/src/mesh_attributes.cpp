#include <merlin/core/mesh_attributes.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <string>
#include <unordered_map>

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

template <typename Primvar>
std::optional<std::size_t> AuthoredElement(const Primvar& primvar,
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

template <typename Result>
void Emit(Result& result, DiagnosticSink* sink, Diagnostic diagnostic) {
  if (sink != nullptr) {
    sink->Report(diagnostic);
  }
  result.diagnostics.push_back(std::move(diagnostic));
}


using Double3 = std::array<double, 3>;

double Dot(const Double3& a, const Double3& b) noexcept {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Double3 Cross(const Double3& a, const Double3& b) noexcept {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
      a[0] * b[1] - a[1] * b[0]};
}

Double3 ToDouble(const Vec3& value) noexcept {
  return {value.x, value.y, value.z};
}

Double3 Subtract(const Vec3& a, const Vec3& b) noexcept {
  return {double(a.x) - b.x, double(a.y) - b.y, double(a.z) - b.z};
}

// Object-space dP/du and dP/dv of one triangle. Invalid when the UV mapping
// is degenerate (zero UV area) or any input is non-finite.
struct TriangleUvFrame {
  Double3 tangent{};
  Double3 bitangent{};
  bool valid{};
};

TriangleUvFrame UvFrame(const Vec3& p0, const Vec3& p1, const Vec3& p2,
    const Vec2& uv0, const Vec2& uv1, const Vec2& uv2) noexcept {
  const auto e1 = Subtract(p1, p0);
  const auto e2 = Subtract(p2, p0);
  const double du1 = double(uv1.x) - uv0.x;
  const double dv1 = double(uv1.y) - uv0.y;
  const double du2 = double(uv2.x) - uv0.x;
  const double dv2 = double(uv2.y) - uv0.y;
  const double determinant = du1 * dv2 - du2 * dv1;
  TriangleUvFrame frame;
  if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-20) {
    return frame;
  }
  for (std::size_t axis = 0; axis < 3U; ++axis) {
    frame.tangent[axis] = (e1[axis] * dv2 - e2[axis] * dv1) / determinant;
    frame.bitangent[axis] = (e2[axis] * du1 - e1[axis] * du2) / determinant;
  }
  frame.valid = std::isfinite(Dot(frame.tangent, frame.tangent)) &&
                std::isfinite(Dot(frame.bitangent, frame.bitangent));
  return frame;
}

// Interior angle at `corner` of the triangle; MikkTSpace weights each face's
// contribution by it so subdividing a face does not change the result.
double CornerAngle(const Vec3& corner, const Vec3& a, const Vec3& b) noexcept {
  const auto u = Subtract(a, corner);
  const auto v = Subtract(b, corner);
  const double length = std::sqrt(Dot(u, u) * Dot(v, v));
  if (!(length > 0.0) || !std::isfinite(length)) {
    return 0.0;
  }
  return std::acos(std::clamp(Dot(u, v) / length, -1.0, 1.0));
}

// Angle-weighted tangent sums per (weld element, handedness). Element `e`
// owns groups 2e (+1) and 2e + 1 (-1); corner_sign records the handedness of
// each corner's own contribution, or 0 when its triangle has no UV frame.
struct TangentGroups {
  std::vector<Double3> sums;
  std::vector<double> weights;
  std::vector<std::int8_t> corner_sign;
};

TangentGroups AccumulateTangents(std::span<const Vec3> points,
    std::span<const MeshCorner> corners, std::span<const Vec3> normals,
    std::span<const Vec2> texcoords, std::span<const std::uint32_t> weld,
    std::size_t weld_count) {
  TangentGroups groups;
  groups.sums.assign(weld_count * 2U, Double3{});
  groups.weights.assign(weld_count * 2U, 0.0);
  groups.corner_sign.assign(corners.size(), 0);
  for (std::size_t triangle = 0; triangle + 2U < corners.size();
      triangle += 3U) {
    const auto& p0 = points[corners[triangle].point];
    const auto& p1 = points[corners[triangle + 1U].point];
    const auto& p2 = points[corners[triangle + 2U].point];
    const auto frame = UvFrame(p0, p1, p2, texcoords[triangle],
        texcoords[triangle + 1U], texcoords[triangle + 2U]);
    if (!frame.valid) {
      continue;
    }
    const std::array<const Vec3*, 3> positions{&p0, &p1, &p2};
    for (std::size_t local = 0; local < 3U; ++local) {
      const auto corner = triangle + local;
      const auto normal = ToDouble(normals[corner]);
      const double along = Dot(normal, frame.tangent);
      const Double3 projected{frame.tangent[0] - normal[0] * along,
          frame.tangent[1] - normal[1] * along,
          frame.tangent[2] - normal[2] * along};
      const double length = std::sqrt(Dot(projected, projected));
      const double angle = CornerAngle(*positions[local],
          *positions[(local + 1U) % 3U], *positions[(local + 2U) % 3U]);
      if (!(length > 0.0) || !std::isfinite(length) || !(angle > 0.0)) {
        continue;
      }
      const auto sign = static_cast<std::int8_t>(
          Dot(Cross(normal, frame.tangent), frame.bitangent) < 0.0 ? -1 : 1);
      const auto group = weld[corner] * 2U + (sign < 0 ? 1U : 0U);
      for (std::size_t axis = 0; axis < 3U; ++axis) {
        groups.sums[group][axis] += projected[axis] / length * angle;
      }
      groups.weights[group] += angle;
      groups.corner_sign[corner] = sign;
    }
  }
  return groups;
}

// Unit tangent of one group, Gram-Schmidt orthogonalized against `normal`,
// with its handedness; zero when the group has no usable direction.
Vec4 ResolveTangent(const TangentGroups& groups, std::size_t group,
    const Vec3& normal) noexcept {
  if (!(groups.weights[group] > 0.0)) {
    return {};
  }
  const auto n = ToDouble(normal);
  auto t = groups.sums[group];
  const double along = Dot(n, t);
  for (std::size_t axis = 0; axis < 3U; ++axis) {
    t[axis] -= n[axis] * along;
  }
  const auto unit = ToUnit(t);
  if (unit.x == 0.0F && unit.y == 0.0F && unit.z == 0.0F) {
    return {};
  }
  return {unit.x, unit.y, unit.z, group % 2U == 0U ? 1.0F : -1.0F};
}

// Corner tangents generated from welded elements. A corner whose own triangle
// has no UV frame borrows the dominant handedness of its element.
std::vector<Vec4> GenerateCornerTangents(std::span<const Vec3> points,
    std::span<const MeshCorner> corners, std::span<const Vec3> normals,
    std::span<const Vec2> texcoords, std::span<const std::uint32_t> weld,
    std::size_t weld_count) {
  const auto groups = AccumulateTangents(
      points, corners, normals, texcoords, weld, weld_count);
  std::vector<Vec4> result(corners.size());
  for (std::size_t corner = 0; corner < corners.size(); ++corner) {
    const std::size_t base = weld[corner] * 2U;
    std::size_t group = base;
    if (groups.corner_sign[corner] < 0 ||
        (groups.corner_sign[corner] == 0 &&
            groups.weights[base + 1U] > groups.weights[base])) {
      group = base + 1U;
    }
    result[corner] = ResolveTangent(groups, group, normals[corner]);
  }
  return result;
}

bool IsZero(const Vec4& value) noexcept {
  return value.x == 0.0F && value.y == 0.0F && value.z == 0.0F &&
         value.w == 0.0F;
}

// Authored tangent with unit xyz and a +/-1 handedness, or nullopt when xyz is
// zero-length, non-finite or parallel to the normal.
std::optional<Vec4> AuthoredTangent(const Vec4& value,
    const Vec3& normal) noexcept {
  const auto direction = Normalized({value.x, value.y, value.z});
  if (!direction || !std::isfinite(value.w)) {
    return std::nullopt;
  }
  const auto n = ToDouble(normal);
  const auto t = ToDouble(*direction);
  if (std::abs(Dot(n, t)) > 0.9999) {
    return std::nullopt;
  }
  return Vec4{direction->x, direction->y, direction->z,
      value.w < 0.0F ? -1.0F : 1.0F};
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

TangentPreparationResult PrepareCornerTangents(std::span<const Vec3> points,
    std::span<const MeshCorner> corners, std::span<const Vec3> normals,
    std::span<const Vec2> texcoords, const TangentPrimvar* authored,
    std::string_view source, DiagnosticSink* sink) {
  TangentPreparationResult result;
  const auto reject = [&](std::string message) {
    Emit(result, sink,
        {kDiagnosticSchemaVersion, "mesh.tangents.malformed",
            DiagnosticSeverity::Warning, DiagnosticDisposition::Rejected,
            std::string(source), std::move(message), "reject-prim"});
    return std::move(result);
  };
  if (corners.size() % 3U != 0U || normals.size() != corners.size() ||
      (!texcoords.empty() && texcoords.size() != corners.size())) {
    return reject("corner normals or texture coordinates do not match the "
                  "triangulated corners");
  }
  for (const auto& corner : corners) {
    if (corner.point >= points.size()) {
      return reject("corner references a point outside the position array");
    }
  }

  std::vector<Vec4> generated;
  const auto generate = [&] {
    if (!generated.empty() || texcoords.empty()) {
      return;
    }
    // Weld corners that share the point, texture coordinate and normal bit
    // for bit; a seam or hard edge therefore keeps separate tangents.
    struct Key {
      std::uint32_t point;
      std::array<std::uint32_t, 5> bits;
      bool operator==(const Key&) const = default;
    };
    struct KeyHash {
      std::size_t operator()(const Key& key) const noexcept {
        std::size_t hash = key.point;
        for (const auto bits : key.bits) {
          hash = hash * 1099511628211ULL ^ bits;
        }
        return hash;
      }
    };
    std::unordered_map<Key, std::uint32_t, KeyHash> elements;
    std::vector<std::uint32_t> weld(corners.size());
    for (std::size_t corner = 0; corner < corners.size(); ++corner) {
      const auto& uv = texcoords[corner];
      const auto& n = normals[corner];
      const Key key{corners[corner].point,
          {std::bit_cast<std::uint32_t>(uv.x), std::bit_cast<std::uint32_t>(uv.y),
              std::bit_cast<std::uint32_t>(n.x), std::bit_cast<std::uint32_t>(n.y),
              std::bit_cast<std::uint32_t>(n.z)}};
      weld[corner] = elements
                         .try_emplace(key,
                             static_cast<std::uint32_t>(elements.size()))
                         .first->second;
    }
    generated = GenerateCornerTangents(
        points, corners, normals, texcoords, weld, elements.size());
  };

  std::vector<Vec4> tangents(corners.size());
  if (authored == nullptr) {
    generate();
    if (!generated.empty()) {
      tangents = std::move(generated);
      result.generated = true;
    }
  } else {
    for (std::size_t corner = 0; corner < corners.size(); ++corner) {
      const auto element = AuthoredElement(*authored, corners[corner]);
      if (!element) {
        return reject(
            "tangents primvar type, interpolation, or indices are malformed");
      }
      if (const auto tangent =
              AuthoredTangent(authored->values[*element], normals[corner])) {
        tangents[corner] = *tangent;
        continue;
      }
      generate();
      if (!generated.empty()) {
        tangents[corner] = generated[corner];
      }
      ++result.degenerate_count;
    }
    if (result.degenerate_count != 0U) {
      Emit(result, sink,
          {kDiagnosticSchemaVersion, "mesh.tangents.degenerate",
              DiagnosticSeverity::Warning, DiagnosticDisposition::Fallback,
              std::string(source),
              std::to_string(result.degenerate_count) +
                  " authored tangent corner(s) are zero-length, non-finite "
                  "or parallel to the normal",
              "use-generated-tangent"});
    }
  }
  for (const auto& tangent : tangents) {
    if (IsZero(tangent)) {
      ++result.unresolved_count;
    }
  }
  result.tangents = std::move(tangents);
  return result;
}

VertexTangentPreparation PrepareVertexTangents(
    std::span<const Vec3> positions, std::span<const std::uint32_t> indices,
    std::span<const Vec3> normals, std::span<const Vec2> texcoords,
    std::span<const Vec4> authored) {
  VertexTangentPreparation result;
  result.tangents.resize(positions.size());
  std::vector<bool> missing(positions.size(), authored.empty());
  if (!authored.empty()) {
    for (std::size_t vertex = 0; vertex < positions.size(); ++vertex) {
      // An all-zero tangent explicitly carries no frame (DrawVertex), as
      // Hydra-prepared corners without a UV frame do.
      if (IsZero(authored[vertex])) {
        continue;
      }
      if (const auto tangent =
              AuthoredTangent(authored[vertex], normals[vertex])) {
        result.tangents[vertex] = *tangent;
      } else {
        missing[vertex] = true;
        ++result.degenerate_count;
      }
    }
  }
  const bool generate = !texcoords.empty() &&
                        (authored.empty() || result.degenerate_count != 0U);
  if (generate) {
    std::vector<MeshCorner> corners;
    std::vector<Vec3> corner_normals;
    std::vector<Vec2> corner_texcoords;
    corners.reserve(indices.size());
    corner_normals.reserve(indices.size());
    corner_texcoords.reserve(indices.size());
    for (std::size_t i = 0; i < indices.size(); ++i) {
      corners.push_back({indices[i], static_cast<std::uint32_t>(i / 3U),
          static_cast<std::uint32_t>(i)});
      corner_normals.push_back(normals[indices[i]]);
      corner_texcoords.push_back(texcoords[indices[i]]);
    }
    const auto groups = AccumulateTangents(positions, corners, corner_normals,
        corner_texcoords, indices, positions.size());
    for (std::size_t vertex = 0; vertex < positions.size(); ++vertex) {
      if (!missing[vertex]) {
        continue;
      }
      const double positive = groups.weights[vertex * 2U];
      const double negative = groups.weights[vertex * 2U + 1U];
      if (positive > 0.0 && negative > 0.0) {
        ++result.handedness_conflicts;
        continue;
      }
      result.tangents[vertex] = ResolveTangent(groups,
          vertex * 2U + (negative > positive ? 1U : 0U), normals[vertex]);
    }
    result.generated = authored.empty();
  }
  for (const auto& tangent : result.tangents) {
    if (IsZero(tangent)) {
      ++result.unresolved_count;
    }
  }
  return result;
}

} // namespace merlin
