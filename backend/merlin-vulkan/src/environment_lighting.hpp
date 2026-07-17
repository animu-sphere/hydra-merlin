#pragma once

#include <array>
#include <cstdint>
#include <filesystem>

#include <merlin/core/types.hpp>

namespace merlin::vulkan::detail {

struct DiffuseEnvironment {
  std::uint32_t width{};
  std::uint32_t height{};
  // Real spherical harmonics through l=2, already convolved with the Lambert
  // cosine kernel and divided by pi for direct multiplication with albedo.
  std::array<Vec4, 9> coefficients{};
};

[[nodiscard]] DiffuseEnvironment LoadDiffuseEnvironment(
    const std::filesystem::path& path);

[[nodiscard]] Vec3 EvaluateDiffuseEnvironment(
    const DiffuseEnvironment& environment, const Vec3& normal);

}  // namespace merlin::vulkan::detail
