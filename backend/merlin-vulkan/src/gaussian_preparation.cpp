#include "gaussian_preparation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace merlin::vulkan::detail {
namespace {

constexpr float kProjectionEpsilon = 1.0e-6F;

struct Matrix3 {
  std::array<float, 9> values{};
};

struct ProjectedPoint {
  Vec3 camera;
  Vec3 ndc;
  float clip_w{};
};

float Dot(Vec3 lhs, Vec3 rhs) {
  return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

Vec3 Cross(Vec3 lhs, Vec3 rhs) {
  return {lhs.y * rhs.z - lhs.z * rhs.y,
          lhs.z * rhs.x - lhs.x * rhs.z,
          lhs.x * rhs.y - lhs.y * rhs.x};
}

float LengthSquared(Vec3 value) { return Dot(value, value); }

Vec3 Normalize(Vec3 value) {
  const auto length_squared = LengthSquared(value);
  if (!std::isfinite(length_squared) ||
      length_squared <= kProjectionEpsilon * kProjectionEpsilon) {
    return {};
  }
  const auto inverse_length = 1.0F / std::sqrt(length_squared);
  return {value.x * inverse_length, value.y * inverse_length,
          value.z * inverse_length};
}

bool IsFinite(Vec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

Mat4 Multiply(const Mat4& lhs, const Mat4& rhs) {
  Mat4 result;
  result.values.fill(0.0F);
  for (std::size_t column = 0; column < 4; ++column) {
    for (std::size_t row = 0; row < 4; ++row) {
      for (std::size_t inner = 0; inner < 4; ++inner) {
        result.values[column * 4 + row] +=
            lhs.values[inner * 4 + row] *
            rhs.values[column * 4 + inner];
      }
    }
  }
  return result;
}

Vec4 TransformPoint4(const Mat4& transform, Vec3 point) {
  return {
      transform.values[0] * point.x + transform.values[4] * point.y +
          transform.values[8] * point.z + transform.values[12],
      transform.values[1] * point.x + transform.values[5] * point.y +
          transform.values[9] * point.z + transform.values[13],
      transform.values[2] * point.x + transform.values[6] * point.y +
          transform.values[10] * point.z + transform.values[14],
      transform.values[3] * point.x + transform.values[7] * point.y +
          transform.values[11] * point.z + transform.values[15]};
}

Matrix3 LinearPart(const Mat4& transform) {
  Matrix3 result;
  for (std::size_t row = 0; row < 3; ++row) {
    for (std::size_t column = 0; column < 3; ++column) {
      result.values[row * 3 + column] =
          transform.values[column * 4 + row];
    }
  }
  return result;
}

Matrix3 CovarianceMatrix(const Covariance3& covariance) {
  return {{{covariance.xx, covariance.xy, covariance.xz,
            covariance.xy, covariance.yy, covariance.yz,
            covariance.xz, covariance.yz, covariance.zz}}};
}

Matrix3 TransformCovariance(const Matrix3& transform,
                            const Matrix3& covariance) {
  Matrix3 intermediate;
  Matrix3 result;
  for (std::size_t row = 0; row < 3; ++row) {
    for (std::size_t column = 0; column < 3; ++column) {
      for (std::size_t inner = 0; inner < 3; ++inner) {
        intermediate.values[row * 3 + column] +=
            transform.values[row * 3 + inner] *
            covariance.values[inner * 3 + column];
      }
    }
  }
  for (std::size_t row = 0; row < 3; ++row) {
    for (std::size_t column = 0; column < 3; ++column) {
      for (std::size_t inner = 0; inner < 3; ++inner) {
        result.values[row * 3 + column] +=
            intermediate.values[row * 3 + inner] *
            transform.values[column * 3 + inner];
      }
    }
  }
  return result;
}

Vec3 Multiply(const Matrix3& matrix, Vec3 value) {
  return {
      matrix.values[0] * value.x + matrix.values[1] * value.y +
          matrix.values[2] * value.z,
      matrix.values[3] * value.x + matrix.values[4] * value.y +
          matrix.values[5] * value.z,
      matrix.values[6] * value.x + matrix.values[7] * value.y +
          matrix.values[8] * value.z};
}

bool Invert(const Matrix3& matrix, Matrix3& inverse) {
  const auto& m = matrix.values;
  const auto c00 = m[4] * m[8] - m[5] * m[7];
  const auto c01 = m[5] * m[6] - m[3] * m[8];
  const auto c02 = m[3] * m[7] - m[4] * m[6];
  const auto determinant = m[0] * c00 + m[1] * c01 + m[2] * c02;
  if (!std::isfinite(determinant) ||
      std::abs(determinant) <= kProjectionEpsilon) {
    return false;
  }
  const auto scale = 1.0F / determinant;
  inverse.values = {
      c00 * scale,
      (m[2] * m[7] - m[1] * m[8]) * scale,
      (m[1] * m[5] - m[2] * m[4]) * scale,
      c01 * scale,
      (m[0] * m[8] - m[2] * m[6]) * scale,
      (m[2] * m[3] - m[0] * m[5]) * scale,
      c02 * scale,
      (m[1] * m[6] - m[0] * m[7]) * scale,
      (m[0] * m[4] - m[1] * m[3]) * scale};
  return true;
}

ProjectedPoint ProjectPoint(const Mat4& local_to_camera,
                            const Mat4& projection, Vec3 position) {
  const auto camera4 = TransformPoint4(local_to_camera, position);
  const Vec3 camera{camera4.x, camera4.y, camera4.z};
  const auto clip = TransformPoint4(projection, camera);
  if (!IsFinite(camera) || !std::isfinite(clip.x) ||
      !std::isfinite(clip.y) || !std::isfinite(clip.z) ||
      !std::isfinite(clip.w) || clip.w <= kProjectionEpsilon) {
    return {camera, {}, clip.w};
  }
  const auto inverse_w = 1.0F / clip.w;
  return {camera,
          {clip.x * inverse_w, clip.y * inverse_w, clip.z * inverse_w},
          clip.w};
}

std::array<Vec3, 2> PerspectiveJacobian(const Mat4& projection,
                                        Vec3 camera, float clip_w) {
  const auto clip = TransformPoint4(projection, camera);
  const Vec3 row_x{projection.values[0], projection.values[4],
                   projection.values[8]};
  const Vec3 row_y{projection.values[1], projection.values[5],
                   projection.values[9]};
  const Vec3 row_w{projection.values[3], projection.values[7],
                   projection.values[11]};
  const auto inverse_w_squared = 1.0F / (clip_w * clip_w);
  const auto quotient_row = [&](Vec3 numerator_row, float numerator) {
    return Vec3{
        (numerator_row.x * clip_w - numerator * row_w.x) *
            inverse_w_squared,
        (numerator_row.y * clip_w - numerator * row_w.y) *
            inverse_w_squared,
        (numerator_row.z * clip_w - numerator * row_w.z) *
            inverse_w_squared};
  };
  return {quotient_row(row_x, clip.x), quotient_row(row_y, clip.y)};
}

std::array<Vec3, 2> TangentialJacobian(const Mat4& projection,
                                       Vec3 camera) {
  const auto distance = std::sqrt(LengthSquared(camera));
  const auto direction = Normalize(camera);
  if (distance <= kProjectionEpsilon || !IsFinite(direction)) {
    return {};
  }
  auto tangent_x = Normalize(
      {1.0F - direction.x * direction.x,
       -direction.x * direction.y, -direction.x * direction.z});
  if (LengthSquared(tangent_x) <= kProjectionEpsilon * kProjectionEpsilon) {
    tangent_x = Normalize({-direction.y * direction.x,
                           1.0F - direction.y * direction.y,
                           -direction.y * direction.z});
  }
  auto tangent_y = Normalize(Cross(tangent_x, direction));
  if (tangent_y.y < 0.0F) {
    tangent_y = {-tangent_y.x, -tangent_y.y, -tangent_y.z};
  }
  const auto focal_x = std::max(
      std::abs(projection.values[0]), kProjectionEpsilon);
  const auto focal_y = std::max(
      std::abs(projection.values[5]), kProjectionEpsilon);
  const auto x_scale = focal_x / distance;
  const auto y_scale = focal_y / distance;
  return {{{tangent_x.x * x_scale, tangent_x.y * x_scale,
            tangent_x.z * x_scale},
           {tangent_y.x * y_scale, tangent_y.y * y_scale,
            tangent_y.z * y_scale}}};
}

bool ProjectCovariance(const Matrix3& camera_covariance,
                       const std::array<Vec3, 2>& jacobian,
                       const GaussianPreparationOptions& options,
                       Vec3& inverse_conic, float& radius_pixels) {
  const auto covariance = [&](Vec3 lhs, Vec3 rhs) {
    return Dot(lhs, Multiply(camera_covariance, rhs));
  };
  const auto width_scale = static_cast<float>(options.width) * 0.5F;
  const auto height_scale = static_cast<float>(options.height) * 0.5F;
  auto xx = covariance(jacobian[0], jacobian[0]) *
            width_scale * width_scale;
  auto xy = covariance(jacobian[0], jacobian[1]) *
            width_scale * height_scale;
  auto yy = covariance(jacobian[1], jacobian[1]) *
            height_scale * height_scale;
  xx += options.minimum_variance_pixels;
  yy += options.minimum_variance_pixels;
  const auto determinant = xx * yy - xy * xy;
  if (!std::isfinite(xx) || !std::isfinite(xy) || !std::isfinite(yy) ||
      !std::isfinite(determinant) || determinant <= kProjectionEpsilon) {
    return false;
  }
  const auto discriminant =
      std::sqrt(std::max(0.0F, (xx - yy) * (xx - yy) + 4.0F * xy * xy));
  const auto largest_eigenvalue = 0.5F * (xx + yy + discriminant);
  radius_pixels = options.sigma_extent *
                  std::sqrt(std::max(largest_eigenvalue, 0.0F));
  inverse_conic = {yy / determinant, -xy / determinant, xx / determinant};
  return std::isfinite(radius_pixels) && radius_pixels > 0.0F &&
         IsFinite(inverse_conic);
}

Vec3 LocalCameraDirection(const Matrix3& local_to_camera, Vec3 camera_point) {
  Matrix3 camera_to_local;
  if (!Invert(local_to_camera, camera_to_local)) {
    return Normalize(camera_point);
  }
  return Normalize(Multiply(camera_to_local, camera_point));
}

void AddScaled(Vec3& result, Vec3 coefficient, float basis) {
  result.x += coefficient.x * basis;
  result.y += coefficient.y * basis;
  result.z += coefficient.z * basis;
}

}  // namespace

Vec3 EvaluateGaussianRadiance(std::span<const Vec3> coefficients,
                              std::uint32_t degree,
                              Vec3 camera_to_particle_direction) {
  constexpr float c0 = 0.28209479177387814F;
  constexpr float c1 = 0.4886025119029199F;
  constexpr std::array<float, 5> c2{
      1.0925484305920792F, 1.0925484305920792F,
      0.31539156525252005F, 1.0925484305920792F,
      0.5462742152960396F};
  constexpr std::array<float, 7> c3{
      -0.5900435899266435F, 2.890611442640554F,
      -0.4570457994644658F, 0.3731763325901154F,
      -0.4570457994644658F, 1.445305721320277F,
      -0.5900435899266435F};

  const auto direction = Normalize(camera_to_particle_direction);
  if (coefficients.empty() || !IsFinite(direction)) {
    return {0.5F, 0.5F, 0.5F};
  }
  const auto supported_degree = std::min(degree, 3U);
  const auto required = static_cast<std::size_t>(supported_degree + 1U) *
                        (supported_degree + 1U);
  if (coefficients.size() < required) {
    return {0.5F, 0.5F, 0.5F};
  }

  Vec3 result{0.5F, 0.5F, 0.5F};
  AddScaled(result, coefficients[0], c0);
  if (supported_degree >= 1U) {
    AddScaled(result, coefficients[1], -c1 * direction.y);
    AddScaled(result, coefficients[2], c1 * direction.z);
    AddScaled(result, coefficients[3], -c1 * direction.x);
  }
  const auto xx = direction.x * direction.x;
  const auto yy = direction.y * direction.y;
  const auto zz = direction.z * direction.z;
  if (supported_degree >= 2U) {
    AddScaled(result, coefficients[4], c2[0] * direction.x * direction.y);
    AddScaled(result, coefficients[5], c2[1] * direction.y * direction.z);
    AddScaled(result, coefficients[6], c2[2] * (2.0F * zz - xx - yy));
    AddScaled(result, coefficients[7], c2[3] * direction.x * direction.z);
    AddScaled(result, coefficients[8], c2[4] * (xx - yy));
  }
  if (supported_degree >= 3U) {
    AddScaled(result, coefficients[9],
              c3[0] * direction.y * (3.0F * xx - yy));
    AddScaled(result, coefficients[10],
              c3[1] * direction.x * direction.y * direction.z);
    AddScaled(result, coefficients[11],
              c3[2] * direction.y * (4.0F * zz - xx - yy));
    AddScaled(result, coefficients[12],
              c3[3] * direction.z * (2.0F * zz - 3.0F * xx - 3.0F * yy));
    AddScaled(result, coefficients[13],
              c3[4] * direction.x * (4.0F * zz - xx - yy));
    AddScaled(result, coefficients[14],
              c3[5] * direction.z * (xx - yy));
    AddScaled(result, coefficients[15],
              c3[6] * direction.x * (xx - 3.0F * yy));
  }
  return {std::max(result.x, 0.0F), std::max(result.y, 0.0F),
          std::max(result.z, 0.0F)};
}

GaussianPreparationResult PrepareGaussianFrame(
    const extraction::FrameSnapshot& snapshot,
    const GaussianPreparationOptions& options) {
  GaussianPreparationResult result;
  if (options.width == 0U || options.height == 0U ||
      !std::isfinite(options.sigma_extent) || options.sigma_extent <= 0.0F ||
      !std::isfinite(options.minimum_variance_pixels) ||
      options.minimum_variance_pixels < 0.0F) {
    return result;
  }

  std::size_t total_particles{};
  for (const auto& record : snapshot.gaussians) {
    total_particles += record.positions ? record.positions->size() : 0U;
  }
  result.gaussians.reserve(total_particles);

  for (const auto& record : snapshot.gaussians) {
    const auto count = record.positions ? record.positions->size() : 0U;
    result.counters.candidate_count += count;
    if (!record.visible) {
      result.counters.hidden_count += count;
      continue;
    }
    const auto coefficients_per_particle =
        static_cast<std::size_t>(record.spherical_harmonics_degree + 1U) *
        (record.spherical_harmonics_degree + 1U);
    if (!record.covariances || !record.opacities ||
        !record.spherical_harmonics_coefficients ||
        record.covariances->size() != count ||
        record.opacities->size() != count ||
        coefficients_per_particle >
            std::numeric_limits<std::size_t>::max() /
                std::max(count, std::size_t{1}) ||
        record.spherical_harmonics_coefficients->size() !=
            count * coefficients_per_particle) {
      result.counters.invalid_culled_count += count;
      continue;
    }

    const auto local_to_camera = Multiply(snapshot.view, record.transform);
    const auto local_to_camera_linear = LinearPart(local_to_camera);
    for (std::size_t particle = 0; particle < count; ++particle) {
      const auto opacity = (*record.opacities)[particle];
      if (!std::isfinite(opacity) || opacity <= 0.0F) {
        ++result.counters.opacity_culled_count;
        continue;
      }
      const auto projected = ProjectPoint(
          local_to_camera, snapshot.projection, (*record.positions)[particle]);
      if (projected.clip_w <= kProjectionEpsilon ||
          !IsFinite(projected.camera) || !IsFinite(projected.ndc) ||
          projected.ndc.z < 0.0F || projected.ndc.z > 1.0F) {
        ++result.counters.frustum_culled_count;
        continue;
      }
      const auto camera_covariance = TransformCovariance(
          local_to_camera_linear,
          CovarianceMatrix((*record.covariances)[particle]));
      const auto jacobian =
          record.projection_mode == GaussianProjectionMode::Tangential
              ? TangentialJacobian(snapshot.projection, projected.camera)
              : PerspectiveJacobian(snapshot.projection, projected.camera,
                                    projected.clip_w);
      Vec3 inverse_conic;
      float radius_pixels{};
      if (!ProjectCovariance(camera_covariance, jacobian, options,
                             inverse_conic, radius_pixels)) {
        ++result.counters.invalid_culled_count;
        continue;
      }
      const Vec2 center{
          (projected.ndc.x * 0.5F + 0.5F) * options.width,
          (projected.ndc.y * 0.5F + 0.5F) * options.height};
      if (center.x + radius_pixels < 0.0F ||
          center.x - radius_pixels > options.width ||
          center.y + radius_pixels < 0.0F ||
          center.y - radius_pixels > options.height) {
        ++result.counters.frustum_culled_count;
        continue;
      }
      const auto coefficient_offset = particle * coefficients_per_particle;
      const auto coefficient_span = std::span<const Vec3>(
          *record.spherical_harmonics_coefficients)
                                        .subspan(coefficient_offset,
                                                 coefficients_per_particle);
      const auto local_direction = LocalCameraDirection(
          local_to_camera_linear, projected.camera);
      const auto sort_key =
          record.sorting_mode == GaussianSortingMode::CameraDistance
              ? LengthSquared(projected.camera)
              : projected.ndc.z;
      result.gaussians.push_back(
          {record.gaussian, static_cast<std::uint32_t>(particle), center,
           inverse_conic,
           EvaluateGaussianRadiance(coefficient_span,
                                    record.spherical_harmonics_degree,
                                    local_direction),
           opacity, radius_pixels, projected.ndc.z, sort_key});
    }
  }

  std::sort(result.gaussians.begin(), result.gaussians.end(),
            [](const PreparedGaussian& lhs, const PreparedGaussian& rhs) {
              if (lhs.sort_key != rhs.sort_key) {
                return lhs.sort_key > rhs.sort_key;
              }
              if (lhs.resource != rhs.resource) {
                return lhs.resource < rhs.resource;
              }
              return lhs.particle < rhs.particle;
            });
  result.counters.visible_count = result.gaussians.size();
  result.counters.sorted_count = result.gaussians.size();
  return result;
}

}  // namespace merlin::vulkan::detail
