#pragma once

#include <merlin/render/diffuse_environment.hpp>

namespace merlin::vulkan::detail {

using render::DiffuseEnvironment;
using render::EvaluateDiffuseEnvironment;

// Preserve the Vulkan error boundary for native callers.
[[nodiscard]] DiffuseEnvironment LoadDiffuseEnvironment(
    const std::filesystem::path& path);

} // namespace merlin::vulkan::detail
