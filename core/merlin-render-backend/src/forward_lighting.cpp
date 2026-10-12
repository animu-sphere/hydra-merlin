#include <merlin/render/forward_lighting.hpp>
#include <merlin/render/backend.hpp>

#include <cmath>

namespace merlin::render {

ForwardDirectionalLighting ExtractForwardDirectionalLighting(
    const extraction::FrameSnapshot& snapshot) {
  const extraction::LightRecord* selected = nullptr;
  for (const auto& light : snapshot.lights) {
    if (light.type == LightType::Directional &&
        (selected == nullptr || light.light < selected->light)) {
      selected = &light;
    }
  }
  ForwardDirectionalLighting result;
  if (selected == nullptr) {
    return result;
  }
  const auto nonnegative_finite = [](float value) {
    return std::isfinite(value) && value >= 0.0F;
  };
  if (!nonnegative_finite(selected->intensity) ||
      !nonnegative_finite(selected->color.x) ||
      !nonnegative_finite(selected->color.y) ||
      !nonnegative_finite(selected->color.z)) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        "extract Forward directional lighting",
        "Directional light " + std::to_string(selected->light) +
            " requires finite, nonnegative color and intensity.");
  }
  // Normalize in double precision so finite float axes at either end of the
  // range do not overflow/underflow when squared.
  const double x = selected->transform.values[8];
  const double y = selected->transform.values[9];
  const double z = selected->transform.values[10];
  const auto length = std::hypot(x, y, z);
  if (std::isfinite(length) && length > 0.0) {
    result.direction_intensity = {static_cast<float>(x / length),
        static_cast<float>(y / length), static_cast<float>(z / length),
        selected->intensity};
  } else {
    result.direction_intensity.w = selected->intensity;
  }
  result.color = selected->color;
  result.light = selected->light;
  return result;
}

Mat4 MultiplyForwardMatrices(const Mat4& lhs, const Mat4& rhs) {
  Mat4 result;
  result.values.fill(0.0F);
  for (std::size_t column = 0; column < 4; ++column) {
    for (std::size_t row = 0; row < 4; ++row) {
      for (std::size_t inner = 0; inner < 4; ++inner) {
        result.values[column * 4U + row] +=
            lhs.values[inner * 4U + row] * rhs.values[column * 4U + inner];
      }
    }
  }
  return result;
}

std::array<Vec4, 3> ForwardNormalMatrix(const Mat4& transform) {
  const Vec3 column0{transform.values[0], transform.values[1],
      transform.values[2]};
  const Vec3 column1{transform.values[4], transform.values[5],
      transform.values[6]};
  const Vec3 column2{transform.values[8], transform.values[9],
      transform.values[10]};
  const auto cross = [](const Vec3& lhs, const Vec3& rhs) {
    return Vec3{lhs.y * rhs.z - lhs.z * rhs.y, lhs.z * rhs.x - lhs.x * rhs.z,
        lhs.x * rhs.y - lhs.y * rhs.x};
  };
  const auto cofactor0 = cross(column1, column2);
  const auto cofactor1 = cross(column2, column0);
  const auto cofactor2 = cross(column0, column1);
  const auto determinant = column0.x * cofactor0.x + column0.y * cofactor0.y +
                           column0.z * cofactor0.z;
  if (std::abs(determinant) <= 1.0e-20F) {
    return {Vec4{1.0F, 0.0F, 0.0F, 0.0F}, Vec4{0.0F, 1.0F, 0.0F, 0.0F},
        Vec4{0.0F, 0.0F, 1.0F, 0.0F}};
  }
  const auto scale = 1.0F / determinant;
  return {
      Vec4{cofactor0.x * scale, cofactor0.y * scale, cofactor0.z * scale, 0.0F},
      Vec4{cofactor1.x * scale, cofactor1.y * scale, cofactor1.z * scale, 0.0F},
      Vec4{cofactor2.x * scale, cofactor2.y * scale, cofactor2.z * scale,
          0.0F}};
}

} // namespace merlin::render
