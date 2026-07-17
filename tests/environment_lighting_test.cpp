#include "environment_lighting.hpp"

#include <merlin/vulkan/renderer.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>
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
