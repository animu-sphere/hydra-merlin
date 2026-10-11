#pragma once

#include <array>
#include <cstdint>
#include <filesystem>

#include <merlin/core/types.hpp>

namespace merlin::render {

struct DiffuseEnvironment {
  std::uint32_t width{};
  std::uint32_t height{};
  // Real SH through l=2, convolved with the Lambert cosine kernel / pi.
  std::array<Vec4, 9> coefficients{};
};

[[nodiscard]] DiffuseEnvironment LoadDiffuseEnvironment(
    const std::filesystem::path& path);
[[nodiscard]] Vec3 EvaluateDiffuseEnvironment(
    const DiffuseEnvironment& environment, const Vec3& normal);

// The packaged StinsonBeach environment projected to SH. Embedded so Metal
// SDK consumers get the same default fill without runtime asset discovery.
// The CPU environment test verifies every coefficient against environment.hdr.
[[nodiscard]] DiffuseEnvironment PackagedDiffuseEnvironment();

} // namespace merlin::render
