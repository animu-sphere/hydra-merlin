#include <merlin/core/mesh_subdivision.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>

#ifdef MERLIN_HAS_OPENSUBDIV
#include <opensubdiv/far/primvarRefiner.h>
#include <opensubdiv/far/stencilTableFactory.h>
#include <opensubdiv/far/topologyDescriptor.h>
#include <opensubdiv/sdc/crease.h>
#endif

namespace merlin {
namespace {

bool ValidIndex(int index, std::size_t count) {
  return index >= 0 && static_cast<std::size_t>(index) < count;
}

bool ValidCage(const SubdivisionTopology& topology, std::size_t point_count) {
  std::size_t offset{};
  for (const int count : topology.face_counts) {
    if (count < 3 ||
        static_cast<std::size_t>(count) > topology.point_indices.size() - offset) {
      return false;
    }
    offset += static_cast<std::size_t>(count);
  }
  if (offset != topology.point_indices.size() ||
      !std::ranges::all_of(topology.point_indices, [&](int point) {
        return ValidIndex(point, point_count);
      })) {
    return false;
  }
  auto holes = topology.holes;
  std::ranges::sort(holes);
  return std::ranges::adjacent_find(holes) == holes.end() &&
         std::ranges::all_of(holes, [&](int face) {
           return ValidIndex(face, topology.face_counts.size());
         });
}

#ifdef MERLIN_HAS_OPENSUBDIV
namespace Far = OpenSubdiv::Far;
namespace Sdc = OpenSubdiv::Sdc;
using Channels = std::array<const SubdivisionPrimvar*, 3>;
constexpr std::size_t kMemoryBudget = 128U * 1024U * 1024U;

bool IsFaceVarying(const SubdivisionPrimvar* channel) {
  return channel && channel->interpolation == PrimvarInterpolation::FaceVarying;
}

std::size_t ElementCount(PrimvarInterpolation interpolation,
    const SubdivisionTopology& topology, std::size_t point_count) {
  switch (interpolation) {
  case PrimvarInterpolation::Constant:
    return 1;
  case PrimvarInterpolation::Uniform:
    return topology.face_counts.size();
  case PrimvarInterpolation::Vertex:
  case PrimvarInterpolation::Varying:
    return point_count;
  case PrimvarInterpolation::FaceVarying:
    return topology.point_indices.size();
  }
  return 0;
}

bool ValidPrimvar(const SubdivisionPrimvar& primvar, std::size_t elements) {
  if (!std::ranges::all_of(primvar.values, [](const Vec4& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
               std::isfinite(value.z) && std::isfinite(value.w);
      })) {
    return false;
  }
  if (primvar.indices.empty()) {
    return primvar.values.size() == elements;
  }
  return primvar.indices.size() == elements &&
         std::ranges::all_of(primvar.indices, [&](int index) {
           return ValidIndex(index, primvar.values.size());
         });
}

bool WithinBudget(const SubdivisionTopology& topology, std::size_t points,
    const Channels& channels, int level) {
  // At level 1 each authored corner produces a quad. Each subsequent level
  // splits every quad in four. Charge for the hierarchy, cascading stencils,
  // expanded attributes, surface preparation and the packed triangle stream.
  const std::size_t corners = topology.point_indices.size();
  const auto bytes_per_corner = 256U + (1024U << (2 * (level - 1)));
  if (corners > kMemoryBudget / bytes_per_corner) {
    return false;
  }
  auto remaining = kMemoryBudget - corners * bytes_per_corner;
  // A coarse n-gon can contribute all n points to each of its vertex
  // stencils. Reserve for that work before materializing stencil weights.
  for (const int count : topology.face_counts) {
    const auto size = static_cast<std::size_t>(count);
    if (size > remaining / 64U / size) {
      return false;
    }
    remaining -= 64U * size * size;
  }
  if (points > remaining / 128U) {
    return false;
  }
  remaining -= points * 128U;
  for (const auto* channel : channels) {
    if (channel) {
      if (channel->values.size() > remaining / 64U) {
        return false;
      }
      remaining -= channel->values.size() * 64U;
    }
  }
  return topology.crease_indices.size() <= remaining / 64U &&
         topology.corner_indices.size() <= remaining / 64U - topology.crease_indices.size();
}

std::optional<Sdc::Options> SchemeOptions(const SubdivisionTopology& topology) {
  if (topology.boundary == SubdivisionBoundary::Unsupported ||
      topology.face_varying == SubdivisionFaceVarying::Unsupported ||
      topology.triangle_rule == SubdivisionTriangleRule::Unsupported ||
      topology.crease_method == SubdivisionCreaseMethod::Unsupported) {
    return std::nullopt;
  }
  const std::array boundaries{Sdc::Options::VTX_BOUNDARY_NONE,
      Sdc::Options::VTX_BOUNDARY_EDGE_ONLY, Sdc::Options::VTX_BOUNDARY_EDGE_AND_CORNER};
  const std::array face_varying{Sdc::Options::FVAR_LINEAR_NONE,
      Sdc::Options::FVAR_LINEAR_CORNERS_ONLY, Sdc::Options::FVAR_LINEAR_CORNERS_PLUS1,
      Sdc::Options::FVAR_LINEAR_CORNERS_PLUS2, Sdc::Options::FVAR_LINEAR_BOUNDARIES,
      Sdc::Options::FVAR_LINEAR_ALL};
  Sdc::Options options;
  options.SetVtxBoundaryInterpolation(boundaries[static_cast<std::size_t>(topology.boundary)]);
  options.SetFVarLinearInterpolation(face_varying[static_cast<std::size_t>(topology.face_varying)]);
  options.SetTriangleSubdivision(topology.triangle_rule == SubdivisionTriangleRule::Smooth
                                     ? Sdc::Options::TRI_SUB_SMOOTH
                                     : Sdc::Options::TRI_SUB_CATMARK);
  options.SetCreasingMethod(topology.crease_method == SubdivisionCreaseMethod::Chaikin
                                ? Sdc::Options::CREASE_CHAIKIN
                                : Sdc::Options::CREASE_UNIFORM);
  return options;
}

bool ExpandCreases(const SubdivisionTopology& topology, std::size_t point_count,
    std::vector<int>& edges, std::vector<float>& weights) {
  const auto valid_point = [&](int point) { return ValidIndex(point, point_count); };
  const auto valid_weight = [](float weight) { return !std::isnan(weight) && weight >= 0; };
  if (!std::ranges::all_of(topology.crease_indices, valid_point) ||
      !std::ranges::all_of(topology.corner_indices, valid_point) ||
      !std::ranges::all_of(topology.crease_sharpness, valid_weight) ||
      !std::ranges::all_of(topology.corner_sharpness, valid_weight) ||
      topology.corner_indices.size() != topology.corner_sharpness.size()) {
    return false;
  }
  std::size_t points_in_chains{};
  for (const int length : topology.crease_lengths) {
    if (length < 2 || static_cast<std::size_t>(length) >
                          topology.crease_indices.size() - points_in_chains) {
      return false;
    }
    points_in_chains += static_cast<std::size_t>(length);
  }
  const auto chain_count = topology.crease_lengths.size();
  const bool per_chain = topology.crease_sharpness.size() == chain_count;
  if (points_in_chains != topology.crease_indices.size() ||
      (!per_chain && topology.crease_sharpness.size() != points_in_chains - chain_count)) {
    return false;
  }
  std::size_t offset{};
  for (std::size_t chain = 0; chain < chain_count; ++chain) {
    const auto length = static_cast<std::size_t>(topology.crease_lengths[chain]);
    for (std::size_t i = 1; i < length; ++i) {
      const auto weight = topology.crease_sharpness[per_chain ? chain : weights.size()];
      edges.push_back(topology.crease_indices[offset + i - 1]);
      edges.push_back(topology.crease_indices[offset + i]);
      weights.push_back(std::min(weight, Sdc::Crease::SHARPNESS_INFINITE));
    }
    offset += length;
  }
  return true;
}

bool FacesHaveUniquePoints(const SubdivisionTopology& topology, std::size_t count) {
  std::vector<int> last_face(count, -1);
  std::size_t offset{};
  for (std::size_t face = 0; face < topology.face_counts.size(); ++face) {
    const auto size = static_cast<std::size_t>(topology.face_counts[face]);
    for (const int point : std::span(topology.point_indices).subspan(offset, size)) {
      auto& previous = last_face[static_cast<std::size_t>(point)];
      if (previous == static_cast<int>(face)) {
        return false;
      }
      previous = static_cast<int>(face);
    }
    offset += size;
  }
  return true;
}

struct Value {
  Vec4 coords;
  void Clear() {
    coords = {};
  }
  void AddWithWeight(const Value& value, float weight) {
    coords.x += value.coords.x * weight;
    coords.y += value.coords.y * weight;
    coords.z += value.coords.z * weight;
    coords.w += value.coords.w * weight;
  }
};

std::vector<Value> RefinePrimvar(
    const Far::TopologyRefiner& refiner, const SubdivisionPrimvar& primvar,
    int fvar_channel) {
  std::vector<Value> values;
  if (primvar.indices.empty() || fvar_channel >= 0) {
    values.reserve(primvar.values.size());
    for (const auto& value : primvar.values) {
      values.push_back({value});
    }
  } else {
    values.reserve(primvar.indices.size());
    for (const int index : primvar.indices) {
      values.push_back({primvar.values[static_cast<std::size_t>(index)]});
    }
  }
  if (primvar.interpolation == PrimvarInterpolation::Constant) {
    return values;
  }
  const Far::PrimvarRefiner interpolate(refiner);
  for (int level = 1; level <= refiner.GetMaxLevel(); ++level) {
    const auto& topology = refiner.GetLevel(level);
    std::vector<Value> next;
    switch (primvar.interpolation) {
    case PrimvarInterpolation::Vertex:
      next.resize(static_cast<std::size_t>(topology.GetNumVertices()));
      interpolate.Interpolate(level, values, next);
      break;
    case PrimvarInterpolation::Varying:
      next.resize(static_cast<std::size_t>(topology.GetNumVertices()));
      interpolate.InterpolateVarying(level, values, next);
      break;
    case PrimvarInterpolation::Uniform:
      next.resize(static_cast<std::size_t>(topology.GetNumFaces()));
      interpolate.InterpolateFaceUniform(level, values, next);
      break;
    case PrimvarInterpolation::FaceVarying:
      next.resize(static_cast<std::size_t>(topology.GetNumFVarValues(fvar_channel)));
      interpolate.InterpolateFaceVarying(level, values, next, fvar_channel);
      break;
    case PrimvarInterpolation::Constant:
      break;
    }
    values = std::move(next);
  }
  return values;
}

std::uint32_t Root(std::vector<std::uint32_t>& groups, std::uint32_t corner) {
  while (groups[corner] != corner) {
    groups[corner] = groups[groups[corner]];
    corner = groups[corner];
  }
  return corner;
}
#endif
} // namespace

struct MeshSubdivision::Plan {
#ifdef MERLIN_HAS_OPENSUBDIV
  SubdivisionTopology topology;
  std::size_t point_count{};
  int level{};
  std::array<int, 3> fvar_channels{-1, -1, -1};
  std::array<std::size_t, 3> fvar_value_counts{};
  std::array<std::vector<int>, 3> fvar_indices;
  std::unique_ptr<const Far::TopologyRefiner> refiner;
  std::unique_ptr<const Far::StencilTable> position_stencils;
  std::vector<MeshCorner> corners;
  std::vector<MeshCorner> normal_corners;
  std::vector<std::uint32_t> normal_points;
  std::vector<MeshCorner> tangent_corners;
  std::vector<std::uint32_t> tangent_points;
  std::vector<std::array<std::uint32_t, 2>> wire_edges;

