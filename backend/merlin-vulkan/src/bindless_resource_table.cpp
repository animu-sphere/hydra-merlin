#include <merlin/vulkan/bindless_resource_table.hpp>

#include <algorithm>
#include <atomic>
#include <string>
#include <utility>

namespace merlin::vulkan {
namespace {

std::atomic<std::uint64_t> g_next_bindless_table_id{1};

std::uint32_t NextGeneration(std::uint32_t generation) noexcept {
  ++generation;
  return generation == 0 ? 1 : generation;
}

}  // namespace

BindlessSlotError::BindlessSlotError(BindlessSlotErrorCode code,
                                     std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

BindlessSlotAllocator::BindlessSlotAllocator(std::string_view label,
                                             std::uint32_t capacity,
                                             std::uint32_t reserved_slots)
    : label_(label),
      table_id_(g_next_bindless_table_id.fetch_add(1,
                                                   std::memory_order_relaxed)),
      slots_(capacity),
      dirty_slots_(capacity) {
  if (capacity < reserved_slots) {
    throw std::invalid_argument(
        label_ + " table capacity is smaller than its reserved slot count");
  }
  if (table_id_ == 0) {
    table_id_ = g_next_bindless_table_id.fetch_add(1,
                                                   std::memory_order_relaxed);
  }

  telemetry_.capacity = capacity;
  telemetry_.reserved_slots = reserved_slots;
  telemetry_.current_use = reserved_slots;
  telemetry_.peak_use = reserved_slots;
  telemetry_.available_slots = capacity - reserved_slots;

  for (std::uint32_t index = 0; index < reserved_slots; ++index) {
    slots_[index].state = State::Reserved;
    MarkDirty(index);
  }
  for (std::uint32_t index = capacity; index > reserved_slots; --index) {
    free_slots_.push_back(index - 1);
  }
}

BindlessSlotHandle BindlessSlotAllocator::HandleFor(
    std::uint32_t index) const noexcept {
  return {index, slots_[index].generation, table_id_};
}

void BindlessSlotAllocator::MarkDirty(std::uint32_t index) {
  if (!dirty_slots_[index]) {
    dirty_slots_[index] = true;
    ++telemetry_.dirty_descriptor_slots;
  }
}

[[noreturn]] void BindlessSlotAllocator::Throw(
    BindlessSlotErrorCode code, std::string_view detail) const {
  throw BindlessSlotError(code, label_ + " table: " + std::string(detail));
}

void BindlessSlotAllocator::ValidateOwnedHandle(BindlessSlotHandle slot) {
  if (!slot) {
    Throw(BindlessSlotErrorCode::InvalidHandle, "invalid slot handle");
  }
  if (slot.table_id != table_id_) {
    Throw(BindlessSlotErrorCode::ForeignHandle,
          "slot handle belongs to another table");
  }
  if (slot.index >= slots_.size()) {
    Throw(BindlessSlotErrorCode::InvalidHandle,
          "slot index is outside table capacity");
  }
  if (slot.generation != slots_[slot.index].generation) {
    ++telemetry_.generation_mismatch_count;
    Throw(BindlessSlotErrorCode::StaleGeneration,
          "slot generation is stale");
  }
}

BindlessSlotHandle BindlessSlotAllocator::Allocate() {
  if (free_slots_.empty()) {
    ++telemetry_.exhaustion_count;
    Throw(BindlessSlotErrorCode::Exhausted,
          "exhausted (capacity=" + std::to_string(telemetry_.capacity) +
              ", current=" + std::to_string(telemetry_.current_use) +
              ", retiring=" + std::to_string(telemetry_.retiring_slots) +
              ", reserved=" + std::to_string(telemetry_.reserved_slots) +
              ")");
  }
  const auto index = free_slots_.back();
  free_slots_.pop_back();
  auto& slot = slots_[index];
  slot.state = State::Active;
  --telemetry_.available_slots;
  ++telemetry_.current_use;
  telemetry_.peak_use =
      std::max(telemetry_.peak_use, telemetry_.current_use);
  ++telemetry_.allocation_count;
  if (slot.generation > 1) {
    ++telemetry_.reuse_count;
  }
  MarkDirty(index);
  return HandleFor(index);
}

bool BindlessSlotAllocator::IsActive(BindlessSlotHandle slot) const noexcept {
  if (!slot || slot.table_id != table_id_ || slot.index >= slots_.size()) {
    return false;
  }
  const auto& state = slots_[slot.index];
  return state.generation == slot.generation &&
         (state.state == State::Active || state.state == State::Reserved);
}

void BindlessSlotAllocator::RequireActive(BindlessSlotHandle slot) {
  ValidateOwnedHandle(slot);
  switch (slots_[slot.index].state) {
    case State::Active:
    case State::Reserved: return;
    case State::Free:
      Throw(BindlessSlotErrorCode::SlotNotAllocated,
            "slot is not allocated");
    case State::Retired:
      Throw(BindlessSlotErrorCode::SlotRetired,
            "slot is pending completion retirement");
  }
}

void BindlessSlotAllocator::Retire(BindlessSlotHandle slot,
                                   std::uint64_t last_completion_value) {
  ValidateOwnedHandle(slot);
  auto& state = slots_[slot.index];
  switch (state.state) {
    case State::Reserved:
      Throw(BindlessSlotErrorCode::ReservedSlot,
            "reserved slot cannot be retired");
    case State::Free:
      Throw(BindlessSlotErrorCode::SlotNotAllocated,
            "slot is not allocated");
    case State::Retired:
      Throw(BindlessSlotErrorCode::SlotRetired,
            "slot is already pending completion retirement");
    case State::Active: break;
  }

  state.state = State::Retired;
  retirements_.push_back({slot, last_completion_value});
  --telemetry_.current_use;
  ++telemetry_.retiring_slots;
  ++telemetry_.retirement_count;
}

std::vector<BindlessSlotHandle> BindlessSlotAllocator::Collect(
    std::uint64_t completed_value) {
  std::vector<BindlessSlotHandle> collected;
  auto retirement = retirements_.begin();
  while (retirement != retirements_.end()) {
    if (retirement->completion_value > completed_value) {
      ++retirement;
      continue;
    }
    const auto old_handle = retirement->slot;
    auto& slot = slots_[old_handle.index];
    slot.generation = NextGeneration(slot.generation);
    slot.state = State::Free;
    free_slots_.push_back(old_handle.index);
    MarkDirty(old_handle.index);
    --telemetry_.retiring_slots;
    ++telemetry_.available_slots;
    ++telemetry_.retirement_collection_count;
    collected.push_back(old_handle);
    retirement = retirements_.erase(retirement);
  }
  return collected;
}

BindlessSlotHandle BindlessSlotAllocator::ReservedSlot(
    std::uint32_t index) const {
  if (index >= slots_.size() || slots_[index].state != State::Reserved) {
    Throw(BindlessSlotErrorCode::InvalidHandle,
          "requested slot is not reserved");
  }
  return HandleFor(index);
}

std::vector<std::uint32_t> BindlessSlotAllocator::ConsumeDirtySlots() {
  std::vector<std::uint32_t> result;
  result.reserve(telemetry_.dirty_descriptor_slots);
  for (std::uint32_t index = 0; index < dirty_slots_.size(); ++index) {
    if (dirty_slots_[index]) {
      result.push_back(index);
      dirty_slots_[index] = false;
    }
  }
  telemetry_.dirty_descriptor_slots = 0;
  telemetry_.descriptor_update_count += result.size();
  return result;
}

BindlessTextureTable::BindlessTextureTable(std::uint32_t capacity)
    : slots_("bindless texture", capacity,
             kReservedBindlessTextureSlots) {}

BindlessSamplerTable::BindlessSamplerTable(std::uint32_t capacity)
    : slots_("bindless sampler", capacity),
      descriptors_by_slot_(capacity) {}

BindlessSlotHandle BindlessSamplerTable::Acquire(
    const BindlessSamplerDescriptor& descriptor) {
  const auto existing = entries_.find(descriptor);
  if (existing != entries_.end()) {
    slots_.RequireActive(existing->second.slot);
    ++existing->second.reference_count;
    ++current_reference_count_;
    peak_reference_count_ =
        std::max(peak_reference_count_, current_reference_count_);
    ++deduplication_hit_count_;
    return existing->second.slot;
  }

  const auto slot = slots_.Allocate();
  entries_.emplace(descriptor, Entry{slot, 1, 0});
  descriptors_by_slot_[slot.index] = descriptor;
  ++current_reference_count_;
  peak_reference_count_ =
      std::max(peak_reference_count_, current_reference_count_);
  return slot;
}

void BindlessSamplerTable::Release(BindlessSlotHandle slot,
                                   std::uint64_t last_completion_value) {
  slots_.RequireActive(slot);
  const auto& descriptor = descriptors_by_slot_[slot.index];
  if (!descriptor) {
    throw std::logic_error("active bindless sampler slot has no descriptor");
  }
  const auto existing = entries_.find(*descriptor);
  if (existing == entries_.end() || existing->second.slot != slot ||
      existing->second.reference_count == 0) {
    throw std::logic_error("bindless sampler reference state is inconsistent");
  }

  auto& entry = existing->second;
  entry.last_completion_value =
      std::max(entry.last_completion_value, last_completion_value);
  --entry.reference_count;
  --current_reference_count_;
  if (entry.reference_count != 0) {
    return;
  }

  const auto completion_value = entry.last_completion_value;
  descriptors_by_slot_[slot.index].reset();
  entries_.erase(existing);
  slots_.Retire(slot, completion_value);
}

BindlessSamplerTelemetry BindlessSamplerTable::telemetry() const noexcept {
  BindlessSamplerTelemetry result;
  result.slots = slots_.telemetry();
  result.unique_sampler_count = static_cast<std::uint32_t>(entries_.size());
  result.current_reference_count = current_reference_count_;
  result.peak_reference_count = peak_reference_count_;
  result.deduplication_hit_count = deduplication_hit_count_;
  return result;
}

}  // namespace merlin::vulkan
