#pragma once

#include <cstdint>
#include <string_view>

namespace merlin {

enum class Aov {
  Color,
  Depth,
  Normal,
  Albedo,
  Roughness,
  Metallic,
  Emission,
  PrimId,
  InstanceId,
  MotionVector
};

enum class PixelFormat {
  Rgba8Unorm,
  Depth32Float,
  R32Uint
};

enum class ImageOrigin { TopLeft, BottomLeft };
enum class ColorSpace { Linear, Srgb, NotApplicable };

[[nodiscard]] constexpr std::uint32_t BytesPerPixel(
    PixelFormat format) noexcept {
  switch (format) {
    case PixelFormat::Rgba8Unorm:
    case PixelFormat::Depth32Float:
    case PixelFormat::R32Uint:
      return 4;
  }
  return 0;
}

[[nodiscard]] constexpr std::string_view AovName(Aov aov) noexcept {
  switch (aov) {
    case Aov::Color: return "color";
    case Aov::Depth: return "depth";
    case Aov::Normal: return "normal";
    case Aov::Albedo: return "albedo";
    case Aov::Roughness: return "roughness";
    case Aov::Metallic: return "metallic";
    case Aov::Emission: return "emission";
    case Aov::PrimId: return "primId";
    case Aov::InstanceId: return "instanceId";
    case Aov::MotionVector: return "motionVector";
  }
  return "unknown";
}

struct RenderProduct {
  std::uint32_t width{512};
  std::uint32_t height{512};
  Aov aov{Aov::Color};
  PixelFormat format{PixelFormat::Rgba8Unorm};
  ImageOrigin origin{ImageOrigin::TopLeft};
  ColorSpace color_space{ColorSpace::Linear};
  // Zero means tightly packed. Non-zero values require each row pitch to be
  // aligned to this byte count.
  std::uint32_t row_pitch_alignment{};

  friend constexpr bool operator==(const RenderProduct&,
                                   const RenderProduct&) = default;
};

[[nodiscard]] constexpr RenderProduct MakeRenderProduct(
    std::uint32_t width, std::uint32_t height, Aov aov) noexcept {
  RenderProduct result;
  result.width = width;
  result.height = height;
  result.aov = aov;
  if (aov == Aov::Depth) {
    result.format = PixelFormat::Depth32Float;
    result.color_space = ColorSpace::NotApplicable;
  } else if (aov == Aov::PrimId || aov == Aov::InstanceId) {
    result.format = PixelFormat::R32Uint;
    result.color_space = ColorSpace::NotApplicable;
  }
  return result;
}

[[nodiscard]] constexpr std::uint64_t TightRowPitchBytes(
    const RenderProduct& product) noexcept {
  return static_cast<std::uint64_t>(product.width) *
         BytesPerPixel(product.format);
}

[[nodiscard]] constexpr bool IsCanonicalRenderProduct(
    const RenderProduct& product) noexcept {
  if (product.width == 0 || product.height == 0 ||
      product.origin != ImageOrigin::TopLeft ||
      product.row_pitch_alignment != 0) {
    return false;
  }
  const auto expected =
      MakeRenderProduct(product.width, product.height, product.aov);
  return product == expected;
}

}  // namespace merlin
