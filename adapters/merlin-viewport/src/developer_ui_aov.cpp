#include "developer_ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>

namespace merlin::viewport {
namespace {

constexpr std::uint32_t kMaxPreviewExtent = 64;

std::pair<std::uint32_t, std::uint32_t> PreviewExtent(std::uint32_t width,
                                                      std::uint32_t height) {
  if (width == 0 || height == 0) {
    throw std::invalid_argument("AOV preview extent must be non-zero");
  }
  const auto scale =
      std::min(1.0, static_cast<double>(kMaxPreviewExtent) /
                        static_cast<double>(std::max(width, height)));
  return {std::max(1U, static_cast<std::uint32_t>(std::round(width * scale))),
          std::max(1U, static_cast<std::uint32_t>(std::round(height * scale)))};
}

template <typename Function>
void PopulatePixels(DeveloperUiAovPreview& result, Function&& function) {
  result.pixels.reserve(static_cast<std::size_t>(result.preview_width) *
                        result.preview_height);
  for (std::uint32_t y = 0; y < result.preview_height; ++y) {
    for (std::uint32_t x = 0; x < result.preview_width; ++x) {
      const auto source_x =
          std::min(result.source_width - 1U,
                   static_cast<std::uint32_t>(
                       (static_cast<std::uint64_t>(x) * result.source_width) /
                       result.preview_width));
      const auto source_y =
          std::min(result.source_height - 1U,
                   static_cast<std::uint32_t>(
                       (static_cast<std::uint64_t>(y) * result.source_height) /
                       result.preview_height));
      auto pixel = function(source_x, source_y);
      pixel.source_x = source_x;
      pixel.source_y = source_y;
      result.pixels.push_back(pixel);
    }
  }
}

DeveloperUiAovPreview MakePreview(Aov aov, std::uint32_t width,
                                  std::uint32_t height,
                                  std::uint64_t frame_index) {
  const auto [preview_width, preview_height] = PreviewExtent(width, height);
  DeveloperUiAovPreview result;
  result.available = true;
  result.aov = aov;
  result.source_width = width;
  result.source_height = height;
  result.preview_width = preview_width;
  result.preview_height = preview_height;
  result.frame_index = frame_index;
  return result;
}

void RequireSize(std::size_t actual, std::size_t expected) {
  if (actual != expected) {
    throw std::invalid_argument(
        "AOV preview payload size does not match extent");
  }
}

std::array<std::uint8_t, 4> IdColor(std::uint32_t id) {
  if (id == std::numeric_limits<std::uint32_t>::max()) {
    return {20, 22, 25, 255};
  }
  auto hash = id + 0x9e3779b9U;
  hash = (hash ^ (hash >> 16U)) * 0x21f0aaadU;
  hash = (hash ^ (hash >> 15U)) * 0x735a2d97U;
  hash ^= hash >> 15U;
  return {static_cast<std::uint8_t>(64U + (hash & 0xBFU)),
          static_cast<std::uint8_t>(64U + ((hash >> 8U) & 0xBFU)),
          static_cast<std::uint8_t>(64U + ((hash >> 16U) & 0xBFU)), 255};
}

}  // namespace

void AddDeveloperUiAovInspectionProduct(
    const DeveloperUiRendererSettings& settings,
    std::vector<render::RenderProductRequest>& products) {
  if (!settings.aov_inspection_enabled) {
    return;
  }
  const auto found =
      std::find_if(products.begin(), products.end(), [&](const auto& product) {
        return product.aov == settings.inspection_aov;
      });
  if (found != products.end()) {
    found->cpu_readback = true;
  } else {
    products.push_back({settings.inspection_aov, true});
  }
}

DeveloperUiAovPreview BuildDeveloperUiColorPreview(
    std::uint32_t width, std::uint32_t height, std::uint64_t frame_index,
    std::span<const std::uint8_t> rgba) {
  RequireSize(rgba.size(), static_cast<std::size_t>(width) * height * 4U);
  auto result = MakePreview(Aov::Color, width, height, frame_index);
  result.minimum.fill(255.0);
  for (std::size_t i = 0; i < rgba.size(); i += 4U) {
    for (std::size_t component = 0; component < 4U; ++component) {
      result.minimum[component] = std::min(
          result.minimum[component], static_cast<double>(rgba[i + component]));
      result.maximum[component] = std::max(
          result.maximum[component], static_cast<double>(rgba[i + component]));
    }
  }
  PopulatePixels(result, [&](std::uint32_t x, std::uint32_t y) {
    DeveloperUiAovPreviewPixel pixel;
    const auto offset = (static_cast<std::size_t>(y) * width + x) * 4U;
    std::copy_n(rgba.begin() + static_cast<std::ptrdiff_t>(offset), 4,
                pixel.color.begin());
    pixel.display_rgba = pixel.color;
    return pixel;
  });
  return result;
}

DeveloperUiAovPreview BuildDeveloperUiDepthPreview(
    std::uint32_t width, std::uint32_t height, std::uint64_t frame_index,
    std::span<const float> depth) {
  RequireSize(depth.size(), static_cast<std::size_t>(width) * height);
  auto result = MakePreview(Aov::Depth, width, height, frame_index);
  auto minimum = std::numeric_limits<float>::infinity();
  auto maximum = -std::numeric_limits<float>::infinity();
  for (const auto value : depth) {
    if (std::isfinite(value)) {
      minimum = std::min(minimum, value);
      maximum = std::max(maximum, value);
    } else {
      ++result.invalid_value_count;
    }
  }
  if (!std::isfinite(minimum)) {
    minimum = maximum = 0.0F;
  }
  result.minimum[0] = minimum;
  result.maximum[0] = maximum;
  const auto range = maximum - minimum;
  PopulatePixels(result, [&](std::uint32_t x, std::uint32_t y) {
    DeveloperUiAovPreviewPixel pixel;
    pixel.depth = depth[static_cast<std::size_t>(y) * width + x];
    if (!std::isfinite(pixel.depth)) {
      pixel.display_rgba = {255, 0, 255, 255};
    } else {
      const auto normalized =
          range > 0.0F ? (pixel.depth - minimum) / range : 0.0F;
      const auto shade = static_cast<std::uint8_t>(
          std::clamp(normalized, 0.0F, 1.0F) * 255.0F);
      pixel.display_rgba = {shade, shade, shade, 255};
    }
    return pixel;
  });
  return result;
}

DeveloperUiAovPreview BuildDeveloperUiIdPreview(
    Aov aov, std::uint32_t width, std::uint32_t height,
    std::uint64_t frame_index, std::span<const std::uint32_t> ids) {
  if (aov != Aov::PrimId && aov != Aov::InstanceId) {
    throw std::invalid_argument("ID preview requires an ID AOV");
  }
  RequireSize(ids.size(), static_cast<std::size_t>(width) * height);
  auto result = MakePreview(aov, width, height, frame_index);
  auto minimum = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t maximum{};
  bool found{};
  for (const auto value : ids) {
    if (value == std::numeric_limits<std::uint32_t>::max()) {
      ++result.invalid_value_count;
    } else {
      found = true;
      minimum = std::min(minimum, value);
      maximum = std::max(maximum, value);
    }
  }
  result.minimum[0] = found ? minimum : 0U;
  result.maximum[0] = found ? maximum : 0U;
  PopulatePixels(result, [&](std::uint32_t x, std::uint32_t y) {
    DeveloperUiAovPreviewPixel pixel;
    pixel.id = ids[static_cast<std::size_t>(y) * width + x];
    pixel.display_rgba = IdColor(pixel.id);
    return pixel;
  });
  return result;
}

}  // namespace merlin::viewport
