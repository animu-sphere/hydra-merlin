#include "environment_lighting.hpp"

#include <merlin/vulkan/renderer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace merlin::vulkan::detail {

namespace {

constexpr double kPi = 3.14159265358979323846;

[[noreturn]] void Fail(const std::filesystem::path& path,
                       const std::string& detail) {
  throw RendererError(RendererErrorCode::InvalidRequest,
                      "load environment HDR", path.string() + ": " + detail);
}

std::uint8_t ReadByte(std::istream& stream,
                      const std::filesystem::path& path) {
  char value{};
  if (!stream.get(value)) {
    Fail(path, "unexpected end of file");
  }
  return static_cast<std::uint8_t>(static_cast<unsigned char>(value));
}

void ReadBytes(std::istream& stream, std::uint8_t* destination,
               std::size_t count, const std::filesystem::path& path) {
  stream.read(reinterpret_cast<char*>(destination),
              static_cast<std::streamsize>(count));
  if (stream.gcount() != static_cast<std::streamsize>(count)) {
    Fail(path, "unexpected end of scanline");
  }
}

std::array<double, 9> ShBasis(double x, double y, double z) {
  return {
      0.28209479177387814,
      0.4886025119029199 * y,
      0.4886025119029199 * z,
      0.4886025119029199 * x,
      1.0925484305920792 * x * y,
      1.0925484305920792 * y * z,
      0.31539156525252005 * (3.0 * z * z - 1.0),
      1.0925484305920792 * x * z,
      0.5462742152960396 * (x * x - y * y),
  };
}

Vec3 DecodeRgbe(const std::uint8_t* rgbe) {
  if (rgbe[3] == 0) {
    return {};
  }
  const auto scale = std::ldexp(1.0F, static_cast<int>(rgbe[3]) - 136);
  return {static_cast<float>(rgbe[0]) * scale,
          static_cast<float>(rgbe[1]) * scale,
          static_cast<float>(rgbe[2]) * scale};
}

}  // namespace

DiffuseEnvironment LoadDiffuseEnvironment(
    const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    Fail(path, "file could not be opened");
  }

  std::string line;
  if (!std::getline(stream, line) || line != "#?RADIANCE") {
    Fail(path, "missing Radiance signature");
  }
  bool rgbe_format{};
  while (std::getline(stream, line) && !line.empty()) {
    rgbe_format = rgbe_format || line == "FORMAT=32-bit_rle_rgbe";
  }
  if (!rgbe_format) {
    Fail(path, "FORMAT=32-bit_rle_rgbe is required");
  }
  if (!std::getline(stream, line)) {
    Fail(path, "missing resolution");
  }
  std::istringstream resolution(line);
  std::string y_orientation;
  std::string x_orientation;
  DiffuseEnvironment result;
  if (!(resolution >> y_orientation >> result.height >> x_orientation >>
        result.width) ||
      y_orientation != "-Y" || x_orientation != "+X" ||
      result.width < 8 || result.width > 32767 || result.height == 0) {
    Fail(path, "only -Y height +X width Radiance images are supported");
  }

  std::array<std::array<double, 3>, 9> projection{};
  std::vector<std::uint8_t> scanline(
      static_cast<std::size_t>(result.width) * 4U);
  const auto delta_theta = kPi / static_cast<double>(result.height);
  const auto delta_phi = 2.0 * kPi / static_cast<double>(result.width);
  for (std::uint32_t row = 0; row < result.height; ++row) {
    const std::array<std::uint8_t, 4> marker{
        ReadByte(stream, path), ReadByte(stream, path), ReadByte(stream, path),
        ReadByte(stream, path)};
    const auto encoded_width =
        (static_cast<std::uint32_t>(marker[2]) << 8U) | marker[3];
    if (marker[0] != 2 || marker[1] != 2 || encoded_width != result.width) {
      Fail(path, "unsupported or inconsistent scanline encoding");
    }
    for (std::uint32_t channel = 0; channel < 4; ++channel) {
      std::uint32_t column{};
      while (column < result.width) {
        const auto code = ReadByte(stream, path);
        if (code > 128) {
          const auto count = static_cast<std::uint32_t>(code - 128);
          if (count == 0 || count > result.width - column) {
            Fail(path, "invalid RLE run");
          }
          const auto value = ReadByte(stream, path);
          std::fill_n(scanline.begin() + channel * result.width + column,
                      count, value);
          column += count;
        } else {
          const auto count = static_cast<std::uint32_t>(code);
          if (count == 0 || count > result.width - column) {
            Fail(path, "invalid RLE literal");
          }
          ReadBytes(stream,
                    scanline.data() + channel * result.width + column, count,
                    path);
          column += count;
        }
      }
    }

    const auto theta =
        kPi * (static_cast<double>(row) + 0.5) / result.height;
    const auto sin_theta = std::sin(theta);
    const auto y = std::cos(theta);
    const auto solid_angle = sin_theta * delta_theta * delta_phi;
    for (std::uint32_t column = 0; column < result.width; ++column) {
      const auto phi =
          2.0 * kPi * (static_cast<double>(column) + 0.5) / result.width;
      const auto x = sin_theta * std::cos(phi);
      const auto z = sin_theta * std::sin(phi);
      const std::array<std::uint8_t, 4> rgbe{
          scanline[column], scanline[result.width + column],
          scanline[2U * result.width + column],
          scanline[3U * result.width + column]};
      const auto radiance = DecodeRgbe(rgbe.data());
      const auto basis = ShBasis(x, y, z);
      for (std::size_t coefficient = 0; coefficient < basis.size();
           ++coefficient) {
        const auto weight = basis[coefficient] * solid_angle;
        projection[coefficient][0] += radiance.x * weight;
        projection[coefficient][1] += radiance.y * weight;
        projection[coefficient][2] += radiance.z * weight;
      }
    }
  }

  // The clamped-cosine convolution divided by pi has multipliers 1, 2/3,
  // and 1/4 for bands l=0, l=1, and l=2 respectively.
  constexpr std::array<double, 9> lambert{
      1.0, 2.0 / 3.0, 2.0 / 3.0, 2.0 / 3.0,
      0.25, 0.25, 0.25, 0.25, 0.25};
  for (std::size_t coefficient = 0; coefficient < projection.size();
       ++coefficient) {
    result.coefficients[coefficient] = {
        static_cast<float>(projection[coefficient][0] *
                           lambert[coefficient]),
        static_cast<float>(projection[coefficient][1] *
                           lambert[coefficient]),
        static_cast<float>(projection[coefficient][2] *
                           lambert[coefficient]),
        0.0F};
  }
  return result;
}

Vec3 EvaluateDiffuseEnvironment(const DiffuseEnvironment& environment,
                                const Vec3& normal) {
  const auto length = std::sqrt(normal.x * normal.x + normal.y * normal.y +
                                normal.z * normal.z);
  if (length <= 0.0F) {
    return {};
  }
  const auto basis = ShBasis(normal.x / length, normal.y / length,
                             normal.z / length);
  Vec3 result;
  for (std::size_t coefficient = 0; coefficient < basis.size();
       ++coefficient) {
    result.x += static_cast<float>(basis[coefficient]) *
                environment.coefficients[coefficient].x;
    result.y += static_cast<float>(basis[coefficient]) *
                environment.coefficients[coefficient].y;
    result.z += static_cast<float>(basis[coefficient]) *
                environment.coefficients[coefficient].z;
  }
  return result;
}

}  // namespace merlin::vulkan::detail
