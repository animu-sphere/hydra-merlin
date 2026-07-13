#include <merlin/vulkan/render_artifacts.hpp>
#include <merlin/vulkan/renderer.hpp>

int main() {
  merlin::vulkan::RenderRequest request;
  return request.products.size() == 2 &&
                 request.products.front().aov == merlin::Aov::Color
             ? 0
             : 1;
}