  bool Matches(const SubdivisionTopology& input, std::size_t points,
      const Channels& channels, int input_level) const {
    if (level != input_level || point_count != points || topology != input) {
      return false;
    }
    for (std::size_t i = 0; i < channels.size(); ++i) {
      const auto* channel = channels[i];
      if (IsFaceVarying(channel) != (fvar_channels[i] >= 0) ||
          (fvar_channels[i] >= 0 &&
              (channel->values.size() != fvar_value_counts[i] ||
                  channel->indices != fvar_indices[i]))) {
        return false;
      }
    }
    return true;
  }

  std::string_view Build(const Channels& channels, Sdc::Options options) {
    std::vector<int> crease_edges;
    std::vector<float> crease_weights;
    if (!ExpandCreases(topology, point_count, crease_edges, crease_weights)) {
      return "invalid crease or corner sharpness data";
    }
    if (!FacesHaveUniquePoints(topology, point_count)) {
      return "a face repeats a point";
    }
    auto corner_weights = topology.corner_sharpness;
    for (auto& weight : corner_weights) {
      weight = std::min(weight, Sdc::Crease::SHARPNESS_INFINITE);
    }
    Far::TopologyDescriptor cage;
    cage.numVertices = static_cast<int>(point_count);
    cage.numFaces = static_cast<int>(topology.face_counts.size());
    cage.numVertsPerFace = topology.face_counts.data();
    cage.vertIndicesPerFace = topology.point_indices.data();
    cage.numHoles = static_cast<int>(topology.holes.size());
    cage.holeIndices = topology.holes.data();
    cage.numCorners = static_cast<int>(topology.corner_indices.size());
    cage.cornerVertexIndices = topology.corner_indices.data();
    cage.cornerWeights = corner_weights.data();
    std::array<Far::TopologyDescriptor::FVarChannel, 3> fvar;
    std::vector<int> unindexed;
    for (std::size_t i = 0; i < channels.size(); ++i) {
      const auto* channel = channels[i];
      if (!IsFaceVarying(channel)) {
        continue;
      }
      fvar_value_counts[i] = channel->values.size();
      fvar_indices[i] = channel->indices;
      fvar_channels[i] = cage.numFVarChannels++;
      auto& descriptor = fvar[static_cast<std::size_t>(fvar_channels[i])];
      descriptor.numValues = static_cast<int>(fvar_value_counts[i]);
      if (channel->indices.empty() && unindexed.empty()) {
        unindexed.resize(topology.point_indices.size());
        std::iota(unindexed.begin(), unindexed.end(), 0);
      }
      descriptor.valueIndices = channel->indices.empty() ? unindexed.data() : channel->indices.data();
    }
    cage.fvarChannels = fvar.data();

    // Validate crease endpoints against actual cage edges before attaching
    // the weights. The descriptor factory ignores unknown crease edges.
    using Factory = Far::TopologyRefinerFactory<Far::TopologyDescriptor>;
    const Factory::Options factory_options(Sdc::SCHEME_CATMARK, options);
    std::unique_ptr<Far::TopologyRefiner> plan(Factory::Create(cage, factory_options));
    if (!plan) {
      return "OpenSubdiv rejected the cage topology";
    }
    const auto& base = plan->GetLevel(0);
    for (int edge = 0; edge < base.GetNumEdges(); ++edge) {
      if (base.IsEdgeNonManifold(edge)) {
        return "non-manifold edges are unsupported";
      }
    }
    for (int vertex = 0; vertex < base.GetNumVertices(); ++vertex) {
      if (base.IsVertexNonManifold(vertex)) {
        return "non-manifold vertices are unsupported";
      }
    }
    if (!crease_weights.empty()) {
      for (std::size_t i = 0; i < crease_edges.size(); i += 2) {
        if (!ValidIndex(base.FindEdge(crease_edges[i], crease_edges[i + 1]),
                static_cast<std::size_t>(base.GetNumEdges()))) {
          return "a crease does not reference a cage edge";
        }
      }
      cage.numCreases = static_cast<int>(crease_weights.size());
      cage.creaseVertexIndexPairs = crease_edges.data();
      cage.creaseWeights = crease_weights.data();
      plan.reset(Factory::Create(cage, factory_options));
      if (!plan) {
        return "OpenSubdiv rejected the crease topology";
      }
    }
    Far::TopologyRefiner::UniformOptions uniform(level);
    uniform.fullTopologyInLastLevel = true;
    plan->RefineUniform(uniform);
    const auto& refined = plan->GetLevel(level);
    BuildCorners(refined);
    if (!corners.empty()) {
      Far::StencilTableFactory::Options stencil_options;
      stencil_options.factorizeIntermediateLevels = false;
      stencil_options.generateOffsets = true;
      position_stencils.reset(Far::StencilTableFactory::Create(*plan, stencil_options));
      if (!position_stencils || position_stencils->GetNumStencils() !=
                                    plan->GetNumVerticesTotal() - cage.numVertices) {
        return "OpenSubdiv could not build the position stencils";
      }
    }
    refiner = std::move(plan);
    return {};
  }

