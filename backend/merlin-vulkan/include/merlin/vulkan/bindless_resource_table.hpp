#pragma once

#include <merlin/core/types.hpp>
#include <merlin/vulkan/descriptor_indexing.hpp>

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace merlin::vulkan {

inline constexpr std::uint32_t kInvalidBindlessSlotIndex = ~std::uint32_t{};

struct BindlessSlotHandle {
  std::uint32_t index{kInvalidBindlessSlotIndex};
  std::uint32_t generation{};
  std::uint64_t table_id{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return index != kInvalidBindlessSlotIndex && generation != 0 &&
           table_id != 0;
  }

  auto operator<=>(const BindlessSlotHandle&) const = default;
};

enum class BindlessSlotErrorCode {
  InvalidHandle,
  ForeignHandle,
  ReservedSlot,
  SlotNotAllocated,
  SlotRetired,
  StaleGeneration,
  Exhausted,
};

class BindlessSlotError : public std::runtime_error {
 public:
  BindlessSlotError(BindlessSlotErrorCode code, std::string message);

  [[nodiscard]] BindlessSlotErrorCode code() const noexcept { return code_; }

 private:
  BindlessSlotErrorCode code_;
};

// Counts describe logical descriptor slots. current_use includes reserved and
// live user slots but excludes completion-delayed retirements, which are
// reported separately and remain unavailable until collected.
struct BindlessSlotTelemetry {
  std::uint32_t schema_version{1};
  std::uint32_t capacity{};
  std::uint32_t reserved_slots{};
  std::uint32_t current_use{};
  std::uint32_t peak_use{};
  std::uint32_t retiring_slots{};
  std::uint32_t available_slots{};
  std::uint32_t dirty_descriptor_slots{};
  std::uint64_t allocation_count{};
  std::uint64_t reuse_count{};
  std::uint64_t retirement_count{};
  std::uint64_t retirement_collection_count{};
  std::uint64_t descriptor_update_count{};
  std::uint64_t exhaustion_count{};
  std::uint64_t generation_mismatch_count{};
};

// Finite, generation-checked slot storage shared by the typed texture and
// sampler tables. Slots enter the free list, and advance generation, only once
// their last GPU completion value has been collected.
class BindlessSlotAllocator {
 public:
  BindlessSlotAllocator(std::string_view label, std::uint32_t capacity,
                        std::uint32_t reserved_slots = 0);
  BindlessSlotAllocator(const BindlessSlotAllocator&) = delete;
  BindlessSlotAllocator& operator=(const BindlessSlotAllocator&) = delete;

  [[nodiscard]] BindlessSlotHandle Allocate();
  void Retire(BindlessSlotHandle slot, std::uint64_t last_completion_value);
  [[nodiscard]] std::vector<BindlessSlotHandle> Collect(
      std::uint64_t completed_value);

  [[nodiscard]] bool IsActive(BindlessSlotHandle slot) const noexcept;
  void RequireActive(BindlessSlotHandle slot);
  [[nodiscard]] BindlessSlotHandle ReservedSlot(std::uint32_t index) const;
  [[nodiscard]] std::vector<std::uint32_t> ConsumeDirtySlots();
  [[nodiscard]] const BindlessSlotTelemetry& telemetry() const noexcept {
    return telemetry_;
  }

 private:
  enum class State : std::uint8_t { Free, Reserved, Active, Retired };

  struct Slot {
    std::uint32_t generation{1};
    State state{State::Free};
  };

  struct Retirement {
    BindlessSlotHandle slot;
    std::uint64_t completion_value{};
  };

  [[nodiscard]] BindlessSlotHandle HandleFor(
      std::uint32_t index) const noexcept;
  void ValidateOwnedHandle(BindlessSlotHandle slot);
  void MarkDirty(std::uint32_t index);
  [[noreturn]] void Throw(BindlessSlotErrorCode code,
                          std::string_view detail) const;

  std::string label_;
  std::uint64_t table_id_{};
  std::vector<Slot> slots_;
  std::vector<std::uint32_t> free_slots_;
  std::vector<Retirement> retirements_;
  std::vector<bool> dirty_slots_;
  BindlessSlotTelemetry telemetry_;
};

enum class BindlessFallbackTexture : std::uint32_t {
  White = 0,
  Black = 1,
  FlatNormal = 2,
  Error = 3,
};

class BindlessTextureTable {
 public:
  explicit BindlessTextureTable(std::uint32_t capacity);

  [[nodiscard]] BindlessSlotHandle Allocate() { return slots_.Allocate(); }
  void Retire(BindlessSlotHandle slot, std::uint64_t last_completion_value) {
    slots_.Retire(slot, last_completion_value);
  }
  [[nodiscard]] std::vector<BindlessSlotHandle> Collect(
      std::uint64_t completed_value) {
    return slots_.Collect(completed_value);
  }
  [[nodiscard]] bool IsActive(BindlessSlotHandle slot) const noexcept {
    return slots_.IsActive(slot);
  }
  [[nodiscard]] BindlessSlotHandle fallback(
      BindlessFallbackTexture texture) const {
    return slots_.ReservedSlot(static_cast<std::uint32_t>(texture));
  }
  [[nodiscard]] std::vector<std::uint32_t> ConsumeDirtySlots() {
    return slots_.ConsumeDirtySlots();
  }
  [[nodiscard]] const BindlessSlotTelemetry& telemetry() const noexcept {
    return slots_.telemetry();
  }

 private:
  BindlessSlotAllocator slots_;
};

// Descriptor identity deliberately excludes debug labels and contains only
// values that affect VkSampler creation.
struct BindlessSamplerDescriptor {
  FilterMode min_filter{FilterMode::Linear};
  FilterMode mag_filter{FilterMode::Linear};
  AddressMode address_u{AddressMode::Repeat};
  AddressMode address_v{AddressMode::Repeat};

  BindlessSamplerDescriptor() = default;
  explicit BindlessSamplerDescriptor(const SamplerDescriptor& descriptor)
      : min_filter(descriptor.min_filter),
        mag_filter(descriptor.mag_filter),
        address_u(descriptor.address_u),
        address_v(descriptor.address_v) {}

  auto operator<=>(const BindlessSamplerDescriptor&) const = default;
};

struct BindlessSamplerTelemetry {
  std::uint32_t schema_version{1};
  BindlessSlotTelemetry slots;
  std::uint32_t unique_sampler_count{};
  std::uint64_t current_reference_count{};
  std::uint64_t peak_reference_count{};
  std::uint64_t deduplication_hit_count{};
};

// Acquire/Release is reference counted by descriptor value. The final Release
// retires the unique slot at the greatest completion value observed across all
// references to that sampler.
class BindlessSamplerTable {
 public:
  explicit BindlessSamplerTable(std::uint32_t capacity);

  [[nodiscard]] BindlessSlotHandle Acquire(
      const BindlessSamplerDescriptor& descriptor);
  void Release(BindlessSlotHandle slot,
               std::uint64_t last_completion_value);
  [[nodiscard]] std::vector<BindlessSlotHandle> Collect(
      std::uint64_t completed_value) {
    return slots_.Collect(completed_value);
  }
  [[nodiscard]] bool IsActive(BindlessSlotHandle slot) const noexcept {
    return slots_.IsActive(slot);
  }
  [[nodiscard]] std::vector<std::uint32_t> ConsumeDirtySlots() {
    return slots_.ConsumeDirtySlots();
  }
  [[nodiscard]] BindlessSamplerTelemetry telemetry() const noexcept;

 private:
  struct Entry {
    BindlessSlotHandle slot;
    std::uint64_t reference_count{};
    std::uint64_t last_completion_value{};
  };

  BindlessSlotAllocator slots_;
  std::map<BindlessSamplerDescriptor, Entry> entries_;
  std::vector<std::optional<BindlessSamplerDescriptor>> descriptors_by_slot_;
  std::uint64_t current_reference_count_{};
  std::uint64_t peak_reference_count_{};
  std::uint64_t deduplication_hit_count_{};
};

}  // namespace merlin::vulkan
