#include <merlin/metal/backend.hpp>

#include <cassert>
#include <memory>
#include <stdexcept>

int main() {
  merlin::metal::StableResourceTable table(1);
  const auto slot = table.Acquire(1, 0);
  assert(slot.index == 0);
  assert(slot.generation != 0);
  assert(table.telemetry().in_use == 1);
  merlin::metal::BackendFactory factory;
  assert(factory.kind() == merlin::render::BackendKind::Metal);
  // Exercise the embedded library from an installed target without shader paths.
  if (factory.availability().available) {
    auto backend = factory.Create({});
    assert(backend);
    merlin::render::RenderRequest request;
    request.snapshot = std::make_shared<merlin::extraction::FrameSnapshot>();
    request.width = request.height = 16;
    request.products = {{merlin::Aov::Color, false}};
    request.gpu_driven_gaussian.mode = merlin::render::GpuDrivenGaussianMode::Require;
    const auto result = backend->Resolve(backend->Submit(request));
    if (result.telemetry.gaussian_draw_count != 1 ||
        result.telemetry.gaussian_gpu_fallback_count != 0)
      throw std::runtime_error("installed Metal Gaussian execution unavailable");
  }
  return 0;
}
