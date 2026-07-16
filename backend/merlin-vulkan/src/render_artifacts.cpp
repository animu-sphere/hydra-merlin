#include <merlin/vulkan/render_artifacts.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace merlin::vulkan {
namespace {

using Bytes = std::vector<std::uint8_t>;

template <class Integer>
void AppendLittleEndian(Bytes& bytes, Integer value) {
  static_assert(std::is_integral_v<Integer>);
  using Unsigned = std::make_unsigned_t<Integer>;
  const auto bits = static_cast<Unsigned>(value);
  for (std::size_t i = 0; i < sizeof(Integer); ++i) {
    bytes.push_back(static_cast<std::uint8_t>(bits >> (i * 8U)));
  }
}

void AppendFloat(Bytes& bytes, float value) {
  AppendLittleEndian(bytes, std::bit_cast<std::uint32_t>(value));
}

void AppendString(Bytes& bytes, std::string_view text) {
  bytes.insert(bytes.end(), text.begin(), text.end());
  bytes.push_back(0);
}

void WriteFile(const std::filesystem::path& path, const Bytes& bytes) {
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("could not create image artifact: " +
                             path.string());
  }
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!stream) {
    throw std::runtime_error("could not write image artifact: " +
                             path.string());
  }
}

std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xffffffffU;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^
            (0xedb88320U & (0U - static_cast<std::uint32_t>(crc & 1U)));
    }
  }
  return ~crc;
}

std::uint32_t Adler32(const Bytes& bytes) {
  constexpr std::uint32_t modulus = 65521U;
  std::uint32_t a = 1;
  std::uint32_t b = 0;
  for (const auto value : bytes) {
    a = (a + value) % modulus;
    b = (b + a) % modulus;
  }
  return (b << 16U) | a;
}

void AppendBigEndian(Bytes& bytes, std::uint32_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
  bytes.push_back(static_cast<std::uint8_t>(value));
}

void AppendPngChunk(Bytes& png, const std::array<char, 4>& type,
                    const Bytes& payload) {
  AppendBigEndian(png, static_cast<std::uint32_t>(payload.size()));
  const auto crc_begin = png.size();
  png.insert(png.end(), type.begin(), type.end());
  png.insert(png.end(), payload.begin(), payload.end());
  AppendBigEndian(png, Crc32(png.data() + crc_begin,
                            png.size() - crc_begin));
}

void ValidateColor(const ImageRgba8& image) {
  if (!IsCanonicalRenderProduct(image.product) ||
      image.product.aov != Aov::Color ||
      image.row_pitch_bytes != TightRowPitchBytes(image.product) ||
      image.pixels.size() != static_cast<std::size_t>(image.row_pitch_bytes) *
                                 image.product.height) {
    throw std::invalid_argument("invalid color image for PNG output");
  }
}

template <class Image>
void ValidateExr(const Image& image, Aov aov) {
  if (!IsCanonicalRenderProduct(image.product) || image.product.aov != aov ||
      image.row_pitch_bytes != TightRowPitchBytes(image.product) ||
      image.pixels.size() != static_cast<std::size_t>(image.product.width) *
                                 image.product.height) {
    throw std::invalid_argument("invalid image for EXR output");
  }
}

void AppendAttribute(Bytes& header, std::string_view name,
                     std::string_view type, const Bytes& payload) {
  AppendString(header, name);
  AppendString(header, type);
  AppendLittleEndian(header, static_cast<std::uint32_t>(payload.size()));
  header.insert(header.end(), payload.begin(), payload.end());
}

