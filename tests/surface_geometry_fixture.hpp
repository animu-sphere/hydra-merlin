#pragma once

#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/render/backend.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace merlin::tests {

// Shared native surface-geometry image gate. Each case renders one patch
// through the identity orthographic camera and compares the debug views at
// pixel centers with an independent double-precision oracle: Ng and Ns_base
// from the inverse-transpose of the instance transform, the hemisphere rule
// for Ns, and the expected facing or culling.
struct SurfaceGeometryReport {
  std::size_t cases{};
  std::size_t samples{};
  int maximum_channel_error{};
};

namespace surface_fixture {

using Double3 = std::array<double, 3>;

inline Double3 Normalize(Double3 value) {
  const double length = std::hypot(value[0], value[1], value[2]);
  return {value[0] / length, value[1] / length, value[2] / length};
}

// Column-major 4x4 composed from the transform helpers below.
inline Mat4 Multiply(const Mat4& lhs, const Mat4& rhs) {
  Mat4 result;
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      float sum{};
      for (int inner = 0; inner < 4; ++inner) {
        sum += lhs.values[inner * 4 + row] * rhs.values[column * 4 + inner];
      }
      result.values[column * 4 + row] = sum;
    }
  }
  return result;
}

inline Mat4 Scale(float x, float y, float z) {
  Mat4 result;
  result.values[0] = x;
  result.values[5] = y;
  result.values[10] = z;
  return result;
}

inline Mat4 Translate(float x, float y, float z) {
  Mat4 result;
  result.values[12] = x;
  result.values[13] = y;
  result.values[14] = z;
  return result;
}

inline Mat4 RotateX(double degrees) {
  const auto radians = degrees * 3.141592653589793 / 180.0;
  Mat4 result;
  result.values[5] = static_cast<float>(std::cos(radians));
  result.values[6] = static_cast<float>(std::sin(radians));
  result.values[9] = static_cast<float>(-std::sin(radians));
  result.values[10] = static_cast<float>(std::cos(radians));
  return result;
}

inline Mat4 RotateZ(double degrees) {
  const auto radians = degrees * 3.141592653589793 / 180.0;
  Mat4 result;
  result.values[0] = static_cast<float>(std::cos(radians));
  result.values[1] = static_cast<float>(std::sin(radians));
  result.values[4] = static_cast<float>(-std::sin(radians));
  result.values[5] = static_cast<float>(std::cos(radians));
  return result;
}

// inverse-transpose(M3) * n, independent of the renderer's cofactor form.
inline Double3 TransformNormal(const Mat4& transform, Double3 normal) {
  const auto m = [&](int row, int column) {
    return static_cast<double>(transform.values[column * 4 + row]);
  };
  const double a = m(0, 0), b = m(0, 1), c = m(0, 2);
  const double d = m(1, 0), e = m(1, 1), f = m(1, 2);
  const double g = m(2, 0), h = m(2, 1), i = m(2, 2);
  const double determinant =
      a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  // inverse(M)^T equals cofactor(M) / det(M).
  const std::array<std::array<double, 3>, 3> inverse_transpose{{
      {(e * i - f * h) / determinant, -(d * i - f * g) / determinant,
          (d * h - e * g) / determinant},
      {-(b * i - c * h) / determinant, (a * i - c * g) / determinant,
          -(a * h - b * g) / determinant},
      {(b * f - c * e) / determinant, -(a * f - c * d) / determinant,
          (a * e - b * d) / determinant},
  }};
  Double3 result{};
  for (int row = 0; row < 3; ++row) {
    result[row] = inverse_transpose[row][0] * normal[0] +
                  inverse_transpose[row][1] * normal[1] +
                  inverse_transpose[row][2] * normal[2];
  }
  return Normalize(result);
}

// Shader hemisphere rule: a shading normal behind Ng is projected to Ng's
// plane and nudged toward Ng by 1e-3.
inline Double3 ResolveShading(Double3 shading, Double3 geometric) {
  const double alignment = shading[0] * geometric[0] +
                           shading[1] * geometric[1] +
                           shading[2] * geometric[2];
  if (alignment >= 0.0) {
    return shading;
  }
  const double offset = alignment - 1.0e-3;
  return Normalize({shading[0] - offset * geometric[0],
      shading[1] - offset * geometric[1], shading[2] - offset * geometric[2]});
}

struct Sample {
  std::uint32_t column{32};
  std::uint32_t row{32};
  Double3 geometric_normal;
  Double3 shading_normal_base;
  bool front_facing{true};
};

