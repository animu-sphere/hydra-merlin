#include <merlin/core/mesh_attributes.hpp>
#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/render/surface_geometry.hpp>

#include <cassert>
#include <cmath>
#include <limits>
#include <vector>

namespace {

using namespace merlin;

bool Near(const Vec3& a, const Vec3& b, float tolerance = 1.0e-6F) {
  return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance &&
         std::abs(a.z - b.z) <= tolerance;
}

struct CollectingSink final : DiagnosticSink {
  void Report(const Diagnostic& diagnostic) override {
    reported.push_back(diagnostic);
  }
  std::vector<Diagnostic> reported;
};

// Two quads folded 90 degrees along x = 0: face 0 lies in z = 0 facing +Z,
// face 1 in x = 0 facing +X. Points 1 and 2 are shared by both faces.
const std::vector<Vec3> kFoldPoints{{-1, 0, 0}, {0, 0, 0}, {0, 1, 0},
    {-1, 1, 0}, {0, 0, -1}, {0, 1, -1}};
const std::vector<MeshCorner> kFoldCorners{
    {0, 0, 0}, {1, 0, 1}, {2, 0, 2}, {0, 0, 0}, {2, 0, 2}, {3, 0, 3},
    {1, 1, 4}, {4, 1, 5}, {5, 1, 6}, {1, 1, 4}, {5, 1, 6}, {2, 1, 7}};

void GeneratedNormals() {
  const auto flat = PrepareCornerNormals(kFoldPoints, kFoldCorners, nullptr,
      NormalGeneration::Flat, "/fold");
  assert(flat.accepted() && flat.diagnostics.empty());
  for (std::size_t corner = 0; corner < 6; ++corner) {
    assert(Near((*flat.normals)[corner], {0, 0, 1}));
  }
  for (std::size_t corner = 6; corner < 12; ++corner) {
    assert(Near((*flat.normals)[corner], {1, 0, 0}));
  }
  // Smooth sums cross(b - a, c - a) over every incident triangle, so each
  // triangle is weighted by its area: point 1 touches one +Z and two +X
  // triangles, point 2 two +Z and one +X. Unshared points keep their face.
  const auto smooth = PrepareCornerNormals(kFoldPoints, kFoldCorners, nullptr,
      NormalGeneration::Smooth, "/fold");
  assert(smooth.accepted());
  const float inverse = 1.0F / std::sqrt(5.0F);
  assert(Near((*smooth.normals)[0], {0, 0, 1}));
  assert(Near((*smooth.normals)[1], {2 * inverse, 0, inverse}));
  assert(Near((*smooth.normals)[7], {1, 0, 0}));
  assert(Near((*smooth.normals)[11], {inverse, 0, 2 * inverse}));
  // A leftHanded adapter reverses the corner order; generation follows it.
  auto reversed = kFoldCorners;
  for (std::size_t triangle = 0; triangle < reversed.size(); triangle += 3) {
    std::swap(reversed[triangle + 1], reversed[triangle + 2]);
  }
  const auto left = PrepareCornerNormals(kFoldPoints, reversed, nullptr,
      NormalGeneration::Flat, "/fold");
  assert(Near((*left.normals)[0], {0, 0, -1}));
}

void AuthoredInterpolation() {
  // Constant, uniform, vertex/varying and faceVarying, indexed or not, all
  // resolve through the same corner identity.
  NormalPrimvar constant{PrimvarInterpolation::Constant, {{0, 3, 0}}, {}};
  auto result = PrepareCornerNormals(kFoldPoints, kFoldCorners, &constant,
      NormalGeneration::Flat, "/fold");
  for (const auto& normal : *result.normals) {
    assert(Near(normal, {0, 1, 0}));
  }
  NormalPrimvar uniform{PrimvarInterpolation::Uniform, {{0, 0, 2}, {0, 1, 0}},
      {1, 0}};
  result = PrepareCornerNormals(kFoldPoints, kFoldCorners, &uniform,
      NormalGeneration::Flat, "/fold");
  assert(Near((*result.normals)[0], {0, 1, 0}));
  assert(Near((*result.normals)[6], {0, 0, 1}));
  for (const auto interpolation :
      {PrimvarInterpolation::Vertex, PrimvarInterpolation::Varying}) {
    NormalPrimvar vertex{interpolation, {}, {}};
    for (std::size_t point = 0; point < kFoldPoints.size(); ++point) {
      vertex.values.push_back({static_cast<float>(point) + 1.0F, 0, 0});
    }
    result = PrepareCornerNormals(kFoldPoints, kFoldCorners, &vertex,
        NormalGeneration::Flat, "/fold");
    assert(Near((*result.normals)[7], {1, 0, 0}));
  }
  // faceVarying keeps a hard edge at shared points: point 1 differs between
  // the two faces' corners.
  NormalPrimvar face_varying{PrimvarInterpolation::FaceVarying,
      {{0, 0, 1}, {-1, 0, 0}}, {0, 0, 0, 0, 1, 1, 1, 1}};
  result = PrepareCornerNormals(kFoldPoints, kFoldCorners, &face_varying,
      NormalGeneration::Flat, "/fold");
  assert(Near((*result.normals)[1], {0, 0, 1}));
  assert(Near((*result.normals)[6], {-1, 0, 0}));
  // Authored values are renormalized and keep their sign: they already
  // describe the outward side, independent of corner order.
  NormalPrimvar scaled{PrimvarInterpolation::Constant, {{0, 0, -5}}, {}};
  result = PrepareCornerNormals(kFoldPoints, kFoldCorners, &scaled,
      NormalGeneration::Flat, "/fold");
  assert(Near((*result.normals)[3], {0, 0, -1}));
}

void DegenerateAndMalformed() {
  CollectingSink sink;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  NormalPrimvar degenerate{PrimvarInterpolation::FaceVarying,
      {{0, 0, 1}, {0, 0, 0}, {nan, 0, 1}}, {0, 1, 0, 0, 2, 0, 0, 0}};
  const auto result = PrepareCornerNormals(kFoldPoints, kFoldCorners,
      &degenerate, NormalGeneration::Smooth, "/fold", &sink);
  assert(result.accepted());
  // Corner 1 (authored corner 1, zero) and corners 6/9 (authored corner 4,
  // NaN) fall back to their own triangle's Ng, not the smooth normal.
  assert(result.degenerate_count == 3);
  assert(Near((*result.normals)[1], {0, 0, 1}));
  assert(Near((*result.normals)[6], {1, 0, 0}));
  assert(Near((*result.normals)[9], {1, 0, 0}));
  assert(sink.reported.size() == 1);
  assert(sink.reported[0].code == "mesh.normals.degenerate");
  assert(sink.reported[0].disposition == DiagnosticDisposition::Fallback);
  assert(sink.reported[0].source == "/fold");

  for (auto malformed : {
           NormalPrimvar{PrimvarInterpolation::Vertex, {{0, 0, 1}}, {}},
           NormalPrimvar{PrimvarInterpolation::Constant, {{0, 0, 1}}, {-1}},
           NormalPrimvar{PrimvarInterpolation::Uniform, {{0, 0, 1}}, {0, 3}},
       }) {
    sink.reported.clear();
    const auto rejected = PrepareCornerNormals(kFoldPoints, kFoldCorners,
        &malformed, NormalGeneration::Flat, "/fold", &sink);
    assert(!rejected.accepted());
    assert(sink.reported.size() == 1 &&
           sink.reported[0].code == "mesh.normals.malformed" &&
           sink.reported[0].disposition == DiagnosticDisposition::Rejected);
  }
  const std::vector<MeshCorner> out_of_range{{0, 0, 0}, {1, 0, 1}, {9, 0, 2}};
  assert(!PrepareCornerNormals(kFoldPoints, out_of_range, nullptr,
      NormalGeneration::Flat, "/fold")
              .accepted());
}

void VertexNormals() {
  const std::vector<Vec3> positions{{0, 0, 0}, {1, 0, 0}, {1, 1, 0},
      {0, 1, 0}};
  const std::vector<std::uint32_t> indices{0, 1, 2, 0, 2, 3};
  const auto generated = PrepareVertexNormals(positions, indices, {});
  assert(generated.generated && generated.degenerate_count == 0);
  for (const auto& normal : generated.normals) {
    assert(Near(normal, {0, 0, 1}));
  }
  const std::vector<Vec3> authored{{0, 2, 0}, {0, 0, 0}, {0, 1, 0},
      {0, 1, 0}};
  const auto repaired = PrepareVertexNormals(positions, indices, authored);
  assert(!repaired.generated && repaired.degenerate_count == 1);
  assert(Near(repaired.normals[0], {0, 1, 0}));
  assert(Near(repaired.normals[1], {0, 0, 1}));
}

void Orientation() {
  Mat4 identity;
  assert(!IsTransformMirrored(identity));
  Mat4 mirrored;
  mirrored.values[0] = -2.0F;
  assert(IsTransformMirrored(mirrored));
  Mat4 double_mirror = mirrored;
  double_mirror.values[5] = -1.0F;
  assert(!IsTransformMirrored(double_mirror));
  Mat4 singular;
  singular.values[10] = 0.0F;
  assert(!IsTransformMirrored(singular));
  // Non-uniform scale with rotation keeps the sign of its determinant.
  Mat4 sheared;
  sheared.values = {0.0F, 3.0F, 0.0F, 0.0F, -0.5F, 0.0F, 0.0F, 0.0F, 0.0F,
      0.0F, -1.0F, 0.0F, 7.0F, 8.0F, 9.0F, 1.0F};
  assert(IsTransformMirrored(sheared));

  using render::ForwardDrawFrontFace;
  assert(ForwardDrawFrontFace(FrontFaceWinding::Clockwise, false) ==
         FrontFaceWinding::Clockwise);
  assert(ForwardDrawFrontFace(FrontFaceWinding::Clockwise, true) ==
         FrontFaceWinding::CounterClockwise);
  assert(ForwardDrawFrontFace(FrontFaceWinding::CounterClockwise, true) ==
         FrontFaceWinding::Clockwise);
}

void SurfacePolicy() {
  using render::DebugView;
  using render::NdcYDirection;
  using render::PackForwardSurfacePolicy;
  // The sign is the camera winding times the NDC Y direction; mirroring does
  // not enter because it reverses both the winding and the derivative sign.
  assert(PackForwardSurfacePolicy(FrontFaceWinding::Clockwise,
             NdcYDirection::Down, DebugView::None)
             .x == 1.0F);
  assert(PackForwardSurfacePolicy(FrontFaceWinding::Clockwise,
             NdcYDirection::Up, DebugView::None)
             .x == -1.0F);
  assert(PackForwardSurfacePolicy(FrontFaceWinding::CounterClockwise,
             NdcYDirection::Down, DebugView::None)
             .x == -1.0F);
  const std::array<std::pair<DebugView, float>, 5> codes{{
      {DebugView::None, 0.0F},
      {DebugView::GeometricNormal, 1.0F},
      {DebugView::ShadingNormalBase, 2.0F},
      {DebugView::Normal, 3.0F},
      {DebugView::FrontFacing, 4.0F},
  }};
  for (const auto& [view, code] : codes) {
    assert(render::IsForwardSurfaceDebugView(view));
    const auto packed = PackForwardSurfacePolicy(
        FrontFaceWinding::Clockwise, NdcYDirection::Down, view);
    assert(packed.y == code && packed.z == 0.0F && packed.w == 0.0F);
  }
  bool rejected = false;
  try {
    (void)PackForwardSurfacePolicy(FrontFaceWinding::Clockwise,
        NdcYDirection::Down, DebugView::Depth);
  } catch (const render::RendererError& error) {
    rejected = error.code() == render::RendererErrorCode::InvalidRequest;
  }
  assert(rejected && !render::IsForwardSurfaceDebugView(DebugView::Depth));
}

void Extraction() {
  RenderWorld world;
  extraction::SceneExtractor extractor;
  MeshDescriptor mesh;
  mesh.positions = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
  mesh.indices = {0, 1, 2, 0, 2, 3};
  mesh.double_sided = true;
  const auto mesh_handle = world.CreateMesh(mesh);
  InstanceDescriptor instance;
  instance.mesh = mesh_handle;
  instance.material = world.CreateMaterial({});
  const auto instance_handle = world.CreateInstance(instance);
  extractor.Apply(world, world.Commit());
  auto snapshot = extractor.snapshot();
  const auto& geometry = snapshot->geometries[0];
  // Native meshes without normals no longer shade with +Z by default; they
  // follow the shared smooth fallback.
  assert(!geometry.has_normals && geometry.generated_normals);
  assert(geometry.double_sided);
  for (const auto& vertex : *geometry.vertices) {
    assert(Near(vertex.normal, {0, 0, 1}));
  }
  assert(!snapshot->instances[0].mirrored);
  const auto base_revision = snapshot->instance_orientation_revision;

  // A translation keeps the handedness revision; a mirror advances it once.
  instance.transform.values[12] = 3.0F;
  world.UpdateInstance(instance_handle, instance, ChangeAspect::Transform);
  extractor.Apply(world, world.Commit());
  assert(extractor.snapshot()->instance_orientation_revision == base_revision);
  instance.transform.values[0] = -1.0F;
  world.UpdateInstance(instance_handle, instance, ChangeAspect::Transform);
  extractor.Apply(world, world.Commit());
  snapshot = extractor.snapshot();
  assert(snapshot->instances[0].mirrored);
  assert(snapshot->instance_orientation_revision == base_revision + 1);
  instance.transform.values[13] = 1.0F;
  world.UpdateInstance(instance_handle, instance, ChangeAspect::Transform);
  extractor.Apply(world, world.Commit());
  assert(extractor.snapshot()->instance_orientation_revision ==
         base_revision + 1);

  // Generated normals diff against the resident payload: an in-plane move
  // changes only that vertex, and an index rotation with the same winding
  // changes no vertex at all.
  mesh.positions[1].x = 1.5F;
  world.UpdateMesh(mesh_handle, mesh, ChangeAspect::Points,
      std::vector<ElementRange>{{1, 1}});
  extractor.Apply(world, world.Commit());
  const auto& slid = extractor.snapshot()->geometries[0];
  assert(slid.vertex_ranges.size() == 1 && slid.vertex_ranges[0].first == 1 &&
         slid.vertex_ranges[0].count == 1);
  const auto vertex_revision = slid.vertex_revision;
  mesh.indices = {1, 2, 0, 2, 3, 0};
  world.UpdateMesh(mesh_handle, mesh, ChangeAspect::Topology);
  extractor.Apply(world, world.Commit());
  assert(extractor.snapshot()->geometries[0].vertex_revision ==
         vertex_revision);

  // Moving one point out of the plane re-derives its neighbors' generated
  // normals, so the range covers every vertex rather than the caller's one.
  mesh.positions[2].z = 1.0F;
  world.UpdateMesh(mesh_handle, mesh, ChangeAspect::Points,
      std::vector<ElementRange>{{2, 1}});
  extractor.Apply(world, world.Commit());
  const auto& moved = extractor.snapshot()->geometries[0];
  assert(moved.vertex_ranges.size() == 1 && moved.vertex_ranges[0].first == 0 &&
         moved.vertex_ranges[0].count == 4);
  assert(!Near((*moved.vertices)[0].normal, {0, 0, 1}));

  // Authored normals keep the caller's ranges and are renormalized.
  mesh.normals.assign(4, {0, 0, 2});
  world.UpdateMesh(mesh_handle, mesh, ChangeAspect::Primvars);
  extractor.Apply(world, world.Commit());
  const auto& authored = extractor.snapshot()->geometries[0];
  assert(authored.has_normals && !authored.generated_normals &&
         authored.degenerate_normal_count == 0);
  assert(Near((*authored.vertices)[3].normal, {0, 0, 1}));
  mesh.normals[1] = {};
  world.UpdateMesh(mesh_handle, mesh, ChangeAspect::Primvars);
  extractor.Apply(world, world.Commit());
  assert(extractor.snapshot()->geometries[0].degenerate_normal_count == 1);
}

} // namespace

int main() {
  GeneratedNormals();
  AuthoredInterpolation();
  DegenerateAndMalformed();
  VertexNormals();
  Orientation();
  SurfacePolicy();
  Extraction();
  return 0;
}
