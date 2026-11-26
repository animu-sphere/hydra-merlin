#include <merlin/render/specular_environment.hpp>

#include <merlin/render/backend.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace merlin::render {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;
// Keep the level extents and roughnesses in sync with specular-environment.slang.
constexpr std::array kRoughness{0.045F, 0.15F, 0.3F, 0.5F, 0.75F, 1.0F};
constexpr int kSourceWidth = kSpecularEnvironmentWidth;
constexpr int kSourceHeight = kSourceWidth / 2;
constexpr int kMinLevelWidth = 64;
constexpr int kDfgSize = 64;
constexpr int kDfgX = 64;
constexpr int kDfgY = 448;

struct Panorama {
  int width;
  int height;
  std::vector<Vec3> pixels;

  const Vec3& At(int x, int y) const {
    return pixels[static_cast<std::size_t>(y) * width + x];
  }
};

Vec3 Lerp(Vec3 a, Vec3 b, float weight) {
  return {std::lerp(a.x, b.x, weight), std::lerp(a.y, b.y, weight),
      std::lerp(a.z, b.z, weight)};
}

Vec3 Sample(const Panorama& panorama, float u, float v) {
  const auto x = u * panorama.width - 0.5F;
  const auto y = v * panorama.height - 0.5F;
  const auto left = static_cast<int>(std::floor(x));
  const auto top = static_cast<int>(std::floor(y));
  const auto x0 = (left + panorama.width) % panorama.width;
  const auto x1 = (x0 + 1) % panorama.width;
  const auto y0 = std::clamp(top, 0, panorama.height - 1);
  const auto y1 = std::clamp(top + 1, 0, panorama.height - 1);
  return Lerp(Lerp(panorama.At(x0, y0), panorama.At(x1, y0), x - left),
      Lerp(panorama.At(x0, y1), panorama.At(x1, y1), x - left), y - top);
}

Vec3 DecodeRgbe(const std::uint8_t* rgbe) {
  const auto scale = rgbe[3] == 0 ? 0.0F : std::ldexp(1.0F, rgbe[3] - 136);
  return {rgbe[0] * scale, rgbe[1] * scale, rgbe[2] * scale};
}

std::array<std::uint8_t, 4> EncodeRgbe(Vec3 value) {
  const auto peak = std::max({value.x, value.y, value.z});
  if (peak == 0.0F) {
    return {};
  }
  int exponent{};
  std::frexp(peak, &exponent);
  exponent = std::clamp(exponent + 128, 1, 255);
  const auto channel = [exponent](float radiance) {
    return static_cast<std::uint8_t>(std::min(255.0,
        std::round(std::ldexp(static_cast<double>(radiance), 136 - exponent))));
  };
  return {channel(value.x), channel(value.y), channel(value.z),
      static_cast<std::uint8_t>(exponent)};
}

Panorama MakeSource(int width, int height, std::span<const std::uint8_t> rgbe) {
  Panorama result{kSourceWidth, kSourceHeight, {}};
  result.pixels.reserve(kSourceWidth * kSourceHeight);
  std::vector<double> latitude_weights(height);
  for (int y = 0; y < height; ++y) {
    latitude_weights[y] = std::sin(std::numbers::pi * (y + 0.5) / height);
  }
  const auto step_x = static_cast<double>(width) / kSourceWidth;
  const auto step_y = static_cast<double>(height) / kSourceHeight;
  const auto half_x = std::max(1.0, step_x) * 0.5;
  const auto half_y = std::max(1.0, step_y) * 0.5;
  for (int y = 0; y < kSourceHeight; ++y) {
    const auto center_y = (y + 0.5) * step_y;
    const auto top = center_y - half_y;
    const auto bottom = center_y + half_y;
    for (int x = 0; x < kSourceWidth; ++x) {
      const auto center_x = (x + 0.5) * step_x;
      const auto left = center_x - half_x;
      const auto right = center_x + half_x;
      double r{}, g{}, b{}, total{};
      for (int row = static_cast<int>(std::floor(top)); row < bottom; ++row) {
        const auto source_y = std::clamp(row, 0, height - 1);
        const auto wy = (std::min(bottom, row + 1.0) - std::max(top, double(row))) *
                        latitude_weights[source_y];
        for (int column = static_cast<int>(std::floor(left)); column < right; ++column) {
          const auto source_x = (column + width) % width;
          const auto weight = wy *
              (std::min(right, column + 1.0) - std::max(left, double(column)));
          const auto texel = (static_cast<std::size_t>(source_y) * width + source_x) * 4;
          const auto value = DecodeRgbe(rgbe.data() + texel);
          r += value.x * weight;
          g += value.y * weight;
          b += value.z * weight;
          total += weight;
        }
      }
      result.pixels.push_back({static_cast<float>(r / total),
          static_cast<float>(g / total), static_cast<float>(b / total)});
    }
  }
  return result;
}

