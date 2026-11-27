#pragma once

#include <merlin/core/types.hpp>
#include <merlin/render/output_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace merlin::tests {

const auto& ForwardCounters(const auto& frame) {
  if constexpr (requires { frame.telemetry; })
    return frame.telemetry;
  else
    return frame.counters;
}

// Shared native image gate, with an independent double-precision oracle.
// Caller supplies a lit +Z patch, no authored light, and a warmed backend.
template <typename Render>
void VerifyForwardOutputTransforms(Render render, Vec4 albedo) {
  using namespace merlin::render;
  const auto require = [](bool condition, const char* message) {
    if (!condition)
      throw std::runtime_error(message);
  };
  const auto same = [](const auto& a, const auto& b) {
    return a.color.pixels == b.color.pixels && a.depth.pixels == b.depth.pixels &&
           a.prim_id.pixels == b.prim_id.pixels &&
           a.instance_id.pixels == b.instance_id.pixels;
  };
  const auto stable = [&](const auto& frame) {
    const auto& counters = ForwardCounters(frame);
    require(counters.upload_bytes == 0 && counters.pipeline_creation_count == 0 &&
                counters.allocation_count == 0,
        "output edit recreated GPU resources");
  };
  // Warm all frame contexts, including lazily allocated IDs/depth/readbacks.
  for (int i = 0; i < 6; ++i)
    (void)render(OutputTransformSettings{});
  const auto baseline = render(OutputTransformSettings{});
  const std::array<double, 3> environment{
      0.947761416 * 0.2820947918 + 0.325469345 * 0.4886025119 -
          0.0461782999 * 0.6307831306,
      1.00647175 * 0.2820947918 + 0.341748059 * 0.4886025119 -
          0.0449634455 * 0.6307831306,
      1.12809479 * 0.2820947918 + 0.369059116 * 0.4886025119 -
          0.0430846624 * 0.6307831306};
  const std::array<double, 3> base{albedo.x, albedo.y, albedo.z};
  for (const auto mode : {ToneMapping::None, ToneMapping::Reinhard}) {
    for (const auto ev : {-32.0F, -2.0F, 0.0F, 1.0F, 4.0F, 32.0F}) {
      const OutputTransformSettings settings{ev, mode};
      const auto frame = render(settings);
      stable(frame);
      require(frame.depth.pixels == baseline.depth.pixels &&
                  frame.prim_id.pixels == baseline.prim_id.pixels &&
                  frame.instance_id.pixels == baseline.instance_id.pixels,
          "output transform changed depth or identity");
      const auto pixel = (frame.color.product.height / 2) * frame.color.row_pitch_bytes +
                         (frame.color.product.width / 2) * 4;
      for (std::size_t channel = 0; channel < 3; ++channel) {
        const double exposed = base[channel] * (0.15 * environment[channel] + 0.85) *
                               std::exp2(static_cast<double>(ev));
        const double transformed = mode == ToneMapping::None ? exposed : exposed / (1 + exposed);
        const auto expected = std::lround(255 * std::clamp(transformed, 0.0, 1.0));
        require(std::abs(static_cast<long>(frame.color.pixels[pixel + channel]) - expected) <= 1,
            "Forward output differs from exposure/tone-mapping oracle");
      }
      require(frame.color.pixels[pixel + 3] == baseline.color.pixels[pixel + 3],
          "output transform changed alpha");
      const auto repeated = render(settings);
      stable(repeated);
      require(same(frame, repeated), "output transform repeated frame differs");
    }
  }
  require(same(baseline, render(OutputTransformSettings{})),
      "restoring output settings did not restore the exact image");
}

// Render with explicit lighting and no lights, so the expected RGB depends
// only on emissive radiance and the output transform.
template <typename Render, typename SetEmission>
void VerifyForwardEmission(Render render, SetEmission set_emission) {
  using namespace merlin::render;
  const auto baseline = render(OutputTransformSettings{});
  set_emission(Vec3{0.125F, 0.5F, 2.0F});
  for (const auto mode : {ToneMapping::None, ToneMapping::Reinhard}) {
    const auto frame = render(OutputTransformSettings{1.0F, mode});
    const auto& counters = ForwardCounters(frame);
    if (counters.upload_bytes != counters.gpu_scene_upload_bytes ||
        counters.pipeline_creation_count != 0)
      throw std::runtime_error("emission edit uploaded non-table resources or created a pipeline");
    const auto pixel = (frame.color.product.height / 2) * frame.color.row_pitch_bytes +
                       (frame.color.product.width / 2) * 4;
    const std::array<double, 3> exposed{0.25, 1.0, 4.0};
    for (std::size_t channel = 0; channel < exposed.size(); ++channel) {
      const auto mapped = mode == ToneMapping::None
          ? exposed[channel]
          : exposed[channel] / (1.0 + exposed[channel]);
      const auto expected = std::lround(255 * std::min(mapped, 1.0));
      if (baseline.color.pixels[pixel + channel] != 0 ||
          std::abs(static_cast<long>(frame.color.pixels[pixel + channel]) - expected) > 1) {
        throw std::runtime_error("Forward emission differs from the scene-linear oracle");
      }
    }
    if (frame.color.pixels[pixel + 3] != baseline.color.pixels[pixel + 3] ||
        frame.depth.pixels != baseline.depth.pixels ||
        frame.prim_id.pixels != baseline.prim_id.pixels ||
        frame.instance_id.pixels != baseline.instance_id.pixels) {
      throw std::runtime_error("Forward emission changed alpha, depth or identity");
    }
  }
  set_emission(Vec3{});
  if (render(OutputTransformSettings{}).color.pixels != baseline.color.pixels)
    throw std::runtime_error("clearing Forward emission did not restore the image");
}

