#include <merlin/vulkan/backend.hpp>
#include <merlin/vulkan/render_artifacts.hpp>
#include <merlin/vulkan/renderer.hpp>

int main() {
  merlin::vulkan::RenderRequest request;
  merlin::vulkan::BackendFactory factory({});
  return request.products.size() == 2 &&
                 request.products.front().aov == merlin::Aov::Color &&
                 factory.kind() == merlin::render::BackendKind::Vulkan
             ? 0
             : 1;
}