Panorama Downsample(const Panorama& source) {
  Panorama result{source.width / 2, source.height / 2, {}};
  result.pixels.reserve(static_cast<std::size_t>(result.width) * result.height);
  for (int y = 0; y < result.height; ++y) {
    const auto a = std::sin(kPi * (2 * y + 0.5F) / source.height);
    const auto b = std::sin(kPi * (2 * y + 1.5F) / source.height);
    for (int x = 0; x < result.width; ++x) {
      const auto top = Lerp(source.At(2 * x, 2 * y), source.At(2 * x + 1, 2 * y), 0.5F);
      const auto bottom = Lerp(source.At(2 * x, 2 * y + 1),
          source.At(2 * x + 1, 2 * y + 1), 0.5F);
      result.pixels.push_back(Lerp(top, bottom, b / (a + b)));
    }
  }
  return result;
}

std::vector<Vec3> GgxHalfVectors(float roughness, unsigned count) {
  // Hammersley points in a local frame with +Z along the normal.
  const auto alpha_squared = roughness * roughness * roughness * roughness;
  std::vector<Vec3> result;
  result.reserve(count);
  for (unsigned i = 0; i < count; ++i) {
    const auto phi = 2.0F * kPi * static_cast<float>(i) / count;
    float fraction{};
    float place = 0.5F;
    for (auto bits = i; bits != 0; bits >>= 1, place *= 0.5F) {
      fraction += static_cast<float>(bits & 1U) * place;
    }
    const auto cos_squared = (1.0F - fraction) /
        (1.0F + (alpha_squared - 1.0F) * fraction);
    const auto sine = std::sqrt(std::max(0.0F, 1.0F - cos_squared));
    result.push_back({sine * std::cos(phi), sine * std::sin(phi), std::sqrt(cos_squared)});
  }
  return result;
}

struct FilterSample {
  Vec3 direction;
  float log_area;
};

std::vector<FilterSample> FilterSamples(float roughness) {
  constexpr unsigned kSampleCount = 128;
  const auto alpha_squared = roughness * roughness * roughness * roughness;
  std::vector<FilterSample> result;
  for (const auto& half : GgxHalfVectors(roughness, kSampleCount)) {
    const auto z = 2.0F * half.z * half.z - 1.0F;
    if (z <= 0.0F) {
      continue;
    }
    const auto denom = (1.0F - half.z * half.z) + alpha_squared * half.z * half.z;
    const auto pdf = alpha_squared / (4.0F * kPi * denom * denom);
    result.push_back({{2.0F * half.z * half.x, 2.0F * half.z * half.y, z},
        std::log2(1.0F / (kSampleCount * pdf))});
  }
  return result;
}

Vec3 Prefilter(const std::vector<Panorama>& source, const std::vector<FilterSample>& samples,
    float sin_theta, float cos_theta, float sin_phi, float cos_phi) {
  constexpr float kTexelArea = (kPi / kSourceHeight) * (2.0F * kPi / kSourceWidth);
  double r{}, g{}, b{}, weight{};
  for (const auto& sample : samples) {
    // Rotate the local GGX lobe into the equirectangular texel's frame.
    const auto& l = sample.direction;
    const auto radial = l.z * sin_theta + l.y * cos_theta;
    const auto x = radial * cos_phi - l.x * sin_phi;
    const auto y = std::clamp(l.z * cos_theta - l.y * sin_theta, -1.0F, 1.0F);
    const auto z = radial * sin_phi + l.x * cos_phi;
    const auto u = std::atan2(z, x) / (2.0F * kPi) + 1.0F;
    const auto wrapped_u = u - std::floor(u);
    const auto v = std::acos(y) / kPi;
    const auto area = kTexelArea * std::max(1.0F / kSourceHeight,
        std::sqrt(std::max(0.0F, 1.0F - y * y)));
    const auto lod = std::clamp(0.5F * (sample.log_area - std::log2(area)),
        0.0F, static_cast<float>(source.size() - 1));
    const auto level = static_cast<std::size_t>(lod);
    const auto value = Lerp(Sample(source[level], wrapped_u, v),
        Sample(source[std::min(level + 1, source.size() - 1)], wrapped_u, v),
        lod - static_cast<float>(level));
    r += static_cast<double>(value.x) * l.z;
    g += static_cast<double>(value.y) * l.z;
    b += static_cast<double>(value.z) * l.z;
    weight += l.z;
  }
  return {static_cast<float>(r / weight), static_cast<float>(g / weight),
      static_cast<float>(b / weight)};
}

