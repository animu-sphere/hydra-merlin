#pragma once

#include <span>

#include <merlin/core/types.hpp>

namespace merlin::render {

inline constexpr std::uint32_t kSpecularEnvironmentWidth = 512;
inline constexpr std::uint32_t kSpecularEnvironmentHeight = 544;

// Builds the Forward atlas from a -Y +X lat-long Radiance image. Store the
// returned linear RGBA8 texture in the snapshot and pass its handle in the dome.
[[nodiscard]] TextureDescriptor BuildSpecularEnvironment(std::uint32_t width,
    std::uint32_t height, std::span<const std::uint8_t> rgbe_pixels);

} // namespace merlin::render