inline TextureDescriptor ForwardEmissiveTexture() {
  TextureDescriptor texture;
  texture.format = TextureFormat::Rgba8Srgb;
  texture.width = 2;
  texture.height = 1;
  texture.pixels = {255, 0, 0, 0, 0, 128, 255, 127};
  return texture;
}

template <typename Render, typename SetEmissiveTexture>
void VerifyForwardEmissiveTexture(Render render, SetEmissiveTexture set_texture) {
  using namespace merlin::render;
  const OutputTransformSettings settings{0.0F, ToneMapping::None};
  const auto baseline = render(settings);
  for (const auto intensity : {0.5F, 0.25F}) {
    set_texture(true, intensity);
    const auto frame = render(settings);
    const auto& counters = ForwardCounters(frame);
    if (intensity == 0.25F &&
        (counters.upload_bytes != counters.gpu_scene_upload_bytes ||
            counters.pipeline_creation_count != 0)) {
      throw std::runtime_error("emissive intensity edit rebuilt GPU resources");
    }
    constexpr double encoded_green = 128.0 / 255.0;
    const double green = std::pow((encoded_green + 0.055) / 1.055, 2.4);
    const std::array expected{std::array{1.0, 0.0, 0.0}, std::array{0.0, green, 1.0}};
    for (std::size_t i = 0; i < expected.size(); ++i) {
      const auto pixel = (frame.color.product.height / 2) * frame.color.row_pitch_bytes +
                         frame.color.product.width * (3 + 2 * i) / 8 * 4;
      for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto value = std::lround(255 * intensity * expected[i][channel]);
        if (std::abs(static_cast<long>(frame.color.pixels[pixel + channel]) - value) > 1)
          throw std::runtime_error("Forward emissive image differs from the UV0/sRGB oracle");
      }
      if (frame.color.pixels[pixel + 3] != baseline.color.pixels[pixel + 3])
        throw std::runtime_error("Forward emissive image changed alpha");
    }
    if (frame.depth.pixels != baseline.depth.pixels ||
        frame.prim_id.pixels != baseline.prim_id.pixels ||
        frame.instance_id.pixels != baseline.instance_id.pixels) {
      throw std::runtime_error("Forward emissive image changed depth or identity");
    }
  }
  set_texture(false, 0.0F);
  if (render(settings).color.pixels != baseline.color.pixels)
    throw std::runtime_error("disconnecting the emissive image did not restore the image");
}

inline TextureDescriptor ForwardOpacityTexture() {
  TextureDescriptor texture;
  texture.format = TextureFormat::Rgba8Srgb;
  texture.width = 2;
  texture.height = 1;
  // Red disappears, green remains at the default cutoff. File alpha is ignored.
  texture.pixels = {255, 0, 0, 255, 0, 250, 0, 0};
  return texture;
}

template <typename Render, typename SetOpacityTexture>
void VerifyForwardOpacityTexture(Render render, SetOpacityTexture set_texture) {
  using namespace merlin::render;
  const OutputTransformSettings settings{0.0F, ToneMapping::None};
  const auto baseline = render(settings);
  const auto width = baseline.color.product.width;
  const auto row = baseline.color.product.height / 2;
  const auto empty = row * width;
  const auto rejected = row * width + width * 3 / 8;
  const auto visible = row * width + width * 5 / 8;
  const auto alpha = row * baseline.color.row_pitch_bytes + (visible % width) * 4 + 3;
  set_texture(true, 0.5F);
  const auto masked = render(settings);
  // 0.69 lies between the raw and decoded green luminance for this texel.
  set_texture(true, baseline.color.pixels[alpha] / 255.0F * 0.69F);
  const auto higher_cutoff = render(settings);
  const auto& counters = ForwardCounters(higher_cutoff);
  if (counters.upload_bytes != counters.gpu_scene_upload_bytes ||
      counters.pipeline_creation_count != 0) {
    throw std::runtime_error("opacity cutoff edit rebuilt GPU resources");
  }

  const auto matches = [width](const auto& frame, auto pixel,
                           const auto& reference, auto reference_pixel, std::size_t channels) {
    const auto color = frame.color.pixels.begin() +
        (pixel / width) * frame.color.row_pitch_bytes + (pixel % width) * 4;
    const auto reference_color = reference.color.pixels.begin() +
        (reference_pixel / width) * reference.color.row_pitch_bytes +
        (reference_pixel % width) * 4;
    return std::equal(color, color + channels, reference_color) &&
           frame.depth.pixels[pixel] == reference.depth.pixels[reference_pixel] &&
           frame.prim_id.pixels[pixel] == reference.prim_id.pixels[reference_pixel] &&
           frame.instance_id.pixels[pixel] == reference.instance_id.pixels[reference_pixel];
  };
  if (baseline.depth.pixels[rejected] >= baseline.depth.pixels[empty] ||
      baseline.depth.pixels[visible] >= baseline.depth.pixels[empty] ||
      !matches(masked, rejected, baseline, empty, 4) ||
      !matches(masked, visible, baseline, visible, 3) ||
      masked.color.pixels[alpha] != 255) {
    throw std::runtime_error("Forward opacity mask disagrees across color, depth or IDs");
  }
  if (!matches(higher_cutoff, visible, baseline, empty, 4))
    throw std::runtime_error("Forward opacity mask did not use linear RGB luminance");

  set_texture(false, 0.5F);
  const auto restored = render(settings);
  if (restored.color.pixels != baseline.color.pixels || restored.depth.pixels != baseline.depth.pixels ||
      restored.prim_id.pixels != baseline.prim_id.pixels ||
      restored.instance_id.pixels != baseline.instance_id.pixels) {
    throw std::runtime_error("disconnecting the opacity image did not restore coverage");
  }
}

} // namespace merlin::tests
