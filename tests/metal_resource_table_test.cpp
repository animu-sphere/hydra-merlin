#include <merlin/metal/resource_table.hpp>

#include <cassert>
#include <stdexcept>

int main() {
  merlin::metal::StableResourceTable table(2);
  const auto first = table.Acquire(0x0000000100000001ULL, 0);
  assert(first.index == 0);
  assert(first.generation != 0);
  assert(table.Acquire(0x0000000100000001ULL, 0) == first);

  const auto second = table.Acquire(0x0000000100000002ULL, 0);
  assert(second.index == 1);
  bool exhausted{};
  try {
    (void)table.Acquire(0x0000000100000003ULL, 0);
  } catch (const std::length_error &) {
    exhausted = true;
  }
  assert(exhausted);
  assert(table.telemetry().exhaustion_count == 1);

  table.Release(0x0000000100000001ULL, 7);
  assert(!table.Find(0x0000000100000001ULL));
  assert(table.telemetry().retiring == 1);
  table.Collect(6);
  assert(table.telemetry().retiring == 1);

  exhausted = false;
  try {
    (void)table.Acquire(0x0000000200000001ULL, 6);
  } catch (const std::length_error &) {
    exhausted = true;
  }
  assert(exhausted);

  table.Collect(7);
  const auto replacement = table.Acquire(0x0000000200000001ULL, 7);
  assert(replacement.index == first.index);
  assert(replacement.generation != first.generation);
  assert(table.telemetry().reuses == 1);
  assert(table.telemetry().in_use == 2);
  return 0;
}
