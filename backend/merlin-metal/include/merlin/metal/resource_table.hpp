#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace merlin::metal {

struct ResourceSlot {
  std::uint32_t index{};
  std::uint32_t generation{};

  friend constexpr bool operator==(const ResourceSlot &,
                                   const ResourceSlot &) = default;
};

struct ResourceTableTelemetry {
  std::uint32_t capacity{};
  std::uint32_t in_use{};
  std::uint32_t retiring{};
  std::uint64_t acquisitions{};
  std::uint64_t releases{};
  std::uint64_t reuses{};
  std::uint64_t exhaustion_count{};
};

// Backend-neutral bookkeeping for Metal argument-buffer entries. A slot is
// identified by both index and generation, and cannot be reused until the last
// command buffer that could reference it has completed.
class StableResourceTable {
public:
  explicit StableResourceTable(std::uint32_t capacity);

  [[nodiscard]] ResourceSlot Acquire(std::uint64_t resource,
                                     std::uint64_t completed_value);
  [[nodiscard]] std::optional<ResourceSlot>
  Find(std::uint64_t resource) const noexcept;
  void Release(std::uint64_t resource, std::uint64_t retire_value);
  void Collect(std::uint64_t completed_value) noexcept;

  [[nodiscard]] ResourceTableTelemetry telemetry() const noexcept;

private:
  enum class State { Free, Active, Retiring };

  struct Entry {
    std::uint64_t resource{};
    std::uint64_t retire_value{};
    std::uint32_t generation{};
    State state{State::Free};
  };

  std::vector<Entry> entries_;
  std::vector<std::uint32_t> free_;
  std::unordered_map<std::uint64_t, std::uint32_t> active_;
  ResourceTableTelemetry telemetry_;
};

} // namespace merlin::metal
