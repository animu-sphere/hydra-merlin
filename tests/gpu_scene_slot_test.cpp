#include <merlin/render/gpu_scene.hpp>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using merlin::extraction::DrawRecord;
using merlin::extraction::FrameSnapshot;
using merlin::extraction::SnapshotDelta;
using merlin::render::GpuSceneDrawSlots;
using merlin::render::GpuSceneSlotAllocator;
using merlin::render::GpuSceneSlotError;
using merlin::render::GpuSceneSlotErrorCode;
using merlin::render::GpuSceneSlotHandle;

template <typename Callback>
void ExpectError(Callback&& callback, GpuSceneSlotErrorCode code,
                 std::string_view fragment) {
  try {
    callback();
    assert(false && "expected GpuSceneSlotError");
  } catch (const GpuSceneSlotError& error) {
    assert(error.code() == code);
    assert(std::string_view(error.what()).find(fragment) !=
           std::string_view::npos);
  }
}

FrameSnapshot Snapshot(std::uint64_t source, std::uint64_t revision,
                       std::vector<std::pair<std::uint64_t, std::uint64_t>> draws,
                       std::optional<SnapshotDelta> delta = std::nullopt) {
  FrameSnapshot snapshot;
  snapshot.source_id = source;
  snapshot.revision = revision;
  std::vector<DrawRecord> records;
  records.reserve(draws.size());
  for (const auto& [draw, record_revision] : draws) {
    DrawRecord record;
    record.draw = draw;
    record.revision = record_revision;
    records.push_back(record);
  }
  snapshot.draws.assign(std::move(records));
  snapshot.delta = std::move(delta);
  return snapshot;
}

SnapshotDelta DrawDelta(std::uint64_t base_revision,
                        std::vector<std::uint64_t> upserts,
                        std::vector<std::uint64_t> removals,
                        std::vector<std::uint32_t> upsert_indices) {
  SnapshotDelta delta;
  delta.base_revision = base_revision;
  delta.draws.upserts = std::move(upserts);
  delta.draws.removals = std::move(removals);
  delta.draws.upsert_indices = std::move(upsert_indices);
  return delta;
}

void TestSlotLifetime() {
  GpuSceneSlotAllocator slots("test slots", 2);
  GpuSceneSlotAllocator foreign("foreign slots", 1);
  const auto first = slots.Allocate();
  const auto second = slots.Allocate();
  assert(first.index == 0);
  assert(second.index == 1);
  assert(first.generation == 1);
  assert(slots.IsActive(first));
  assert(slots.telemetry().active_slots == 2);

  ExpectError([&] { (void)slots.Allocate(); },
              GpuSceneSlotErrorCode::Exhausted, "capacity=2");
  ExpectError([&] { slots.RequireActive(foreign.Allocate()); },
              GpuSceneSlotErrorCode::ForeignHandle, "another allocator");

  slots.Retire(first, 7);
  assert(!slots.IsActive(first));
  ExpectError([&] { slots.RequireActive(first); },
              GpuSceneSlotErrorCode::SlotRetired, "pending");
  assert(slots.Collect(6).empty());
  ExpectError([&] { (void)slots.Allocate(); },
              GpuSceneSlotErrorCode::Exhausted, "retiring=1");

  assert(slots.Collect(7) == std::vector<GpuSceneSlotHandle>{first});
  const auto replacement = slots.Allocate();
  assert(replacement.index == first.index);
  assert(replacement.generation == first.generation + 1);
  ExpectError([&] { slots.RequireActive(first); },
              GpuSceneSlotErrorCode::StaleGeneration, "stale");

  const auto telemetry = slots.telemetry();
  assert(telemetry.schema_version == 1);
  assert(telemetry.capacity == 2);
  assert(telemetry.active_slots == 2);
  assert(telemetry.peak_active_slots == 2);
  assert(telemetry.retiring_slots == 0);
  assert(telemetry.available_slots == 0);
  assert(telemetry.allocation_count == 3);
  assert(telemetry.reuse_count == 1);
  assert(telemetry.retirement_count == 1);
  assert(telemetry.retirement_collection_count == 1);
  assert(telemetry.exhaustion_count == 2);
  assert(telemetry.generation_mismatch_count == 1);
}

