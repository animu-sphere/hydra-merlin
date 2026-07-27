#include <merlin/metal/resource_table.hpp>

#include <stdexcept>

namespace merlin::metal {

StableResourceTable::StableResourceTable(std::uint32_t capacity)
    : entries_(capacity) {
  if (capacity == 0) {
    throw std::invalid_argument("resource table capacity must be non-zero");
  }
  free_.reserve(capacity);
  for (std::uint32_t index = capacity; index != 0; --index) {
    free_.push_back(index - 1U);
  }
  telemetry_.capacity = capacity;
}

ResourceSlot StableResourceTable::Acquire(std::uint64_t resource,
                                          std::uint64_t completed_value) {
  if (resource == 0) {
    throw std::invalid_argument("resource identity must be non-zero");
  }
  Collect(completed_value);
  if (const auto found = active_.find(resource); found != active_.end()) {
    const auto &entry = entries_[found->second];
    return {found->second, entry.generation};
  }
  if (free_.empty()) {
    ++telemetry_.exhaustion_count;
    throw std::length_error("stable resource table capacity exhausted");
  }
  const auto index = free_.back();
  free_.pop_back();
  auto &entry = entries_[index];
  ++entry.generation;
  if (entry.generation == 0) {
    ++entry.generation;
  }
  if (entry.resource != 0) {
    ++telemetry_.reuses;
  }
  entry.resource = resource;
  entry.retire_value = 0;
  entry.state = State::Active;
  active_.emplace(resource, index);
  ++telemetry_.acquisitions;
  ++telemetry_.in_use;
  return {index, entry.generation};
}

std::optional<ResourceSlot>
StableResourceTable::Find(std::uint64_t resource) const noexcept {
  const auto found = active_.find(resource);
  if (found == active_.end()) {
    return std::nullopt;
  }
  const auto &entry = entries_[found->second];
  return ResourceSlot{found->second, entry.generation};
}

void StableResourceTable::Release(std::uint64_t resource,
                                  std::uint64_t retire_value) {
  const auto found = active_.find(resource);
  if (found == active_.end()) {
    throw std::invalid_argument("resource is not resident in the table");
  }
  auto &entry = entries_[found->second];
  active_.erase(found);
  entry.retire_value = retire_value;
  entry.state = State::Retiring;
  ++telemetry_.releases;
  --telemetry_.in_use;
  ++telemetry_.retiring;
}

void StableResourceTable::Collect(std::uint64_t completed_value) noexcept {
  for (std::uint32_t index = 0; index < entries_.size(); ++index) {
    auto &entry = entries_[index];
    if (entry.state == State::Retiring &&
        entry.retire_value <= completed_value) {
      entry.state = State::Free;
      entry.retire_value = 0;
      free_.push_back(index);
      --telemetry_.retiring;
    }
  }
}

ResourceTableTelemetry StableResourceTable::telemetry() const noexcept {
  return telemetry_;
}

} // namespace merlin::metal
