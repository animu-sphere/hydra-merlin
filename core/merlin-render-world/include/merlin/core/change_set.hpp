#pragma once

#include <cstdint>
#include <vector>

namespace merlin {

enum class ObjectKind { Mesh, Material, Instance, Camera, Light };
enum class ChangeKind { Created, Updated, Removed };

struct Change {
  ObjectKind object_kind{};
  ChangeKind change_kind{};
  std::uint64_t handle{};
};

struct ChangeSet {
  std::uint64_t revision{};
  std::vector<Change> changes;

  [[nodiscard]] bool empty() const noexcept { return changes.empty(); }
};

}  // namespace merlin