void TestPersistentDrawSlots() {
  GpuSceneDrawSlots slots(3);
  const auto initial = Snapshot(11, 1, {{10, 1}, {20, 1}});
  const auto initial_plan = slots.Apply(initial, 0, 0);
  assert(initial_plan.full_reconciliation);
  assert(initial_plan.upserts.size() == 2);
  assert(initial_plan.retirements.empty());
  assert(initial_plan.upserts[0].draw == 10);
  assert(initial_plan.upserts[0].slot.index == 0);
  assert(initial_plan.upserts[1].draw == 20);
  assert(initial_plan.upserts[1].slot.index == 1);
  const auto original_ten = *slots.Find(10);
  const auto original_twenty = *slots.Find(20);

  // Re-applying one immutable snapshot performs no residency or upload work.
  const auto static_plan = slots.Apply(initial, 0, 0);
  assert(!static_plan.full_reconciliation);
  assert(static_plan.upserts.empty());
  assert(static_plan.retirements.empty());
  assert(slots.Find(10) == original_ten);

  // A changed record receives a fresh slot while the old generation remains
  // unavailable until the last frame that referenced it completes.
  const auto changed_ten = Snapshot(
      11, 2, {{10, 2}, {20, 1}}, DrawDelta(1, {10}, {}, {0}));
  const auto change_plan = slots.Apply(changed_ten, 5, 4);
  assert(!change_plan.full_reconciliation);
  assert(change_plan.upserts.size() == 1);
  assert(change_plan.retirements.size() == 1);
  assert(change_plan.retirements[0].slot == original_ten);
  const auto replacement_ten = *slots.Find(10);
  assert(replacement_ten.index == 2);
  assert(slots.Find(20) == original_twenty);
  assert(slots.telemetry().retiring_slots == 1);

  // With every physical slot active or retiring, a second in-flight
  // replacement fails before mutating the accepted revision or mapping.
  const auto changed_twenty = Snapshot(
      11, 3, {{10, 2}, {20, 3}}, DrawDelta(2, {20}, {}, {1}));
  ExpectError([&] { (void)slots.Apply(changed_twenty, 6, 4); },
              GpuSceneSlotErrorCode::Exhausted, "completion-safe");
  assert(slots.revision() == 2);
  assert(slots.Find(20) == original_twenty);

  // Once the first retirement completes its physical index is reused with a
  // new generation. The failed Apply did not consume the exact delta base.
  const auto twenty_plan = slots.Apply(changed_twenty, 6, 5);
  assert(!twenty_plan.full_reconciliation);
  assert(twenty_plan.collected ==
         std::vector<GpuSceneSlotHandle>{original_ten});
  assert(twenty_plan.upserts.size() == 1);
  const auto replacement_twenty = *slots.Find(20);
  assert(replacement_twenty.index == original_ten.index);
  assert(replacement_twenty.generation == original_ten.generation + 1);

  const auto removed_ten = Snapshot(
      11, 4, {{20, 3}}, DrawDelta(3, {}, {10}, {}));
  const auto remove_plan = slots.Apply(removed_ten, 7, 6);
  assert(!remove_plan.full_reconciliation);
  assert(remove_plan.upserts.empty());
  assert(remove_plan.retirements.size() == 1);
  assert(!slots.Find(10));

  // A malformed/missing delta index cannot be trusted, so the full table is
  // reconciled. Unchanged identities from the same source retain their slot.
  const auto malformed_delta = Snapshot(
      11, 5, {{20, 3}, {30, 5}}, DrawDelta(4, {30}, {}, {}));
  const auto reconcile_plan = slots.Apply(malformed_delta, 7, 7);
  assert(reconcile_plan.full_reconciliation);
  assert(reconcile_plan.upserts.size() == 1);
  assert(reconcile_plan.upserts[0].draw == 30);
  assert(slots.Find(20) == replacement_twenty);

  // Draw IDs are source-local. The same numeric ID from another source must
  // receive a new generation rather than alias the previous source's record.
  const auto before_source_change = *slots.Find(20);
  const auto other_source = Snapshot(22, 1, {{20, 1}});
  const auto source_plan = slots.Apply(other_source, 7, 8);
  assert(source_plan.full_reconciliation);
  assert(source_plan.retirements.size() == 2);
  assert(source_plan.upserts.size() == 1);
  assert(*slots.Find(20) != before_source_change);
  assert(slots.source_id() == 22);
  assert(slots.revision() == 1);
}

void TestInvalidSnapshots() {
  GpuSceneDrawSlots slots(2);
  ExpectError([&] { (void)slots.Apply(Snapshot(1, 1, {{0, 1}}), 0, 0); },
              GpuSceneSlotErrorCode::InvalidSnapshot, "non-zero");
  ExpectError(
      [&] {
        (void)slots.Apply(Snapshot(1, 1, {{10, 1}, {10, 1}}), 0, 0);
      },
      GpuSceneSlotErrorCode::InvalidSnapshot, "unique");

  const auto current = Snapshot(2, 2, {{20, 2}});
  (void)slots.Apply(current, 0, 0);
  ExpectError([&] { (void)slots.Apply(Snapshot(2, 1, {{20, 1}}), 0, 0); },
              GpuSceneSlotErrorCode::InvalidSnapshot,
              "precedes resident revision");
}

}  // namespace

int main() {
  TestSlotLifetime();
  TestPersistentDrawSlots();
  TestInvalidSnapshots();
  std::cout << "GPU Scene slot tests passed\n";
  return 0;
}
