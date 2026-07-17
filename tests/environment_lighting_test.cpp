#include "environment_lighting.hpp"

#include <merlin/vulkan/renderer.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

bool Finite(const merlin::Vec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

float Difference(const merlin::Vec3& lhs, const merlin::Vec3& rhs) {
  return std::abs(lhs.x - rhs.x) + std::abs(lhs.y - rhs.y) +
         std::abs(lhs.z - rhs.z);
}

// A 64x32 RLE Radiance image whose every texel is exactly 1.0: the RGBE
// bytes (128, 128, 128, 129) decode to 128 * 2^(129 - 136) = 1.
void WriteUniformEnvironment(const std::filesystem::path& path) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  assert(stream);
  stream << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 32 +X 64\n";
  for (int row = 0; row < 32; ++row) {
    const std::array<unsigned char, 4> marker{2, 2, 0, 64};
    stream.write(reinterpret_cast<const char*>(marker.data()),
                 static_cast<std::streamsize>(marker.size()));
    for (int channel = 0; channel < 4; ++channel) {
      const std::array<unsigned char, 2> run{
          128 + 64, static_cast<unsigned char>(channel == 3 ? 129 : 128)};
      stream.write(reinterpret_cast<const char*>(run.data()),
                   static_cast<std::streamsize>(run.size()));
    }
  }
  assert(stream);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: environment_lighting_test ENVIRONMENT_HDR\n";
    return 1;
  }
  const auto environment =
      merlin::vulkan::detail::LoadDiffuseEnvironment(argv[1]);
  assert(environment.width == 1024);
  assert(environment.height == 512);
  for (const auto& coefficient : environment.coefficients) {
    assert(std::isfinite(coefficient.x));
    assert(std::isfinite(coefficient.y));
    assert(std::isfinite(coefficient.z));
  }
  assert(environment.coefficients[0].x > 0.0F);
  assert(environment.coefficients[0].y > 0.0F);
  assert(environment.coefficients[0].z > 0.0F);

  const auto up = merlin::vulkan::detail::EvaluateDiffuseEnvironment(
      environment, {0.0F, 1.0F, 0.0F});
  const auto horizon = merlin::vulkan::detail::EvaluateDiffuseEnvironment(
      environment, {1.0F, 0.0F, 0.0F});
  const auto down = merlin::vulkan::detail::EvaluateDiffuseEnvironment(
      environment, {0.0F, -1.0F, 0.0F});
  assert(Finite(up));
  assert(Finite(horizon));
  assert(Finite(down));
  assert(std::max({Difference(up, horizon), Difference(up, down),
                   Difference(horizon, down)}) > 0.01F);

  // A uniform radiance-1 environment must evaluate to irradiance/pi of 1 in
  // every direction; this pins the SH basis and Lambert convolution factors.
  const auto uniform_path = std::filesystem::temp_directory_path() /
                            "merlin-uniform-environment.hdr";
  WriteUniformEnvironment(uniform_path);
  const auto uniform =
      merlin::vulkan::detail::LoadDiffuseEnvironment(uniform_path);
  for (const auto& normal :
       {merlin::Vec3{0.0F, 1.0F, 0.0F}, merlin::Vec3{1.0F, 0.0F, 0.0F},
        merlin::Vec3{0.0F, 0.0F, -1.0F},
        merlin::Vec3{0.577F, -0.577F, 0.577F}}) {
    const auto value =
        merlin::vulkan::detail::EvaluateDiffuseEnvironment(uniform, normal);
    assert(std::abs(value.x - 1.0F) < 0.01F);
    assert(std::abs(value.y - 1.0F) < 0.01F);
    assert(std::abs(value.z - 1.0F) < 0.01F);
  }
  std::filesystem::remove(uniform_path);

  bool missing_rejected{};
  try {
    (void)merlin::vulkan::detail::LoadDiffuseEnvironment(
        std::filesystem::path(argv[1]).parent_path() / "missing.hdr");
  } catch (const merlin::vulkan::RendererError& error) {
    missing_rejected =
        error.code() == merlin::vulkan::RendererErrorCode::InvalidRequest &&
        error.operation() == "load environment HDR";
  }
  assert(missing_rejected);
}
