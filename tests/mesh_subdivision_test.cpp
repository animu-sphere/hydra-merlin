#include <merlin/core/mesh_subdivision.hpp>
#include <merlin/core/render_world.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>

#ifdef MERLIN_HAS_OPENSUBDIV
#include <opensubdiv/far/primvarRefiner.h>
#include <opensubdiv/far/topologyDescriptor.h>
#endif

namespace {
using namespace merlin;

class Sink final : public DiagnosticSink {
public:
  void Report(const Diagnostic& value) override {
    diagnostics.push_back(value);
  }
  std::vector<Diagnostic> diagnostics;
};

#ifdef MERLIN_HAS_OPENSUBDIV
bool Near(float actual, float expected) {
  return std::abs(actual - expected) < 2.0e-5F;
}

float Dot(Vec3 first, Vec3 second) {
  return first.x * second.x + first.y * second.y + first.z * second.z;
}

struct ReferenceValue {
  Vec4 value;
  void Clear() {
    value = {};
  }
  void AddWithWeight(const ReferenceValue& source, float weight) {
    value.x += source.value.x * weight;
    value.y += source.value.y * weight;
    value.z += source.value.z * weight;
  }
};

void CompareFar(const SubdivisionTopology& topology, std::span<const Vec3> points,
    const SubdivisionPrimvar& uvs, int level, const MeshDescriptor& mesh) {
  namespace Far = OpenSubdiv::Far;
  namespace Sdc = OpenSubdiv::Sdc;
  Far::TopologyDescriptor input;
  input.numVertices = static_cast<int>(points.size());
  input.numFaces = static_cast<int>(topology.face_counts.size());
  input.numVertsPerFace = topology.face_counts.data();
  input.vertIndicesPerFace = topology.point_indices.data();
  input.numHoles = static_cast<int>(topology.holes.size());
  input.holeIndices = topology.holes.data();
  const std::array crease_pairs{0, 1, 1, 5};
  const std::array crease_weights{2.5F, 2.5F};
  if (!topology.crease_indices.empty()) {
    input.numCreases = static_cast<int>(crease_weights.size());
    input.creaseVertexIndexPairs = crease_pairs.data();
    input.creaseWeights = crease_weights.data();
  }
  input.numCorners = static_cast<int>(topology.corner_indices.size());
  input.cornerVertexIndices = topology.corner_indices.data();
  input.cornerWeights = topology.corner_sharpness.data();
  Far::TopologyDescriptor::FVarChannel channel;
  channel.numValues = static_cast<int>(uvs.values.size());
  channel.valueIndices = uvs.indices.data();
  input.numFVarChannels = 1;
  input.fvarChannels = &channel;
  Sdc::Options options;
  options.SetVtxBoundaryInterpolation(topology.boundary == SubdivisionBoundary::EdgeOnly
                                          ? Sdc::Options::VTX_BOUNDARY_EDGE_ONLY
                                          : Sdc::Options::VTX_BOUNDARY_EDGE_AND_CORNER);
  options.SetFVarLinearInterpolation(Sdc::Options::FVAR_LINEAR_CORNERS_PLUS1);
  using Factory = Far::TopologyRefinerFactory<Far::TopologyDescriptor>;
  std::unique_ptr<Far::TopologyRefiner> refiner(Factory::Create(input,
      Factory::Options(Sdc::SCHEME_CATMARK, options)));
  assert(refiner);
  refiner->RefineUniform(Far::TopologyRefiner::UniformOptions(level));
  Far::PrimvarRefiner reference(*refiner);
  std::vector<ReferenceValue> positions;
  std::vector<ReferenceValue> texcoords;
  for (const auto point : points) {
    positions.push_back({{point.x, point.y, point.z, 0}});
  }
  for (const auto uv : uvs.values) {
    texcoords.push_back({uv});
  }
  for (int step = 1; step <= level; ++step) {
    const auto& refined = refiner->GetLevel(step);
    std::vector<ReferenceValue> refined_points(static_cast<std::size_t>(refined.GetNumVertices()));
    std::vector<ReferenceValue> refined_uvs(static_cast<std::size_t>(refined.GetNumFVarValues()));
    reference.Interpolate(step, positions, refined_points);
    reference.InterpolateFaceVarying(step, texcoords, refined_uvs);
    positions = std::move(refined_points);
    texcoords = std::move(refined_uvs);
  }
  const auto& refined = refiner->GetLevel(level);
  const auto order = topology.left_handed ? std::array{0, 2, 1, 0, 3, 2}
                                          : std::array{0, 1, 2, 0, 2, 3};
  std::size_t index{};
  for (int face = 0; face < refined.GetNumFaces(); ++face) {
    if (refined.IsFaceHole(face)) {
      continue;
    }
    for (const int corner : order) {
      assert(index < mesh.positions.size());
      const auto expected = positions[static_cast<std::size_t>(refined.GetFaceVertices(face)[corner])].value;
      const auto expected_uv = texcoords[static_cast<std::size_t>(refined.GetFaceFVarValues(face)[corner])].value;
      const auto actual = mesh.positions[index];
      const auto actual_uv = mesh.texcoords[index];
      if (!Near(actual.x, expected.x) || !Near(actual.y, expected.y) || !Near(actual.z, expected.z)) {
        std::fprintf(stderr, "%s L%d face %d corner %d: (%g, %g, %g) expected (%g, %g, %g)\n",
            mesh.label.c_str(), level, face, corner, actual.x, actual.y, actual.z,
            expected.x, expected.y, expected.z);
      }
      assert(Near(actual.x, expected.x) && Near(actual.y, expected.y) && Near(actual.z, expected.z));
      assert(Near(actual_uv.x, expected_uv.x) && Near(actual_uv.y, expected_uv.y));
      ++index;
    }
  }
  assert(index == mesh.positions.size());
  assert(index == mesh.indices.size());
  assert(index == mesh.normals.size());
  assert(index == mesh.tangents.size());
}

void CheckSharpCornerFrames(const MeshDescriptor& mesh, Vec3 corner) {
  std::vector<Vec3> corner_normals;
  bool saw_tangent{};
  for (std::size_t vertex = 0; vertex < mesh.positions.size(); ++vertex) {
    const auto position = mesh.positions[vertex];
    const auto normal = mesh.normals[vertex];
    assert(Near(Dot(normal, normal), 1.0F));
    const auto tangent = mesh.tangents[vertex];
    if (tangent.w != 0.0F) {
      const Vec3 direction{tangent.x, tangent.y, tangent.z};
      assert(Near(Dot(normal, direction), 0.0F));
      assert(Near(Dot(direction, direction), 1.0F));
      assert(std::abs(tangent.w) == 1.0F);
      saw_tangent = true;
    }
    if (Near(position.x, corner.x) && Near(position.y, corner.y) &&
        Near(position.z, corner.z) &&
        std::ranges::none_of(corner_normals, [&](Vec3 other) {
          return Near(Dot(normal, other), 1.0F);
        })) {
      corner_normals.push_back(normal);
    }
  }
  assert(saw_tangent);
  assert(corner_normals.size() == 3);
}

void CheckRefinement() {
  SubdivisionTopology topology;
  topology.scheme = SubdivisionScheme::CatmullClark;
  topology.face_counts = {4, 4, 4};
  topology.point_indices = {0, 4, 5, 1, 1, 5, 6, 2, 2, 6, 7, 3};
  std::array<Vec3, 8> points{{{-1.2F, 0, -.5F}, {-.4F, 0, -.5F},
      {.4F, .15F, -.5F}, {1.2F, 0, -.5F}, {-1.2F, 0, .5F},
      {-.4F, .6F, .5F}, {.4F, .2F, .5F}, {1.2F, 0, .5F}}};
  SubdivisionPrimvar uv{PrimvarInterpolation::FaceVarying,
      {{0, 0}, {0, 1}, {.25F, 1}, {.25F, 0}, {.5F, 0}, {.5F, 1},
          {.75F, 1}, {.75F, 0}, {1, 1}, {1, 0}},
      {0, 1, 2, 3, 4, 5, 6, 7, 7, 6, 8, 9}};
  SubdivisionPrimvar color{PrimvarInterpolation::Uniform, {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {}};
  SubdivisionPrimvar opacity{PrimvarInterpolation::Constant, {{.5F}}, {}};
  MeshSubdivision cache;
  auto result = cache.Refine(topology, points, {&uv, &color, &opacity}, 1, "strip");
  assert(result.status == SubdivisionStatus::Refined && result.rebuilt_plan);
  assert(cache.WireEdges().size() == 32);
  CompareFar(topology, points, uv, 1, result.mesh);
  for (const auto value : result.mesh.colors) {
    assert(Near(value.x + value.y + value.z, 1.0F) && Near(value.w, .5F));
  }
  bool saw_seam{};
  for (std::size_t i = 0; i < result.mesh.positions.size(); ++i) {
    const auto p = result.mesh.positions[i];
    for (std::size_t j = i + 1; j < result.mesh.positions.size(); ++j) {
      const auto q = result.mesh.positions[j];
      if (Near(p.x, q.x) && Near(p.y, q.y) && Near(p.z, q.z) &&
          !Near(result.mesh.texcoords[i].x, result.mesh.texcoords[j].x)) {
        saw_seam = true;
      }
    }
  }
  assert(saw_seam);
  RenderWorld world;
  assert(world.CreateMesh(result.mesh).valid());
  for (int level = 2; level <= MeshSubdivision::kMaxLevel; ++level) {
    result = cache.Refine(topology, points, {&uv}, level, "strip");
    assert(result.status == SubdivisionStatus::Refined && result.rebuilt_plan);
    CompareFar(topology, points, uv, level, result.mesh);
    points[5].y += .25F;
    result = cache.Refine(topology, points, {&uv}, level, "strip");
    assert(result.status == SubdivisionStatus::Refined && !result.rebuilt_plan);
    CompareFar(topology, points, uv, level, result.mesh);
  }
  topology.boundary = SubdivisionBoundary::EdgeOnly;
  topology.left_handed = true;
  topology.holes = {2};
  result = cache.Refine(topology, points, {&uv}, 2, "strip");
  assert(result.status == SubdivisionStatus::Refined && result.rebuilt_plan);
  CompareFar(topology, points, uv, 2, result.mesh);
  uv.indices[4] = 3;
  result = cache.Refine(topology, points, {&uv}, 2, "strip");
  assert(result.status == SubdivisionStatus::Refined && result.rebuilt_plan);
  CompareFar(topology, points, uv, 2, result.mesh);

  // Closed cube with the fixture's semi-sharp crease and fully sharp corner.
  topology.holes.clear();
  topology.left_handed = false;
  topology.face_counts = {4, 4, 4, 4, 4, 4};
  topology.point_indices = {0, 1, 2, 3, 4, 7, 6, 5, 0, 4, 5, 1,
      1, 5, 6, 2, 2, 6, 7, 3, 3, 7, 4, 0};
  topology.crease_indices = {0, 1, 5};
  topology.crease_lengths = {3};
  topology.crease_sharpness = {2.5F};
  topology.corner_indices = {6};
  topology.corner_sharpness = {10.0F};
  points = {{{-1, 0, -1}, {1, 0, -1}, {1, 0, 1}, {-1, 0, 1},
      {-1, 2, -1}, {1, 2, -1}, {1, 2, 1}, {-1, 2, 1}}};
  uv.values = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  uv.indices.resize(topology.point_indices.size());
  for (std::size_t i = 0; i < uv.indices.size(); ++i) {
    uv.indices[i] = static_cast<int>(i % 4U);
  }
  result = cache.Refine(topology, points, {&uv}, 2, "cube");
  assert(result.status == SubdivisionStatus::Refined && result.rebuilt_plan);
  CompareFar(topology, points, uv, 2, result.mesh);
  CheckSharpCornerFrames(result.mesh, points[6]);
  topology.crease_sharpness = {0.0F, 0.0F};
  result = cache.Refine(topology, points, {&uv}, 2, "cube");
  assert(result.status == SubdivisionStatus::Refined && result.rebuilt_plan);
  topology.crease_indices[1] = 6;
  assert(cache.Refine(topology, points, {&uv}, 2, "cube").status == SubdivisionStatus::Fallback);
}
#endif
} // namespace

int main() {
  MeshSubdivision cache;
  Sink sink;
  SubdivisionTopology topology;
  topology.face_counts = {4};
  topology.point_indices = {0, 1, 2, 3};
  const std::array<Vec3, 4> points{{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}};
  assert(cache.Refine(topology, points, {}, 1, "quad", &sink).status == SubdivisionStatus::Polygon);
  topology.scheme = SubdivisionScheme::CatmullClark;
  assert(cache.Refine(topology, points, {}, 0, "quad", &sink).status == SubdivisionStatus::Polygon);
  assert(sink.diagnostics.empty());
  assert(cache.Refine(topology, points, {}, MeshSubdivision::kMaxLevel + 1, "quad", &sink).status ==
         SubdivisionStatus::Fallback);
  assert(sink.diagnostics.back().code == "mesh.subdivision.level");
  topology.point_indices[0] = -1;
  assert(cache.Refine(topology, points, {}, 1, "quad", &sink).status == SubdivisionStatus::Rejected);
  assert(sink.diagnostics.back().code == "mesh.subdivision.invalid-cage");
  topology.point_indices[0] = 0;
#ifdef MERLIN_HAS_OPENSUBDIV
  CheckRefinement();
  topology.face_counts.assign(2049, 4);
  topology.point_indices.resize(topology.face_counts.size() * 4U);
  assert(cache.Refine(topology, points, {}, 3, "over-budget", &sink).status == SubdivisionStatus::Fallback);
  assert(sink.diagnostics.back().code == "mesh.subdivision.budget");
#else
  assert(cache.Refine(topology, points, {}, 1, "quad", &sink).status == SubdivisionStatus::Fallback);
  assert(sink.diagnostics.back().code == "mesh.subdivision.unavailable");
#endif
}
