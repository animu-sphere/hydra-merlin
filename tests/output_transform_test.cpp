#include <merlin/render/backend.hpp>
#include <merlin/render/output_transform.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

int main() {
  using namespace merlin::render;
  const auto check = [](float actual, double expected) {
    assert(std::abs(actual - expected) <= 1e-6 * std::max(1.0, expected));
  };
  for (const auto ev : {-32.0F, -2.0F, 0.0F, 1.0F, 32.0F}) {
    const double exposure = std::exp2(static_cast<double>(ev));
    for (const auto mode : {ToneMapping::None, ToneMapping::Reinhard}) {
      OutputTransformSettings settings{ev, mode};
      const auto output = ApplyForwardOutputTransform({0.0F, 0.25F, 8.0F}, settings);
      const auto expected = [&](double value) {
        value *= exposure;
        return mode == ToneMapping::None ? value : value / (1 + value);
      };
      check(output.x, 0);
      check(output.y, expected(0.25));
      check(output.z, expected(8));
    }
  }
  // HDR * positive exposure would overflow float. Reinhard must remain finite.
  const auto hdr = ApplyForwardOutputTransform(
      {std::numeric_limits<float>::max(), 0, 0}, {32, ToneMapping::Reinhard});
  assert(hdr.x == 1.0F);
  for (const auto ev : {33.0F, -33.0F, std::numeric_limits<float>::infinity(),
           std::numeric_limits<float>::quiet_NaN()}) {
    bool rejected{};
    try {
      (void)PackForwardOutputTransform({ev, ToneMapping::None});
    } catch (const RendererError& error) {
      rejected = error.code() == RendererErrorCode::InvalidRequest &&
                 error.detail().starts_with("renderer-settings.invalid-exposure");
    }
    assert(rejected);
  }
  for (const auto mode : {ToneMapping::Aces, static_cast<ToneMapping>(99)}) {
    bool rejected{};
    try {
      (void)PackForwardOutputTransform({0, mode});
    } catch (const RendererError& error) {
      rejected = error.code() == RendererErrorCode::InvalidRequest;
    }
    assert(rejected);
  }
  assert(RendererSettings{}.tone_mapping == ToneMapping::Reinhard);
}
