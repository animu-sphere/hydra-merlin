#pragma once

#include <merlin/extraction/gaussian_preparation.hpp>

// Keep Vulkan's internal callers on the shared CPU reference implementation.
namespace merlin::vulkan::detail {
using extraction::EvaluateGaussianRadiance;
using extraction::GaussianPreparationCounters;
using extraction::GaussianPreparationOptions;
using extraction::GaussianPreparationResult;
using extraction::GaussianSortingPolicy;
using extraction::PreparedGaussian;
using extraction::PrepareGaussianFrame;
using extraction::SelectGaussianSortingPolicy;
} // namespace merlin::vulkan::detail