  void BuildCorners(const Far::TopologyLevel& refined) {
    std::vector<int> face_to_quad(static_cast<std::size_t>(refined.GetNumFaces()), -1);
    std::vector<std::uint32_t> points;
    std::vector<std::array<std::uint32_t, 4>> stream_indices;
    const auto order = topology.left_handed ? std::array{0, 2, 1, 0, 3, 2}
                                            : std::array{0, 1, 2, 0, 2, 3};
    for (int face = 0; face < refined.GetNumFaces(); ++face) {
      if (refined.IsFaceHole(face)) {
        continue;
      }
      face_to_quad[static_cast<std::size_t>(face)] = static_cast<int>(stream_indices.size());
      const auto vertices = refined.GetFaceVertices(face);
      std::array<std::uint32_t, 4> indices{};
      for (const int corner : order) {
        indices[static_cast<std::size_t>(corner)] = static_cast<std::uint32_t>(corners.size());
        corners.push_back({static_cast<std::uint32_t>(vertices[corner]),
            static_cast<std::uint32_t>(face), static_cast<std::uint32_t>(corner)});
      }
      for (const int point : vertices) {
        points.push_back(static_cast<std::uint32_t>(point));
      }
      stream_indices.push_back(indices);
    }

    std::vector<std::uint32_t> groups(points.size());
    std::iota(groups.begin(), groups.end(), 0U);
    for (int edge = 0; edge < refined.GetNumEdges(); ++edge) {
      const auto faces = refined.GetEdgeFaces(edge);
      const auto local = refined.GetEdgeFaceLocalIndices(edge);
      const auto vertices = refined.GetEdgeVertices(edge);
      std::array<std::uint32_t, 2> slots{};
      bool has_face = false;
      for (int i = 0; i < faces.size(); ++i) {
        const int quad = face_to_quad[static_cast<std::size_t>(faces[i])];
        if (quad < 0) {
          continue;
        }
        const auto start = static_cast<std::uint32_t>(local[i]);
        const auto end = (start + 1U) % 4U;
        const auto first = static_cast<std::uint32_t>(quad) * 4U;
        const bool forward = points[first + start] == static_cast<std::uint32_t>(vertices[0]);
        const std::array adjacent{first + (forward ? start : end),
            first + (forward ? end : start)};
        if (!has_face) {
          const auto& indices = stream_indices[static_cast<std::size_t>(quad)];
          wire_edges.push_back({indices[start], indices[end]});
          slots = adjacent;
          has_face = true;
        } else if (refined.GetEdgeSharpness(edge) == 0.0F) {
          for (std::size_t endpoint = 0; endpoint < slots.size(); ++endpoint) {
            if (!Sdc::Crease::IsInfinite(refined.GetVertexSharpness(vertices[static_cast<int>(endpoint)]))) {
              groups[Root(groups, slots[endpoint])] = Root(groups, adjacent[endpoint]);
            }
          }
        }
      }
    }

    normal_corners = corners;
    std::vector<int> group_indices(groups.size(), -1);
    for (auto& corner : normal_corners) {
      const auto quad = static_cast<std::uint32_t>(face_to_quad[corner.face]);
      auto& index = group_indices[Root(groups, quad * 4U + corner.authored_corner)];
      if (index < 0) {
        index = static_cast<int>(normal_points.size());
        normal_points.push_back(corner.point);
      }
      corner.point = static_cast<std::uint32_t>(index);
    }

    // Even coincident UV values remain separate when their face-varying
    // topology separates them. Preserve that identity as well as the normal
    // group when passing corners through the shared tangent generator.
    if (fvar_channels[0] >= 0) {
      tangent_corners = normal_corners;
      std::unordered_map<std::uint64_t, std::uint32_t> groups_by_uv;
      for (auto& corner : tangent_corners) {
        const auto uv = static_cast<std::uint32_t>(FaceVaryingIndex(refined, corner, 0));
        const auto key = (static_cast<std::uint64_t>(corner.point) << 32U) | uv;
        const auto [group, inserted] = groups_by_uv.try_emplace(
            key, static_cast<std::uint32_t>(tangent_points.size()));
        if (inserted) {
          tangent_points.push_back(normal_points[corner.point]);
        }
        corner.point = group->second;
      }
    }
  }