const auto& EnvironmentBrdf() {
  // Split-sum directional albedo for the same correlated Smith GGX and
  // Schlick Fresnel as Forward's direct lobe. Integrate the F0 and F90 terms.
  static const auto coefficients = [] {
    constexpr unsigned kSampleCount = 256;
    std::array<std::array<std::uint8_t, 4>, kDfgSize * kDfgSize> result{};
    for (int row = 0; row < kDfgSize; ++row) {
      const auto roughness = std::max(kRoughness.front(), float(row) / (kDfgSize - 1));
      const auto alpha_squared = roughness * roughness * roughness * roughness;
      const auto half_vectors = GgxHalfVectors(roughness, kSampleCount);
      for (int column = 0; column < kDfgSize; ++column) {
        const auto nv = std::max(0.0001F, float(column) / (kDfgSize - 1));
        const auto vx = std::sqrt(1.0F - nv * nv);
        const auto smith_v = std::sqrt(nv * nv * (1.0F - alpha_squared) + alpha_squared);
        float a{}, b{};
        for (const auto& half : half_vectors) {
          const auto vh = std::clamp(vx * half.x + nv * half.z, 0.0F, 1.0F);
          const auto nl = 2.0F * vh * half.z - nv;
          if (nl <= 0.0F) {
            continue;
          }
          const auto smith_l = std::sqrt(nl * nl * (1.0F - alpha_squared) + alpha_squared);
          const auto weight = 2.0F * nl * vh / (half.z * (nl * smith_v + nv * smith_l));
          const auto f = 1.0F - vh;
          const auto fresnel = f * f * f * f * f;
          a += weight * (1.0F - fresnel);
          b += weight * fresnel;
        }
        const auto encode = [](float sum) {
          return static_cast<unsigned>(std::round(std::clamp(sum / kSampleCount,
              0.0F, 1.0F) * 65535.0F));
        };
        const auto scale = encode(a);
        const auto bias = encode(b);
        result[row * kDfgSize + column] = {static_cast<std::uint8_t>(scale >> 8),
            static_cast<std::uint8_t>(scale), static_cast<std::uint8_t>(bias >> 8),
            static_cast<std::uint8_t>(bias)};
      }
    }
    return result;
  }();
  return coefficients;
}

} // namespace

TextureDescriptor BuildSpecularEnvironment(std::uint32_t width,
    std::uint32_t height, std::span<const std::uint8_t> rgbe_pixels) {
  constexpr auto max_height = static_cast<std::uint32_t>(std::numeric_limits<int>::max() / 2);
  if (width == 0 || height == 0 || width > 32767 || height > max_height ||
      rgbe_pixels.size() % 4 != 0 ||
      static_cast<std::uint64_t>(width) * height != rgbe_pixels.size() / 4) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        "build specular environment", "invalid dimensions or RGBE pixel count");
  }
  std::vector<Panorama> source;
  source.push_back(MakeSource(static_cast<int>(width), static_cast<int>(height), rgbe_pixels));
  while (source.back().height > 1) {
    source.push_back(Downsample(source.back()));
  }

  TextureDescriptor result;
  result.label = "Forward specular environment";
  result.width = kSpecularEnvironmentWidth;
  result.height = kSpecularEnvironmentHeight;
  result.pixels.resize(static_cast<std::size_t>(result.width) * result.height * 4);
  const auto store = [&](int x, int y, const std::array<std::uint8_t, 4>& pixel) {
    const auto offset = (static_cast<std::size_t>(y) * result.width + x) * 4;
    std::copy(pixel.begin(), pixel.end(), result.pixels.data() + offset);
  };
  for (int y = 0; y < kSourceHeight; ++y) {
    for (int x = 0; x < kSourceWidth; ++x) {
      store(x, y, EncodeRgbe(source.front().At(x, y)));
    }
  }
  int offset = kSourceHeight;
  for (std::size_t level = 1; level < kRoughness.size(); ++level) {
    const auto level_width = std::max(kMinLevelWidth, kSourceWidth >> level);
    const auto level_height = level_width / 2;
    const auto samples = FilterSamples(kRoughness[level]);
    for (int y = 0; y < level_height; ++y) {
      const auto theta = kPi * (y + 0.5F) / level_height;
      for (int x = 0; x < level_width; ++x) {
        const auto phi = 2.0F * kPi * (x + 0.5F) / level_width;
        store(x, offset + y, EncodeRgbe(Prefilter(source, samples,
            std::sin(theta), std::cos(theta), std::sin(phi), std::cos(phi))));
      }
    }
    offset += level_height;
  }
  const auto& brdf = EnvironmentBrdf();
  for (int y = 0; y < kDfgSize; ++y) {
    for (int x = 0; x < kDfgSize; ++x) {
      store(kDfgX + x, kDfgY + y, brdf[y * kDfgSize + x]);
    }
  }
  return result;
}

} // namespace merlin::render
