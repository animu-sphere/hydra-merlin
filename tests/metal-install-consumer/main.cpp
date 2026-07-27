#include <merlin/metal/backend.hpp>

#include <cassert>

int main() {
  merlin::metal::StableResourceTable table(1);
  const auto slot = table.Acquire(1, 0);
  assert(slot.index == 0);
  assert(slot.generation != 0);
  assert(table.telemetry().in_use == 1);
  merlin::metal::BackendFactory factory;
  assert(factory.kind() == merlin::render::BackendKind::Metal);
  return 0;
}
