#include "environment_lighting.hpp"

#include <merlin/render/backend.hpp>
#include <merlin/vulkan/renderer.hpp>

namespace merlin::vulkan::detail {

DiffuseEnvironment LoadDiffuseEnvironment(const std::filesystem::path& path) {
  try {
    return render::LoadDiffuseEnvironment(path);
  } catch (const render::RendererError& error) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        error.operation(), error.detail());
  }
}

} // namespace merlin::vulkan::detail