template <class Value>
Bytes MakeExr(std::uint32_t width, std::uint32_t height,
              std::string_view channel, std::uint32_t pixel_type,
              const std::vector<Value>& pixels) {
  Bytes header;
  Bytes channels;
  AppendString(channels, channel);
  AppendLittleEndian(channels, pixel_type);
  channels.insert(channels.end(), {0, 0, 0, 0});
  AppendLittleEndian(channels, 1U);
  AppendLittleEndian(channels, 1U);
  channels.push_back(0);
  AppendAttribute(header, "channels", "chlist", channels);
  AppendAttribute(header, "compression", "compression", Bytes{0});
  Bytes box;
  AppendLittleEndian(box, 0);
  AppendLittleEndian(box, 0);
  AppendLittleEndian(box, static_cast<std::int32_t>(width - 1U));
  AppendLittleEndian(box, static_cast<std::int32_t>(height - 1U));
  AppendAttribute(header, "dataWindow", "box2i", box);
  AppendAttribute(header, "displayWindow", "box2i", box);
  AppendAttribute(header, "lineOrder", "lineOrder", Bytes{0});
  Bytes one;
  AppendFloat(one, 1.0F);
  AppendAttribute(header, "pixelAspectRatio", "float", one);
  Bytes center;
  AppendFloat(center, 0.0F);
  AppendFloat(center, 0.0F);
  AppendAttribute(header, "screenWindowCenter", "v2f", center);
  AppendAttribute(header, "screenWindowWidth", "float", one);
  header.push_back(0);

  Bytes exr;
  AppendLittleEndian(exr, 20000630U);
  AppendLittleEndian(exr, 2U);
  exr.insert(exr.end(), header.begin(), header.end());
  const auto table_begin = exr.size();
  exr.resize(exr.size() + static_cast<std::size_t>(height) * 8U);
  const auto row_bytes = static_cast<std::uint32_t>(width * sizeof(Value));
  for (std::uint32_t y = 0; y < height; ++y) {
    const auto offset = static_cast<std::uint64_t>(exr.size());
    for (std::size_t byte = 0; byte < 8; ++byte) {
      exr[table_begin + static_cast<std::size_t>(y) * 8U + byte] =
          static_cast<std::uint8_t>(offset >> (byte * 8U));
    }
    AppendLittleEndian(exr, static_cast<std::int32_t>(y));
    AppendLittleEndian(exr, row_bytes);
    const auto row = pixels.data() + static_cast<std::size_t>(y) * width;
    for (std::uint32_t x = 0; x < width; ++x) {
      if constexpr (std::is_floating_point_v<Value>) {
        AppendLittleEndian(exr, std::bit_cast<std::uint32_t>(row[x]));
      } else {
        AppendLittleEndian(exr, static_cast<std::uint32_t>(row[x]));
      }
    }
  }
  return exr;
}

void RequireComparisonInputs(const RenderResult& expected,
                             const RenderResult& actual) {
  for (const auto aov :
       {Aov::Color, Aov::Depth, Aov::PrimId, Aov::InstanceId}) {
    if (!HasCpuReadback(expected, aov) || !HasCpuReadback(actual, aov)) {
      throw std::invalid_argument(
          "comparison artifacts require color, depth, primId, and instanceId readback");
    }
  }
  ValidateRenderResult(expected);
  ValidateRenderResult(actual);
  if (expected.color.product.width != actual.color.product.width ||
      expected.color.product.height != actual.color.product.height) {
    throw std::invalid_argument("comparison artifact extents do not match");
  }
}

}  // namespace

