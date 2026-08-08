#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <merlin/extraction/frame_snapshot.hpp>
#include <merlin/render/gpu_scene_abi.hpp>

namespace merlin::render {

inline constexpr std::uint32_t kInvalidGpuSceneSlotIndex = ~std::uint32_t{};

// CPU-visible identity for one finite GPU Scene table slot. Shaders consume
// only index; owner and generation prevent stale CPU references from silently
// aliasing a slot after completion-safe reuse.
struct GpuSceneSlotHandle {
  std::uint32_t index{kInvalidGpuSceneSlotIndex};
  std::uint32_t generation{};
  std::uint64_t owner{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return index != kInvalidGpuSceneSlotIndex && generation != 0 && owner != 0;
  }

  auto operator<=>(const GpuSceneSlotHandle&) const = default;
};

enum class GpuSceneSlotErrorCode {
  InvalidHandle,
  ForeignHandle,
  SlotNotAllocated,
  SlotRetired,
  StaleGeneration,
  Exhausted,
  InvalidSnapshot,
};

class GpuSceneSlotError : public std::runtime_error {
 public:
  GpuSceneSlotError(GpuSceneSlotErrorCode code, std::string message);

  [[nodiscard]] GpuSceneSlotErrorCode code() const noexcept { return code_; }

 private:
  GpuSceneSlotErrorCode code_;
};

struct GpuSceneSlotTelemetry {
  std::uint32_t schema_version{1};
  std::uint32_t capacity{};
  std::uint32_t active_slots{};
  std::uint32_t peak_active_slots{};
  std::uint32_t retiring_slots{};
  std::uint32_t available_slots{};
  std::uint64_t allocation_count{};
  std::uint64_t reuse_count{};
  std::uint64_t retirement_count{};
  std::uint64_t retirement_collection_count{};
  std::uint64_t exhaustion_count{};
  std::uint64_t generation_mismatch_count{};
};

// Backend-neutral finite-slot primitive for geometry, instance, material, draw,
// and Gaussian GPU Scene tables. Retire removes a slot from active use, but the
// index is not reusable and its generation does not advance until Collect sees
// the last completion value that may still reference it.
class GpuSceneSlotAllocator {
 public:
  explicit GpuSceneSlotAllocator(std::string_view label,
                                 std::uint32_t capacity);
  GpuSceneSlotAllocator(const GpuSceneSlotAllocator&) = delete;
  GpuSceneSlotAllocator& operator=(const GpuSceneSlotAllocator&) = delete;

  [[nodiscard]] GpuSceneSlotHandle Allocate();
  void Retire(GpuSceneSlotHandle slot, std::uint64_t last_completion_value);
  [[nodiscard]] std::vector<GpuSceneSlotHandle> Collect(
      std::uint64_t completed_value);
  [[nodiscard]] std::size_t CollectableCount(
      std::uint64_t completed_value) const noexcept;

  [[nodiscard]] bool IsActive(GpuSceneSlotHandle slot) const noexcept;
  void RequireActive(GpuSceneSlotHandle slot);
  [[nodiscard]] const GpuSceneSlotTelemetry& telemetry() const noexcept {
    return telemetry_;
  }

 private:
  friend class GpuSceneDrawSlots;

  enum class State : std::uint8_t { Free, Active, Retired };

  struct Slot {
    std::uint32_t generation{1};
    State state{State::Free};
  };

  struct Retirement {
    GpuSceneSlotHandle slot;
    std::uint64_t completion_value{};
  };

  [[nodiscard]] GpuSceneSlotHandle HandleFor(
      std::uint32_t index) const noexcept;
  void ValidateOwnedHandle(GpuSceneSlotHandle slot);
  void RequireAvailable(std::size_t required,
                        std::size_t immediately_reclaimable = 0);
  [[noreturn]] void Throw(GpuSceneSlotErrorCode code,
                          std::string_view detail) const;

  std::string label_;
  std::uint64_t owner_{};
  std::vector<Slot> slots_;
  std::vector<std::uint32_t> free_slots_;
  std::vector<Retirement> retirements_;
  GpuSceneSlotTelemetry telemetry_;
};

struct GpuSceneDrawUpsert {
  std::uint64_t draw{};
  GpuSceneSlotHandle slot;
  std::uint32_t snapshot_index{};
  std::uint64_t record_revision{};
};

struct GpuSceneDrawRetirement {
  std::uint64_t draw{};
  GpuSceneSlotHandle slot;
};

// Describes the CPU-to-GPU work caused by accepting one snapshot. A full
// reconciliation compares all draw identities; an exact source/base-revision
// delta visits only its named records. Upserts always name fresh slots when a
// resident record changes so in-flight frames keep immutable table contents.
struct GpuSceneDrawUpdatePlan {
  std::uint64_t source_id{};
  std::uint64_t base_revision{};
  std::uint64_t revision{};
  // Number of snapshot draw records indexed while classifying this update.
  // Static frames index none, exact deltas index only named upserts, and full
  // reconciliation indexes the complete table.
  std::uint64_t indexed_snapshot_draws{};
  bool full_reconciliation{};
  std::vector<GpuSceneDrawUpsert> upserts;
  std::vector<GpuSceneDrawRetirement> retirements;
  std::vector<GpuSceneSlotHandle> collected;
};

// Persistent draw identity-to-slot mapping over FrameSnapshot. The class owns
// CPU residency and update planning only; native buffer allocation and copies
// remain backend responsibilities.
class GpuSceneDrawSlots {
 public:
  explicit GpuSceneDrawSlots(std::uint32_t capacity);
  GpuSceneDrawSlots(const GpuSceneDrawSlots&) = delete;
  GpuSceneDrawSlots& operator=(const GpuSceneDrawSlots&) = delete;

  [[nodiscard]] GpuSceneDrawUpdatePlan Apply(
      const extraction::FrameSnapshot& snapshot,
      std::uint64_t last_completion_value,
      std::uint64_t completed_value);
  [[nodiscard]] std::vector<GpuSceneSlotHandle> Collect(
      std::uint64_t completed_value) {
    return slots_.Collect(completed_value);
  }

  [[nodiscard]] std::optional<GpuSceneSlotHandle> Find(
      std::uint64_t draw) const noexcept;
  [[nodiscard]] std::uint64_t source_id() const noexcept { return source_id_; }
  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
  [[nodiscard]] std::size_t size() const noexcept { return resident_.size(); }
  [[nodiscard]] const GpuSceneSlotTelemetry& telemetry() const noexcept {
    return slots_.telemetry();
  }

 private:
  struct ResidentDraw {
    GpuSceneSlotHandle slot;
    std::uint64_t record_revision{};
  };

  GpuSceneSlotAllocator slots_;
  std::map<std::uint64_t, ResidentDraw> resident_;
  std::uint64_t source_id_{};
  std::uint64_t revision_{};
};

}  // namespace merlin::render
