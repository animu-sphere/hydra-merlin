#include <merlin/render/forward_lighting.hpp>
#include <merlin/render/backend.hpp>
#include <merlin/render/specular_environment.hpp>

#include <algorithm>
#include <cmath>

namespace merlin::render {

namespace {

ForwardDirectionalLighting PackDirectionalLight(
    const extraction::LightRecord* selected) {
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

// At a given NDC position, the viewing ray is the intersection of the
// two planes rowX - ndcX * rowW and rowY - ndcY * rowW. Expanding their
// cross product gives three coefficients linear in clip X, Y and W.
std::array<Vec4, 3> ClipToViewDirection(const Mat4& view_projection) {
  const auto row = [&](std::size_t i) {
    return Vec3{view_projection.values[i], view_projection.values[4 + i],
        view_projection.values[8 + i]};
  };
  const auto cross = [](const Vec3& a, const Vec3& b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x};
  };
  const auto x = row(0);
  const auto y = row(1);
  const auto z = row(2);
  const auto w = row(3);
  const auto center = cross(x, y);
  const auto depth = (w.x != 0.0F || w.y != 0.0F || w.z != 0.0F) ? w : z;
  const auto alignment = center.x * depth.x + center.y * depth.y +
                         center.z * depth.z;
  const auto sign = alignment > 0.0F ? -1.0F : 1.0F;
  const auto pack = [sign](const Vec3& coefficient) {
    return Vec4{sign * coefficient.x, sign * coefficient.y,
        sign * coefficient.z, 0.0F};
  };
  return {pack(cross(y, w)), pack(cross(w, x)), pack(center)};
}

} // namespace

DomeDrawConstants PackDomeDrawConstants(const ForwardDomeLight& dome) {
  return {dome.clip_to_environment, {dome.radiance_scale.x,
      dome.radiance_scale.y, dome.radiance_scale.z, 0.0F}};
}

ForwardDirectionalLighting ExtractForwardDirectionalLighting(
    const extraction::FrameSnapshot& snapshot) {
  const extraction::LightRecord* selected = nullptr;
  for (const auto& light : snapshot.lights) {
    if (light.type == LightType::Directional &&
        (selected == nullptr || light.light < selected->light)) {
      selected = &light;
    }
  }
  return PackDirectionalLight(selected);
}

ForwardLighting PrepareForwardLighting(const extraction::FrameSnapshot& snapshot,
    const std::array<Vec4, 9>& default_environment,
    const ForwardLightingSettings& settings) {
  ForwardLighting result;
  result.clip_to_view_direction =
      ClipToViewDirection(MultiplyForwardMatrices(snapshot.projection, snapshot.view));
  if (!settings.explicit_lights && !settings.dome) {
    result.primary = ExtractForwardDirectionalLighting(snapshot);
    result.environment = default_environment;
    return result;
  }

  std::vector<const extraction::LightRecord*> lights;
  for (const auto& light : snapshot.lights) {
    if (light.type == LightType::Directional) {
      lights.push_back(&light);
    }
  }
  const auto count = std::min(lights.size(), kForwardAdditionalLightCount + 1);
  std::partial_sort(lights.begin(), lights.begin() + count, lights.end(),
      [](const auto* a, const auto* b) { return a->light < b->light; });
  if (count == 0) {
    result.primary.direction_intensity.w = 0.0F;
  } else {
    result.primary = PackDirectionalLight(lights.front());
    for (std::size_t i = 1; i < count; ++i) {
      const auto light = PackDirectionalLight(lights[i]);
      result.additional_directions[i - 1] = light.direction_intensity;
      result.additional_colors[i - 1] = {
          light.color.x, light.color.y, light.color.z, 0.0F};
    }
  }
  result.policy = {settings.dome ? 1.0F : 0.0F, 1.0F,
      count > 0 ? static_cast<float>(count - 1) : 0.0F, 0.0F};
  if (settings.dome) {
    const auto& dome = *settings.dome;
    result.world_to_environment = dome.world_to_environment;
    if (dome.specular_texture.valid()) {
      const auto texture = std::find_if(snapshot.textures.begin(), snapshot.textures.end(),
          [&](const auto& record) { return record.texture == dome.specular_texture.value(); });
      if (texture == snapshot.textures.end() ||
          texture->width != kSpecularEnvironmentWidth ||
          texture->height != kSpecularEnvironmentHeight ||
          texture->format != TextureFormat::Rgba8Unorm) {
        throw RendererError(RendererErrorCode::InvalidRequest,
            "prepare Forward specular environment",
            "dome requires a specular environment atlas in the snapshot");
      }
      result.specular_texture = dome.specular_texture;
      const auto& rgb = dome.radiance_scale;
      result.specular_environment_scale = {rgb.x, rgb.y, rgb.z,
          rgb.x != 0.0F || rgb.y != 0.0F || rgb.z != 0.0F ? 1.0F : 0.0F};
    }
    for (std::size_t i = 0; i < result.environment.size(); ++i) {
      const auto& coefficient = dome.diffuse_environment[i];
      result.environment[i] = {coefficient.x * dome.radiance_scale.x,
          coefficient.y * dome.radiance_scale.y,
          coefficient.z * dome.radiance_scale.z, 0.0F};
    }
  }
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