void WritePng(const std::filesystem::path& path, const ImageRgba8& image) {
  ValidateColor(image);
  Bytes raw;
  raw.reserve(static_cast<std::size_t>(image.row_pitch_bytes + 1U) *
              image.product.height);
  for (std::uint32_t y = 0; y < image.product.height; ++y) {
    raw.push_back(0);
    const auto begin = image.pixels.begin() +
                       static_cast<std::size_t>(y) * image.row_pitch_bytes;
    raw.insert(raw.end(), begin, begin + image.row_pitch_bytes);
  }

  Bytes zlib{0x78, 0x01};
  std::size_t cursor{};
  while (cursor < raw.size()) {
    const auto count = static_cast<std::uint16_t>(
        std::min<std::size_t>(65535U, raw.size() - cursor));
    const bool final = cursor + count == raw.size();
    zlib.push_back(final ? 1 : 0);
    AppendLittleEndian(zlib, count);
    AppendLittleEndian(zlib, static_cast<std::uint16_t>(~count));
    zlib.insert(zlib.end(), raw.begin() + cursor,
                raw.begin() + cursor + count);
    cursor += count;
  }
  AppendBigEndian(zlib, Adler32(raw));

  Bytes png{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  Bytes ihdr;
  AppendBigEndian(ihdr, image.product.width);
  AppendBigEndian(ihdr, image.product.height);
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
  AppendPngChunk(png, {'I', 'H', 'D', 'R'}, ihdr);
  AppendPngChunk(png, {'I', 'D', 'A', 'T'}, zlib);
  AppendPngChunk(png, {'I', 'E', 'N', 'D'}, {});
  WriteFile(path, png);
}

void WriteExr(const std::filesystem::path& path, const ImageDepth32& image) {
  ValidateExr(image, Aov::Depth);
  WriteFile(path, MakeExr(image.product.width, image.product.height, "Z", 2,
                          image.pixels));
}

void WriteExr(const std::filesystem::path& path, const ImageUint32& image) {
  if (image.product.aov != Aov::PrimId &&
      image.product.aov != Aov::InstanceId) {
    throw std::invalid_argument("integer EXR requires an ID AOV");
  }
  ValidateExr(image, image.product.aov);
  WriteFile(path, MakeExr(image.product.width, image.product.height, "ID", 0,
                          image.pixels));
}

ComparisonArtifactSet SaveComparisonArtifacts(
    const RenderResult& expected, const RenderResult& actual,
    const std::filesystem::path& directory) {
  RequireComparisonInputs(expected, actual);
  ComparisonArtifactSet result;
  result.matches = true;

  ImageRgba8 color_diff = actual.color;
  for (std::size_t i = 0; i < color_diff.pixels.size(); ++i) {
    const auto lhs = expected.color.pixels[i];
    const auto rhs = actual.color.pixels[i];
    color_diff.pixels[i] = i % 4U == 3U
                               ? 255U
                               : static_cast<std::uint8_t>(
                                     lhs > rhs ? lhs - rhs : rhs - lhs);
    result.matches = result.matches && lhs == rhs;
  }
  ImageDepth32 depth_diff = actual.depth;
  for (std::size_t i = 0; i < depth_diff.pixels.size(); ++i) {
    const auto lhs = expected.depth.pixels[i];
    const auto rhs = actual.depth.pixels[i];
    depth_diff.pixels[i] = lhs > rhs ? lhs - rhs : rhs - lhs;
    result.matches = result.matches && lhs == rhs;
  }
  ImageUint32 prim_id_diff = actual.prim_id;
  for (std::size_t i = 0; i < prim_id_diff.pixels.size(); ++i) {
    const bool different =
        expected.prim_id.pixels[i] != actual.prim_id.pixels[i];
    prim_id_diff.pixels[i] = different ? 1U : 0U;
    result.matches = result.matches && !different;
  }
  ImageUint32 instance_id_diff = actual.instance_id;
  for (std::size_t i = 0; i < instance_id_diff.pixels.size(); ++i) {
    const bool different =
        expected.instance_id.pixels[i] != actual.instance_id.pixels[i];
    instance_id_diff.pixels[i] = different ? 1U : 0U;
    result.matches = result.matches && !different;
  }

  const auto add = [&](std::string_view name) {
    result.files.push_back(directory / name);
    return result.files.back();
  };
  WritePng(add("color-expected.png"), expected.color);
  WritePng(add("color-actual.png"), actual.color);
  WritePng(add("color-diff.png"), color_diff);
  WriteExr(add("depth-expected.exr"), expected.depth);
  WriteExr(add("depth-actual.exr"), actual.depth);
  WriteExr(add("depth-diff.exr"), depth_diff);
  WriteExr(add("primId-expected.exr"), expected.prim_id);
  WriteExr(add("primId-actual.exr"), actual.prim_id);
  WriteExr(add("primId-diff.exr"), prim_id_diff);
  WriteExr(add("instanceId-expected.exr"), expected.instance_id);
  WriteExr(add("instanceId-actual.exr"), actual.instance_id);
  WriteExr(add("instanceId-diff.exr"), instance_id_diff);
  return result;
}

}  // namespace merlin::vulkan