struct Case {
  std::string name;
  MeshDescriptor mesh;
  Mat4 transform;
  bool material_double_sided{};
  // Empty for a single-sided back face, which must stay uncovered.
  std::vector<Sample> samples;
};

inline MeshDescriptor Patch(std::vector<Vec3> normals, bool reversed = false) {
  MeshDescriptor mesh;
  mesh.positions = {{-0.7F, -0.7F, 0.0F}, {0.7F, -0.7F, 0.0F},
      {0.7F, 0.7F, 0.0F}, {-0.7F, 0.7F, 0.0F}};
  mesh.normals = std::move(normals);
  mesh.indices = reversed ? std::vector<std::uint32_t>{0, 2, 1, 0, 3, 2}
                          : std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3};
  return mesh;
}

inline std::vector<Case> Cases() {
  const Double3 plus_z{0, 0, 1};
  const Double3 tilted{0.6, 0, 0.8};
  const auto depth = Translate(0, 0, 0.5F);
  std::vector<Case> cases;
  cases.push_back({"authored", Patch(std::vector<Vec3>(4, {0, 0, 1})), depth,
      false, {{32, 32, plus_z, plus_z}}});
  // Native meshes without normals use the shared smooth fallback, not +Z by
  // default: tilting the patch must tilt the generated normal with it.
  cases.push_back({"generated", Patch({}), Multiply(depth, RotateX(30)),
      false,
      {{32, 32, TransformNormal(RotateX(30), plus_z),
          TransformNormal(RotateX(30), plus_z)}}});
  cases.push_back({"authored-tilted", Patch(std::vector<Vec3>(4, {0.6F, 0, 0.8F})),
      depth, false, {{32, 32, plus_z, tilted}}});
  cases.push_back({"authored-behind",
      Patch(std::vector<Vec3>(4, {0.6F, 0, -0.8F})), depth, false,
      {{32, 32, plus_z, {0.6, 0, -0.8}}}});
  // Vertex interpolation: normals vary linearly in x, so every pixel has an
  // exact affine oracle.
  {
    Case interpolated{"vertex-interpolation",
        Patch({{-0.6F, 0, 0.8F}, {0.6F, 0, 0.8F}, {0.6F, 0, 0.8F},
            {-0.6F, 0, 0.8F}}),
        depth, false, {}};
    for (const std::uint32_t column : {16U, 32U, 48U}) {
      const double x = (column + 0.5) / 32.0 - 1.0;
      interpolated.samples.push_back(
          {column, 32, plus_z, Normalize({0.6 * x / 0.7, 0, 0.8})});
    }
    cases.push_back(std::move(interpolated));
  }
  const auto uniform = Scale(2, 2, 2);
  cases.push_back({"uniform-scale", Patch(std::vector<Vec3>(4, {0.6F, 0, 0.8F})),
      Multiply(depth, uniform), false,
      {{32, 32, plus_z, TransformNormal(uniform, tilted)}}});
  const auto non_uniform = Multiply(RotateX(30), Scale(2, 0.5F, 1));
  cases.push_back({"rotated-non-uniform",
      Patch(std::vector<Vec3>(4, {0.6F, 0, 0.8F})), Multiply(depth, non_uniform),
      false,
      {{32, 32, TransformNormal(non_uniform, plus_z),
          TransformNormal(non_uniform, tilted)}}});
  // A mirrored instance reverses its winding so it stays front-facing and
  // covered while single-sided.
  const auto mirror_x = Scale(-1, 1, 1);
  cases.push_back({"negative-scale", Patch(std::vector<Vec3>(4, {0.6F, 0, 0.8F})),
      Multiply(depth, mirror_x), false,
      {{32, 32, TransformNormal(mirror_x, plus_z),
          TransformNormal(mirror_x, tilted)}}});
  // Nested parent * child with a mirrored, non-uniform child.
  const auto nested = Multiply(RotateZ(90), Scale(0.5F, -2, 1));
  cases.push_back({"nested-negative", Patch(std::vector<Vec3>(4, {0.6F, 0, 0.8F})),
      Multiply(depth, nested), false,
      {{32, 32, TransformNormal(nested, plus_z),
          TransformNormal(nested, tilted)}}});
  // Mirroring Z turns the outward side away from the viewer: culled when
  // single-sided; when double-sided, back-facing with both normals flipped.
  const auto mirror_z = Scale(1, 1, -1);
  cases.push_back({"negative-z-single-sided",
      Patch(std::vector<Vec3>(4, {0, 0, 1})), Multiply(depth, mirror_z), false,
      {}});
  cases.push_back({"negative-z-double-sided-material",
      Patch(std::vector<Vec3>(4, {0, 0, 1})), Multiply(depth, mirror_z), true,
      {{32, 32, plus_z, plus_z, false}}});
  // Reversed (leftHanded) winding with normals authored for that outward side.
  cases.push_back({"reversed-single-sided",
      Patch(std::vector<Vec3>(4, {0, 0, -1}), true), depth, false, {}});
  auto double_sided_mesh = Patch(std::vector<Vec3>(4, {0.6F, 0, -0.8F}), true);
  double_sided_mesh.double_sided = true;
  cases.push_back({"reversed-double-sided-mesh", double_sided_mesh, depth,
      false, {{32, 32, plus_z, {-0.6, 0, 0.8}, false}}});
  return cases;
}

} // namespace surface_fixture