  std::size_t FaceVaryingIndex(const Far::TopologyLevel& refined,
      const MeshCorner& corner, std::size_t channel) const {
    return static_cast<std::size_t>(refined.GetFaceFVarValues(
        static_cast<int>(corner.face), fvar_channels[channel])[static_cast<int>(corner.authored_corner)]);
  }

  MeshDescriptor Evaluate(std::span<const Vec3> points, const Channels& channels,
      std::string_view label, DiagnosticSink* sink) const {
    MeshDescriptor mesh;
    mesh.label = label;
    if (corners.empty()) {
      return mesh;
    }
    std::vector<Value> values(point_count + static_cast<std::size_t>(position_stencils->GetNumStencils()));
    for (std::size_t i = 0; i < points.size(); ++i) {
      values[i].coords = {points[i].x, points[i].y, points[i].z, 0.0F};
    }
    // Cascading stencils address the preceding level's local vertex indices.
    // Apply each range after producing its source slice.
    const Value* source = values.data();
    auto* refined_values = values.data() + point_count;
    int first{};
    for (int step = 1; step <= level; ++step) {
      const auto count = refiner->GetLevel(step).GetNumVertices();
      position_stencils->UpdateValues(source, refined_values, first, first + count);
      source = refined_values + first;
      first += count;
    }
    const auto& refined = refiner->GetLevel(level);
    std::vector<Vec3> positions;
    const auto refined_count = static_cast<std::size_t>(refined.GetNumVertices());
    positions.reserve(refined_count);
    for (const auto& value : std::span(values).last(refined_count)) {
      positions.push_back({value.coords.x, value.coords.y, value.coords.z});
    }

    std::vector<Vec3> normal_positions;
    normal_positions.reserve(normal_points.size());
    for (const auto point : normal_points) {
      normal_positions.push_back(positions[point]);
    }
    mesh.normals = std::move(*PrepareCornerNormals(normal_positions, normal_corners,
        nullptr, NormalGeneration::Smooth, label, sink)
            .normals);

    std::array<std::vector<Value>, 3> attributes;
    for (std::size_t i = 0; i < channels.size(); ++i) {
      if (channels[i]) {
        attributes[i] = RefinePrimvar(*refiner, *channels[i], fvar_channels[i]);
      }
    }
    const auto value = [&](std::size_t i, const MeshCorner& corner) {
      std::size_t element{};
      switch (channels[i]->interpolation) {
      case PrimvarInterpolation::Constant:
        break;
      case PrimvarInterpolation::Uniform:
        element = corner.face;
        break;
      case PrimvarInterpolation::Vertex:
      case PrimvarInterpolation::Varying:
        element = corner.point;
        break;
      case PrimvarInterpolation::FaceVarying:
        element = FaceVaryingIndex(refined, corner, i);
        break;
      }
      return attributes[i][element].coords;
    };
    mesh.positions.reserve(corners.size());
    mesh.indices.resize(corners.size());
    std::iota(mesh.indices.begin(), mesh.indices.end(), 0U);
    if (channels[0]) {
      mesh.texcoords.reserve(corners.size());
    }
    if (channels[1] || channels[2]) {
      mesh.colors.reserve(corners.size());
    }
    for (const auto& corner : corners) {
      mesh.positions.push_back(positions[corner.point]);
      if (channels[0]) {
        const auto uv = value(0, corner);
        mesh.texcoords.push_back({uv.x, uv.y});
      }
      if (channels[1] || channels[2]) {
        auto color = channels[1] ? value(1, corner) : Vec4{1, 1, 1, 1};
        color.w = channels[2] ? std::clamp(value(2, corner).x, 0.0F, 1.0F) : 1.0F;
        mesh.colors.push_back(color);
      }
    }
    if (!mesh.texcoords.empty()) {
      std::vector<Vec3> tangent_positions;
      tangent_positions.reserve(tangent_points.size());
      for (const auto point : tangent_points) {
        tangent_positions.push_back(positions[point]);
      }
      const auto& weld_positions = tangent_points.empty() ? normal_positions : tangent_positions;
      const auto& weld_corners = tangent_points.empty() ? normal_corners : tangent_corners;
      mesh.tangents = std::move(*PrepareCornerTangents(weld_positions,
          weld_corners, mesh.normals, mesh.texcoords, nullptr, label, sink)
              .tangents);
    }
    return mesh;
  }
#endif
};

MeshSubdivision::MeshSubdivision() = default;
MeshSubdivision::~MeshSubdivision() = default;
MeshSubdivision::MeshSubdivision(MeshSubdivision&&) noexcept = default;
MeshSubdivision& MeshSubdivision::operator=(MeshSubdivision&&) noexcept = default;

void MeshSubdivision::Reset() noexcept {
  plan_.reset();
  refined_ = false;
}

std::span<const std::array<std::uint32_t, 2>>
MeshSubdivision::WireEdges() const noexcept {
#ifdef MERLIN_HAS_OPENSUBDIV
  if (refined_ && plan_) {
    return plan_->wire_edges;
  }
#endif
  return {};
}

SubdivisionResult MeshSubdivision::Refine(const SubdivisionTopology& topology,
    std::span<const Vec3> points, const SubdivisionPrimvars& primvars,
    int level, std::string_view label, DiagnosticSink* sink) {
  refined_ = false;
  if (topology.scheme == SubdivisionScheme::None || level == 0) {
    Reset();
    return {};
  }
  const auto report = [&](SubdivisionStatus status, std::string code, std::string_view message) {
    if (sink) {
      const bool rejected = status == SubdivisionStatus::Rejected;
      sink->Report({kDiagnosticSchemaVersion, std::move(code), DiagnosticSeverity::Warning,
          rejected ? DiagnosticDisposition::Rejected : DiagnosticDisposition::Fallback,
          std::string(label), std::string(message), rejected ? "reject-prim" : "use-polygon-cage"});
    }
    Reset();
    return SubdivisionResult{status, {}};
  };
  const auto fallback = [&](std::string code, std::string_view message) {
    return report(SubdivisionStatus::Fallback, std::move(code), message);
  };
  if (!ValidCage(topology, points.size())) {
    return report(SubdivisionStatus::Rejected, "mesh.subdivision.invalid-cage",
        "invalid face counts, point indices, or holes");
  }
  if (topology.scheme != SubdivisionScheme::CatmullClark) {
    return fallback("mesh.subdivision.scheme", "subdivision scheme is unsupported");
  }
  if (level < 0 || level > kMaxLevel) {
    return fallback("mesh.subdivision.level", "requested refinement level exceeds the supported range");
  }
#ifndef MERLIN_HAS_OPENSUBDIV
  (void)primvars;
  return fallback("mesh.subdivision.unavailable", "OpenSubdiv CPU support is unavailable in this build");
#else
  const auto options = SchemeOptions(topology);
  if (!options) {
    return fallback("mesh.subdivision.rule", "subdivision interpolation or creasing rule is unsupported");
  }
  const Channels channels{primvars.texcoords, primvars.colors, primvars.opacities};
  if (!WithinBudget(topology, points.size(), channels, level)) {
    return fallback("mesh.subdivision.budget", "estimated subdivision working memory exceeds 128 MiB");
  }
  for (const auto* channel : channels) {
    if (channel && !ValidPrimvar(*channel, ElementCount(channel->interpolation, topology, points.size()))) {
      return fallback("mesh.subdivision.primvar", "subdivision primvar values or indices are malformed");
    }
  }
  if (topology.face_counts.empty()) {
    Reset();
    MeshDescriptor mesh;
    mesh.label = label;
    return {SubdivisionStatus::Refined, std::move(mesh)};
  }
  const bool rebuild = !plan_ || !plan_->Matches(topology, points.size(), channels, level);
  if (rebuild) {
    Reset();
    auto plan = std::make_unique<Plan>();
    plan->topology = topology;
    plan->point_count = points.size();
    plan->level = level;
    if (const auto error = plan->Build(channels, *options); !error.empty()) {
      return fallback("mesh.subdivision.topology", error);
    }
    plan_ = std::move(plan);
  }
  auto mesh = plan_->Evaluate(points, channels, label, sink);
  refined_ = true;
  return {SubdivisionStatus::Refined, std::move(mesh), rebuild};
#endif
}

} // namespace merlin
