#pragma once

#include <cstddef>
#include <cstdint>
#include <merlin/core/types.hpp>
#include "../../../core/merlin-render-backend/shaders/gaussian-raster-abi.slang"

namespace merlin::metal {

// Scalar layout consumed by gaussian-metal.slang byte-address loads.
// IDs follow the existing Vulkan AOV ABI.
struct GaussianInstance {
  Vec2 center;
  Vec3 conic;
  Vec3 radiance;
  float opacity;
  float radius;
  float depth;
  std::uint32_t resource;
  std::uint32_t particle;
};
static_assert(sizeof(GaussianInstance) == MERLIN_GAUSSIAN_STRIDE);
static_assert(offsetof(GaussianInstance, center) == MERLIN_GAUSSIAN_CENTER);
static_assert(offsetof(GaussianInstance, conic) == MERLIN_GAUSSIAN_CONIC);
static_assert(offsetof(GaussianInstance, radiance) == MERLIN_GAUSSIAN_RADIANCE);
static_assert(offsetof(GaussianInstance, opacity) == MERLIN_GAUSSIAN_OPACITY);
static_assert(offsetof(GaussianInstance, radius) == MERLIN_GAUSSIAN_RADIUS);
static_assert(offsetof(GaussianInstance, depth) == MERLIN_GAUSSIAN_DEPTH);
static_assert(offsetof(GaussianInstance, resource) == MERLIN_GAUSSIAN_RESOURCE);
static_assert(offsetof(GaussianInstance, particle) == MERLIN_GAUSSIAN_PARTICLE);

} // namespace merlin::metal
