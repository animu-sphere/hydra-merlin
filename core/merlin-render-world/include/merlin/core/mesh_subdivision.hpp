#pragma once

#include <array>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <merlin/core/mesh_attributes.hpp>

namespace merlin {

enum class SubdivisionScheme { None,
  CatmullClark,
  Unsupported };
enum class SubdivisionBoundary { None,
  EdgeOnly,
  EdgeAndCorner,
  Unsupported };
enum class SubdivisionFaceVarying {
  None,
  CornersOnly,
  CornersPlus1,
  CornersPlus2,
  Boundaries,
  All,
  Unsupported
};
enum class SubdivisionTriangleRule { CatmullClark,
  Smooth,
  Unsupported };
enum class SubdivisionCreaseMethod { Uniform,
  Chaikin,
  Unsupported };

struct SubdivisionTopology {
  SubdivisionScheme scheme{SubdivisionScheme::None};
  SubdivisionBoundary boundary{SubdivisionBoundary::EdgeAndCorner};
  SubdivisionFaceVarying face_varying{SubdivisionFaceVarying::CornersPlus1};
  SubdivisionTriangleRule triangle_rule{SubdivisionTriangleRule::CatmullClark};
  SubdivisionCreaseMethod crease_method{SubdivisionCreaseMethod::Uniform};
  bool left_handed{};
  std::vector<int> face_counts;
  std::vector<int> point_indices;
  std::vector<int> holes;
  std::vector<int> crease_lengths;
  std::vector<int> crease_indices;
  std::vector<float> crease_sharpness;
  std::vector<int> corner_indices;
  std::vector<float> corner_sharpness;

  bool operator==(const SubdivisionTopology&) const = default;
};

struct SubdivisionPrimvar {
  PrimvarInterpolation interpolation{PrimvarInterpolation::Constant};
  // Use x/y for UVs, xyz for color, and x for opacity.
  std::vector<Vec4> values;
  std::vector<int> indices;
};

struct SubdivisionPrimvars {
  const SubdivisionPrimvar* texcoords{};
  const SubdivisionPrimvar* colors{};
  const SubdivisionPrimvar* opacities{};
};

enum class SubdivisionStatus { Polygon,
  Refined,
  Fallback,
  Rejected };

struct SubdivisionResult {
  SubdivisionStatus status{SubdivisionStatus::Polygon};
  MeshDescriptor mesh;
  bool rebuilt_plan{};
};

// Retain one instance per mesh. Refine reuses the topology and position
// stencils across points and attribute value changes. The caller keeps its
// polygon path for Polygon and Fallback, and removes a Rejected mesh.
class MeshSubdivision {
public:
  static constexpr int kMaxLevel = 3;

  MeshSubdivision();
  ~MeshSubdivision();
  MeshSubdivision(MeshSubdivision&&) noexcept;
  MeshSubdivision& operator=(MeshSubdivision&&) noexcept;

  [[nodiscard]] SubdivisionResult Refine(const SubdivisionTopology& topology,
      std::span<const Vec3> points, const SubdivisionPrimvars& primvars,
      int level, std::string_view label, DiagnosticSink* sink = nullptr);

  // Chooses the highest level at or below the requested display level that
  // fits the working-memory budget. Level zero still uses the cage.
  [[nodiscard]] SubdivisionResult RefineForDisplay(const SubdivisionTopology& topology,
      std::span<const Vec3> points, const SubdivisionPrimvars& primvars,
      int level, std::string_view label, DiagnosticSink* sink = nullptr);

  // Refined quad boundary edges, indexed into the last successful triangle
  // stream. This view is valid until the next Refine or Reset.
  [[nodiscard]] std::span<const std::array<std::uint32_t, 2>>
  WireEdges() const noexcept;
  void Reset() noexcept;

private:
  [[nodiscard]] SubdivisionResult RefineImpl(const SubdivisionTopology& topology,
      std::span<const Vec3> points, const SubdivisionPrimvars& primvars,
      int level, std::string_view label, DiagnosticSink* sink, bool fit_budget);

  struct Plan;
  std::unique_ptr<Plan> plan_;
  bool refined_{};
};

} // namespace merlin
