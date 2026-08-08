#include <merlin/render/gpu_scene.hpp>

#include <algorithm>
#include <atomic>
#include <limits>
#include <set>
#include <utility>

namespace merlin::render {
namespace {

std::atomic<std::uint64_t> g_next_gpu_scene_slot_owner{1};

std::uint32_t NextGeneration(std::uint32_t generation) noexcept {
  ++generation;
  return generation == 0 ? 1 : generation;
}

[[noreturn]] void ThrowInvalidSnapshot(std::string_view detail) {
  throw GpuSceneSlotError(GpuSceneSlotErrorCode::InvalidSnapshot,
                          "GPU Scene draw slots: " + std::string(detail));
}

struct PendingUpsert {
  std::uint64_t draw{};
  std::uint32_t snapshot_index{};
  std::uint64_t record_revision{};
};

struct PendingResourceUpsert {
  std::uint64_t resource{};
  std::uint32_t snapshot_index{};
  GpuSceneResourceVersion record_version;
};

std::string_view ResourceTableName(GpuSceneResourceTable table) noexcept {
  switch (table) {
    case GpuSceneResourceTable::Geometry: return "geometry";
    case GpuSceneResourceTable::Instance: return "instance";
    case GpuSceneResourceTable::Material: return "material";
  }
  return "unknown";
}

[[noreturn]] void ThrowInvalidResourceSnapshot(
    GpuSceneResourceTable table, std::string_view detail) {
  throw GpuSceneSlotError(
      GpuSceneSlotErrorCode::InvalidSnapshot,
      "GPU Scene " + std::string(ResourceTableName(table)) +
          " slots: " + std::string(detail));
}

const extraction::ResourceDelta& ResourceDeltaFor(
    const extraction::SnapshotDelta& delta, GpuSceneResourceTable table) {
  switch (table) {
    case GpuSceneResourceTable::Geometry: return delta.geometries;
    case GpuSceneResourceTable::Instance: return delta.instances;
    case GpuSceneResourceTable::Material: return delta.materials;
  }
  ThrowInvalidResourceSnapshot(table, "unknown resource table");
}

std::size_t ResourceCount(const extraction::FrameSnapshot& snapshot,
                          GpuSceneResourceTable table) noexcept {
  switch (table) {
    case GpuSceneResourceTable::Geometry: return snapshot.geometries.size();
    case GpuSceneResourceTable::Instance: return snapshot.instances.size();
    case GpuSceneResourceTable::Material: return snapshot.materials.size();
  }
  return 0;
}

std::uint64_t ResourceIdentityAt(const extraction::FrameSnapshot& snapshot,
                                 GpuSceneResourceTable table,
                                 std::uint32_t index) {
  switch (table) {
    case GpuSceneResourceTable::Geometry:
      return snapshot.geometries[index].mesh;
    case GpuSceneResourceTable::Instance:
      return snapshot.instances[index].instance;
    case GpuSceneResourceTable::Material:
      return snapshot.materials[index].material;
  }
  ThrowInvalidResourceSnapshot(table, "unknown resource table");
}

GpuSceneResourceVersion ResourceVersionAt(
    const extraction::FrameSnapshot& snapshot, GpuSceneResourceTable table,
    std::uint32_t index) {
  switch (table) {
    case GpuSceneResourceTable::Geometry: {
      const auto& geometry = snapshot.geometries[index];
      return {geometry.vertex_revision, geometry.index_revision};
    }
    case GpuSceneResourceTable::Instance:
      return {snapshot.instances[index].revision, 0};
    case GpuSceneResourceTable::Material:
      return {snapshot.materials[index].revision, 0};
  }
  ThrowInvalidResourceSnapshot(table, "unknown resource table");
}

bool ReadUsableResourceDelta(
    const extraction::FrameSnapshot& snapshot, GpuSceneResourceTable table,
    std::uint64_t source_id, std::uint64_t revision,
    std::vector<PendingResourceUpsert>& upserts,
    std::vector<std::uint64_t>& removals) {
  if (source_id == 0 || snapshot.source_id != source_id || !snapshot.delta ||
      snapshot.delta->base_revision != revision) {
    return false;
  }

  const auto& delta = ResourceDeltaFor(*snapshot.delta, table);
  if (delta.upserts.size() != delta.upsert_indices.size()) {
    return false;
  }

  upserts.reserve(delta.upserts.size());
  removals.reserve(delta.removals.size());
  std::set<std::uint64_t> changed;
  const auto record_count = ResourceCount(snapshot, table);
  for (std::size_t index = 0; index < delta.upserts.size(); ++index) {
    const auto resource = delta.upserts[index];
    const auto snapshot_index = delta.upsert_indices[index];
    if (resource == 0 || snapshot_index >= record_count ||
        ResourceIdentityAt(snapshot, table, snapshot_index) != resource ||
        !changed.insert(resource).second) {
      upserts.clear();
      removals.clear();
      return false;
    }
    upserts.push_back({resource, snapshot_index,
                       ResourceVersionAt(snapshot, table, snapshot_index)});
  }
  for (const auto resource : delta.removals) {
    if (resource == 0 || !changed.insert(resource).second) {
      upserts.clear();
      removals.clear();
      return false;
    }
    removals.push_back(resource);
  }
  return true;
}

std::vector<GpuSceneDirtyRange> BuildDirtyRanges(
    std::vector<std::uint32_t> slots) {
  if (slots.empty()) {
    return {};
  }
  std::sort(slots.begin(), slots.end());
  slots.erase(std::unique(slots.begin(), slots.end()), slots.end());

  std::vector<GpuSceneDirtyRange> ranges;
  auto first = slots.front();
  auto previous = first;
  for (std::size_t index = 1; index < slots.size(); ++index) {
    if (slots[index] == previous + 1U) {
      previous = slots[index];
      continue;
    }
    ranges.push_back({first, previous - first + 1U});
    first = slots[index];
    previous = first;
  }
  ranges.push_back({first, previous - first + 1U});
  return ranges;
}

bool ReadUsableDelta(const extraction::FrameSnapshot& snapshot,
                     std::uint64_t source_id, std::uint64_t revision,
                     std::vector<PendingUpsert>& upserts,
                     std::vector<std::uint64_t>& removals) {
  if (source_id == 0 || snapshot.source_id != source_id || !snapshot.delta ||
      snapshot.delta->base_revision != revision) {
    return false;
  }

  const auto& delta = snapshot.delta->draws;
  if (delta.upserts.size() != delta.upsert_indices.size()) {
    return false;
  }

  upserts.reserve(delta.upserts.size());
  removals.reserve(delta.removals.size());
  std::set<std::uint64_t> changed;
  for (std::size_t index = 0; index < delta.upserts.size(); ++index) {
    const auto draw = delta.upserts[index];
    const auto snapshot_index = delta.upsert_indices[index];
    if (draw == 0 || snapshot_index >= snapshot.draws.size()) {
      upserts.clear();
      removals.clear();
      return false;
    }
    const auto& record = snapshot.draws[snapshot_index];
    if (record.draw != draw || !changed.insert(draw).second) {
      upserts.clear();
      removals.clear();
      return false;
    }
    upserts.push_back({draw, snapshot_index, record.revision});
  }
  for (const auto draw : delta.removals) {
    if (draw == 0 || !changed.insert(draw).second) {
      upserts.clear();
      removals.clear();
      return false;
    }
    removals.push_back(draw);
  }
  return true;
}

}  // namespace

GpuSceneSlotError::GpuSceneSlotError(GpuSceneSlotErrorCode code,
                                     std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

GpuSceneSlotAllocator::GpuSceneSlotAllocator(std::string_view label,
                                             std::uint32_t capacity)
    : label_(label),
      owner_(g_next_gpu_scene_slot_owner.fetch_add(1,
                                                   std::memory_order_relaxed)),
      slots_(capacity) {
  if (owner_ == 0) {
    owner_ = g_next_gpu_scene_slot_owner.fetch_add(1,
                                                   std::memory_order_relaxed);
  }
  telemetry_.capacity = capacity;
  telemetry_.available_slots = capacity;
  for (std::uint32_t index = capacity; index > 0; --index) {
    free_slots_.push_back(index - 1);
  }
}

GpuSceneSlotHandle GpuSceneSlotAllocator::HandleFor(
    std::uint32_t index) const noexcept {
  return {index, slots_[index].generation, owner_};
}

[[noreturn]] void GpuSceneSlotAllocator::Throw(
    GpuSceneSlotErrorCode code, std::string_view detail) const {
  throw GpuSceneSlotError(code, label_ + ": " + std::string(detail));
}

void GpuSceneSlotAllocator::ValidateOwnedHandle(GpuSceneSlotHandle slot) {
  if (!slot) {
    Throw(GpuSceneSlotErrorCode::InvalidHandle, "invalid slot handle");
  }
  if (slot.owner != owner_) {
    Throw(GpuSceneSlotErrorCode::ForeignHandle,
          "slot handle belongs to another allocator");
  }
  if (slot.index >= slots_.size()) {
    Throw(GpuSceneSlotErrorCode::InvalidHandle,
          "slot index is outside table capacity");
  }
  if (slot.generation != slots_[slot.index].generation) {
    ++telemetry_.generation_mismatch_count;
    Throw(GpuSceneSlotErrorCode::StaleGeneration,
          "slot generation is stale");
  }
}

GpuSceneSlotHandle GpuSceneSlotAllocator::Allocate() {
  if (free_slots_.empty()) {
    ++telemetry_.exhaustion_count;
    Throw(GpuSceneSlotErrorCode::Exhausted,
          "exhausted (capacity=" + std::to_string(telemetry_.capacity) +
              ", active=" + std::to_string(telemetry_.active_slots) +
              ", retiring=" + std::to_string(telemetry_.retiring_slots) +
              ")");
  }

  const auto index = free_slots_.back();
  free_slots_.pop_back();
  auto& slot = slots_[index];
  slot.state = State::Active;
  --telemetry_.available_slots;
  ++telemetry_.active_slots;
  telemetry_.peak_active_slots =
      std::max(telemetry_.peak_active_slots, telemetry_.active_slots);
  ++telemetry_.allocation_count;
  if (slot.generation > 1) {
    ++telemetry_.reuse_count;
  }
  return HandleFor(index);
}

bool GpuSceneSlotAllocator::IsActive(GpuSceneSlotHandle slot) const noexcept {
  if (!slot || slot.owner != owner_ || slot.index >= slots_.size()) {
    return false;
  }
  const auto& state = slots_[slot.index];
  return state.generation == slot.generation && state.state == State::Active;
}

void GpuSceneSlotAllocator::RequireActive(GpuSceneSlotHandle slot) {
  ValidateOwnedHandle(slot);
  switch (slots_[slot.index].state) {
    case State::Active: return;
    case State::Free:
      Throw(GpuSceneSlotErrorCode::SlotNotAllocated,
            "slot is not allocated");
    case State::Retired:
      Throw(GpuSceneSlotErrorCode::SlotRetired,
            "slot is pending completion retirement");
  }
}

void GpuSceneSlotAllocator::RequireAvailable(
    std::size_t required, std::size_t immediately_reclaimable) {
  const auto completion_safe =
      static_cast<std::size_t>(telemetry_.available_slots) +
      immediately_reclaimable;
  if (required <= completion_safe) {
    return;
  }
  ++telemetry_.exhaustion_count;
  Throw(GpuSceneSlotErrorCode::Exhausted,
        "replacement requires " + std::to_string(required) +
            " slots, but only " + std::to_string(completion_safe) +
            " are completion-safe");
}

void GpuSceneSlotAllocator::Retire(GpuSceneSlotHandle slot,
                                   std::uint64_t last_completion_value) {
  ValidateOwnedHandle(slot);
  auto& state = slots_[slot.index];
  switch (state.state) {
    case State::Free:
      Throw(GpuSceneSlotErrorCode::SlotNotAllocated,
            "slot is not allocated");
    case State::Retired:
      Throw(GpuSceneSlotErrorCode::SlotRetired,
            "slot is already pending completion retirement");
    case State::Active: break;
  }

  state.state = State::Retired;
  retirements_.push_back({slot, last_completion_value});
  --telemetry_.active_slots;
  ++telemetry_.retiring_slots;
  ++telemetry_.retirement_count;
}

std::vector<GpuSceneSlotHandle> GpuSceneSlotAllocator::Collect(
    std::uint64_t completed_value) {
  std::vector<GpuSceneSlotHandle> collected;
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
    --telemetry_.retiring_slots;
    ++telemetry_.available_slots;
    ++telemetry_.retirement_collection_count;
    collected.push_back(old_handle);
    retirement = retirements_.erase(retirement);
  }
  return collected;
}

std::size_t GpuSceneSlotAllocator::CollectableCount(
    std::uint64_t completed_value) const noexcept {
  return static_cast<std::size_t>(std::count_if(
      retirements_.begin(), retirements_.end(),
      [completed_value](const Retirement& retirement) {
        return retirement.completion_value <= completed_value;
      }));
}

GpuSceneResourceSlots::GpuSceneResourceSlots(GpuSceneResourceTable table,
                                             std::uint32_t capacity)
    : table_(table),
      slots_("GPU Scene " + std::string(ResourceTableName(table)) + " table",
             capacity) {}

std::optional<GpuSceneSlotHandle> GpuSceneResourceSlots::Find(
    std::uint64_t resource) const noexcept {
  const auto found = resident_.find(resource);
  if (found == resident_.end()) {
    return std::nullopt;
  }
  return found->second.slot;
}

GpuSceneResourceUpdatePlan GpuSceneResourceSlots::Apply(
    const extraction::FrameSnapshot& snapshot,
    std::uint64_t last_completion_value, std::uint64_t completed_value) {
  const auto record_count = ResourceCount(snapshot, table_);
  if (record_count >
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    ThrowInvalidResourceSnapshot(table_,
                                 "table exceeds 32-bit GPU slot indexing");
  }

  GpuSceneResourceUpdatePlan plan;
  plan.table = table_;
  plan.source_id = snapshot.source_id;
  plan.base_revision = revision_;
  plan.revision = snapshot.revision;

  if (snapshot.source_id != 0 && source_id_ == snapshot.source_id &&
      snapshot.revision < revision_) {
    ThrowInvalidResourceSnapshot(table_,
                                 "snapshot revision precedes resident revision");
  }

  if (snapshot.source_id != 0 && source_id_ == snapshot.source_id &&
      revision_ == snapshot.revision) {
    plan.collected = slots_.Collect(completed_value);
    return plan;
  }

  std::vector<PendingResourceUpsert> delta_upserts;
  std::vector<std::uint64_t> delta_removals;
  const auto incremental = ReadUsableResourceDelta(
      snapshot, table_, source_id_, revision_, delta_upserts, delta_removals);
  plan.indexed_snapshot_records = delta_upserts.size();
  plan.full_reconciliation = !incremental;

  std::vector<GpuSceneResourceRetirement> retirements;
  std::vector<PendingResourceUpsert> upserts;
  const auto same_stable_source =
      snapshot.source_id != 0 && source_id_ == snapshot.source_id;

  if (incremental) {
    for (const auto resource : delta_removals) {
      const auto resident = resident_.find(resource);
      if (resident == resident_.end()) {
        plan.full_reconciliation = true;
        break;
      }
      retirements.push_back({resource, resident->second.slot});
    }
    if (!plan.full_reconciliation) {
      for (const auto& upsert : delta_upserts) {
        const auto resident = resident_.find(upsert.resource);
        if (resident != resident_.end() &&
            resident->second.record_version == upsert.record_version) {
          continue;
        }
        if (resident != resident_.end()) {
          retirements.push_back({upsert.resource, resident->second.slot});
        }
        upserts.push_back(upsert);
      }
    }
  }

  if (plan.full_reconciliation) {
    retirements.clear();
    upserts.clear();
    std::map<std::uint64_t, std::uint32_t> snapshot_indices;
    for (std::uint32_t index = 0; index < record_count; ++index) {
      const auto resource = ResourceIdentityAt(snapshot, table_, index);
      if (resource == 0) {
        ThrowInvalidResourceSnapshot(table_,
                                     "resource identity must be non-zero");
      }
      if (!snapshot_indices.emplace(resource, index).second) {
        ThrowInvalidResourceSnapshot(table_,
                                     "resource identities must be unique");
      }
    }
    plan.indexed_snapshot_records = record_count;
    for (const auto& [resource, resident] : resident_) {
      const auto snapshot_record = snapshot_indices.find(resource);
      const auto retain =
          same_stable_source && snapshot_record != snapshot_indices.end() &&
          ResourceVersionAt(snapshot, table_, snapshot_record->second) ==
              resident.record_version;
      if (!retain) {
        retirements.push_back({resource, resident.slot});
      }
    }
    for (const auto& [resource, snapshot_index] : snapshot_indices) {
      const auto resident = resident_.find(resource);
      const auto record_version =
          ResourceVersionAt(snapshot, table_, snapshot_index);
      const auto retain = same_stable_source && resident != resident_.end() &&
                          resident->second.record_version == record_version;
      if (!retain) {
        upserts.push_back({resource, snapshot_index, record_version});
      }
    }
  }

  const auto immediately_reusable =
      last_completion_value <= completed_value ? retirements.size() : 0U;
  slots_.RequireAvailable(upserts.size(),
                          slots_.CollectableCount(completed_value) +
                              immediately_reusable);
  plan.collected = slots_.Collect(completed_value);

  for (const auto& retirement : retirements) {
    slots_.Retire(retirement.slot, last_completion_value);
    resident_.erase(retirement.resource);
    plan.retirements.push_back(retirement);
  }
  if (last_completion_value <= completed_value && !retirements.empty()) {
    auto collected = slots_.Collect(completed_value);
    plan.collected.insert(plan.collected.end(), collected.begin(),
                          collected.end());
  }

  std::vector<std::uint32_t> dirty_slots;
  dirty_slots.reserve(upserts.size());
  for (const auto& upsert : upserts) {
    const auto slot = slots_.Allocate();
    resident_.emplace(
        upsert.resource, ResidentResource{slot, upsert.record_version});
    plan.upserts.push_back({upsert.resource, slot, upsert.snapshot_index,
                            upsert.record_version});
    dirty_slots.push_back(slot.index);
  }
  plan.dirty_ranges = BuildDirtyRanges(std::move(dirty_slots));

  source_id_ = snapshot.source_id;
  revision_ = snapshot.revision;
  return plan;
}

GpuSceneDrawSlots::GpuSceneDrawSlots(std::uint32_t capacity)
    : slots_("GPU Scene draw table", capacity) {}

std::optional<GpuSceneSlotHandle> GpuSceneDrawSlots::Find(
    std::uint64_t draw) const noexcept {
  const auto found = resident_.find(draw);
  if (found == resident_.end()) {
    return std::nullopt;
  }
  return found->second.slot;
}

GpuSceneDrawUpdatePlan GpuSceneDrawSlots::Apply(
    const extraction::FrameSnapshot& snapshot,
    std::uint64_t last_completion_value, std::uint64_t completed_value) {
  if (snapshot.draws.size() >
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    ThrowInvalidSnapshot("draw table exceeds 32-bit GPU slot indexing");
  }

  GpuSceneDrawUpdatePlan plan;
  plan.source_id = snapshot.source_id;
  plan.base_revision = revision_;
  plan.revision = snapshot.revision;

  if (snapshot.source_id != 0 && source_id_ == snapshot.source_id &&
      snapshot.revision < revision_) {
    ThrowInvalidSnapshot("snapshot revision precedes resident revision");
  }

  if (snapshot.source_id != 0 && source_id_ == snapshot.source_id &&
      revision_ == snapshot.revision) {
    plan.collected = slots_.Collect(completed_value);
    return plan;
  }

  std::vector<PendingUpsert> delta_upserts;
  std::vector<std::uint64_t> delta_removals;
  const auto incremental =
      ReadUsableDelta(snapshot, source_id_, revision_, delta_upserts,
                      delta_removals);
  plan.indexed_snapshot_draws = delta_upserts.size();
  plan.full_reconciliation = !incremental;

  std::vector<GpuSceneDrawRetirement> retirements;
  std::vector<PendingUpsert> upserts;
  const auto same_stable_source =
      snapshot.source_id != 0 && source_id_ == snapshot.source_id;

  if (incremental) {
    for (const auto draw : delta_removals) {
      const auto resident = resident_.find(draw);
      if (resident == resident_.end()) {
        plan.full_reconciliation = true;
        break;
      }
      retirements.push_back({draw, resident->second.slot});
    }
    if (!plan.full_reconciliation) {
      for (const auto& upsert : delta_upserts) {
        const auto resident = resident_.find(upsert.draw);
        if (resident != resident_.end() &&
            resident->second.record_revision == upsert.record_revision) {
          continue;
        }
        if (resident != resident_.end()) {
          retirements.push_back({upsert.draw, resident->second.slot});
        }
        upserts.push_back(upsert);
      }
    }
  }

  if (plan.full_reconciliation) {
    retirements.clear();
    upserts.clear();
    std::map<std::uint64_t, std::uint32_t> snapshot_indices;
    for (std::uint32_t index = 0; index < snapshot.draws.size(); ++index) {
      const auto draw = snapshot.draws[index].draw;
      if (draw == 0) {
        ThrowInvalidSnapshot("draw identity must be non-zero");
      }
      if (!snapshot_indices.emplace(draw, index).second) {
        ThrowInvalidSnapshot("draw identities must be unique");
      }
    }
    plan.indexed_snapshot_draws = snapshot.draws.size();
    for (const auto& [draw, resident] : resident_) {
      const auto snapshot_record = snapshot_indices.find(draw);
      const auto retain =
          same_stable_source && snapshot_record != snapshot_indices.end() &&
          snapshot.draws[snapshot_record->second].revision ==
              resident.record_revision;
      if (!retain) {
        retirements.push_back({draw, resident.slot});
      }
    }
    for (const auto& [draw, snapshot_index] : snapshot_indices) {
      const auto resident = resident_.find(draw);
      const auto& record = snapshot.draws[snapshot_index];
      const auto retain = same_stable_source && resident != resident_.end() &&
                          resident->second.record_revision == record.revision;
      if (!retain) {
        upserts.push_back({draw, snapshot_index, record.revision});
      }
    }
  }

  // Check capacity before changing residency. Slots retired by this update are
  // reusable immediately only when their last referencing submission has
  // already completed.
  const auto immediately_reusable =
      last_completion_value <= completed_value ? retirements.size() : 0U;
  slots_.RequireAvailable(upserts.size(),
                          slots_.CollectableCount(completed_value) +
                              immediately_reusable);
  plan.collected = slots_.Collect(completed_value);

  for (const auto& retirement : retirements) {
    slots_.Retire(retirement.slot, last_completion_value);
    resident_.erase(retirement.draw);
    plan.retirements.push_back(retirement);
  }
  if (last_completion_value <= completed_value && !retirements.empty()) {
    auto collected = slots_.Collect(completed_value);
    plan.collected.insert(plan.collected.end(), collected.begin(),
                          collected.end());
  }

  for (const auto& upsert : upserts) {
    const auto slot = slots_.Allocate();
    resident_.emplace(
        upsert.draw, ResidentDraw{slot, upsert.record_revision});
    plan.upserts.push_back({upsert.draw, slot, upsert.snapshot_index,
                            upsert.record_revision});
  }
  std::vector<std::uint32_t> dirty_slots;
  dirty_slots.reserve(plan.upserts.size());
  for (const auto& upsert : plan.upserts) {
    dirty_slots.push_back(upsert.slot.index);
  }
  plan.dirty_ranges = BuildDirtyRanges(std::move(dirty_slots));

  source_id_ = snapshot.source_id;
  revision_ = snapshot.revision;
  return plan;
}

}  // namespace merlin::render
