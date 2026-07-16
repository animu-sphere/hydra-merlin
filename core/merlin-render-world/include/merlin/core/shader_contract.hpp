#pragma once

#include <cstdint>
#include <string_view>

namespace merlin {

// Renderer-facing shader capabilities. These describe semantics that a
// backend may implement; they never expose API command or resource objects.
enum class ShaderCapability : std::uint64_t {
  None = 0,
  MaterialConstants = 1ULL << 0U,
  BaseColorTexture = 1ULL << 1U,
  BindlessResources = 1ULL << 2U,
  NonUniformResourceIndexing = 1ULL << 3U,
};

constexpr ShaderCapability operator|(ShaderCapability lhs,
                                     ShaderCapability rhs) noexcept {
  return static_cast<ShaderCapability>(static_cast<std::uint64_t>(lhs) |
                                       static_cast<std::uint64_t>(rhs));
}

enum class ShaderStage : std::uint8_t { Vertex = 1, Fragment = 2 };

struct ShaderPermutation {
  std::string_view family;
  std::string_view entry_point;
  ShaderStage stage{};
  ShaderCapability capabilities{};
  std::uint32_t abi_version{1};
};

// Stable FNV-1a identity for a logical permutation. Artifact cache keys add
// compiler, target, policy, and dependency hashes in the installed manifest.
[[nodiscard]] constexpr std::uint64_t
MakeShaderPermutationKey(const ShaderPermutation& permutation) noexcept {
  constexpr std::uint64_t offset_basis = 14695981039346656037ULL;
  constexpr std::uint64_t prime = 1099511628211ULL;
  auto hash = offset_basis;
  const auto append_byte = [&hash](std::uint8_t value) constexpr {
    hash ^= value;
    hash *= prime;
  };
  const auto append_text = [&append_byte](std::string_view value) constexpr {
    for (const char character : value) {
      append_byte(static_cast<std::uint8_t>(character));
    }
    append_byte(0xffU);
  };
  append_text(permutation.family);
  append_text(permutation.entry_point);
  append_byte(static_cast<std::uint8_t>(permutation.stage));
  const auto capabilities =
      static_cast<std::uint64_t>(permutation.capabilities);
  for (unsigned int shift = 0; shift < 64; shift += 8) {
    append_byte(static_cast<std::uint8_t>(capabilities >> shift));
  }
  for (unsigned int shift = 0; shift < 32; shift += 8) {
    append_byte(static_cast<std::uint8_t>(permutation.abi_version >> shift));
  }
  return hash;
}

}  // namespace merlin