// Render(snapshot, view) returns a result with color/depth CPU products.
template <typename Render>
SurfaceGeometryReport VerifySurfaceGeometryViews(Render render) {
  using surface_fixture::Double3;
  const auto require = [](bool condition, const std::string& message) {
    if (!condition) {
      throw std::runtime_error("surface geometry fixture: " + message);
    }
  };
  RenderWorld world;
  extraction::SceneExtractor extractor;
  const auto cases = surface_fixture::Cases();
  MaterialDescriptor material;
  material.features = MaterialFeature::DirectionalLight;
  const auto material_handle = world.CreateMaterial(material);
  InstanceDescriptor instance;
  instance.mesh = world.CreateMesh(cases.front().mesh);
  instance.material = material_handle;
  const auto instance_handle = world.CreateInstance(instance);
  extractor.SetActiveCamera(world.CreateCamera({}));

  SurfaceGeometryReport report;
  const auto encode = [](double value) {
    return static_cast<int>(std::lround(255.0 * (0.5 * value + 0.5)));
  };
  for (const auto& test : cases) {
    world.UpdateMesh(instance.mesh, test.mesh);
    material.double_sided = test.material_double_sided;
    world.UpdateMaterial(material_handle, material);
    instance.transform = test.transform;
    world.UpdateInstance(instance_handle, instance, ChangeAspect::Transform);
    extractor.Apply(world, world.Commit());
    const auto snapshot = extractor.snapshot();
    ++report.cases;
    for (const auto view : {render::DebugView::GeometricNormal,
             render::DebugView::ShadingNormalBase, render::DebugView::Normal,
             render::DebugView::FrontFacing}) {
      const auto result = render(snapshot, view);
      const auto repeated = render(snapshot, view);
      require(result.color.pixels == repeated.color.pixels &&
                  result.depth.pixels == repeated.depth.pixels,
          test.name + " " + std::string(render::DebugViewName(view)) +
              " repeated frame differs");
      const auto width = result.color.product.width;
      const auto covered = [&](std::uint32_t column, std::uint32_t row) {
        return result.depth.pixels[row * width + column] < 1.0F;
      };
      if (test.samples.empty()) {
        require(!covered(32, 32), test.name + " back face was not culled");
        continue;
      }
      for (const auto& sample : test.samples) {
        require(covered(sample.column, sample.row),
            test.name + " sample is uncovered");
        Double3 expected{};
        switch (view) {
        case render::DebugView::GeometricNormal:
          expected = sample.geometric_normal;
          break;
        case render::DebugView::ShadingNormalBase:
          expected = sample.shading_normal_base;
          break;
        case render::DebugView::Normal:
          expected = surface_fixture::ResolveShading(
              sample.shading_normal_base, sample.geometric_normal);
          break;
        default:
          expected = sample.front_facing ? Double3{1, 1, 1}
                                         : Double3{-1, -1, -1};
          break;
        }
        const auto* pixel = result.color.pixels.data() +
                            sample.row * result.color.row_pitch_bytes +
                            sample.column * 4U;
        for (std::size_t channel = 0; channel < 3; ++channel) {
          const int error =
              std::abs(static_cast<int>(pixel[channel]) - encode(expected[channel]));
          report.maximum_channel_error =
              std::max(report.maximum_channel_error, error);
          require(error <= 1, test.name + " " +
                                  std::string(render::DebugViewName(view)) +
                                  " differs from the oracle at column " +
                                  std::to_string(sample.column) + " channel " +
                                  std::to_string(channel) + ": " +
                                  std::to_string(pixel[channel]) + " vs " +
                                  std::to_string(encode(expected[channel])));
        }
        ++report.samples;
      }
    }
  }
  return report;
}

} // namespace merlin::tests
