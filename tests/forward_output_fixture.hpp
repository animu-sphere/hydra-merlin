#pragma once

#include <merlin/render/output_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace merlin::tests {

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
    const auto& counters = [&]() -> const auto& {
      if constexpr (requires { frame.telemetry; })
        return frame.telemetry;
      else
        return frame.counters;
    }();
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

} // namespace merlin::tests
