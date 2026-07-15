#include <merlin/vulkan/bindless_resource_table.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using merlin::vulkan::BindlessFallbackTexture;
using merlin::vulkan::BindlessSamplerDescriptor;
using merlin::vulkan::BindlessSamplerTable;
using merlin::vulkan::BindlessSlotError;
using merlin::vulkan::BindlessSlotErrorCode;
using merlin::vulkan::BindlessSlotHandle;
using merlin::vulkan::BindlessTextureTable;
using merlin::AddressMode;
using merlin::FilterMode;
using merlin::SamplerDescriptor;

template <typename Callback>
void ExpectError(Callback&& callback, BindlessSlotErrorCode code,
                 std::string_view message_fragment) {
  try {
    callback();
    assert(false && "expected BindlessSlotError");
  } catch (const BindlessSlotError& error) {
    assert(error.code() == code);
    assert(std::string_view(error.what()).find(message_fragment) !=
           std::string_view::npos);
  }
}

void TestTextureSlots() {
  BindlessTextureTable textures(6);
  const std::array fallbacks{
      BindlessFallbackTexture::White,
      BindlessFallbackTexture::Black,
      BindlessFallbackTexture::FlatNormal,
      BindlessFallbackTexture::Error,
  };
  for (std::uint32_t index = 0; index < fallbacks.size(); ++index) {
    const auto slot = textures.fallback(fallbacks[index]);
    assert(slot.index == index);
    assert(slot.generation == 1);
    assert(textures.IsActive(slot));
  }
  auto telemetry = textures.telemetry();
  assert(telemetry.schema_version == 1);
  assert(telemetry.capacity == 6);
  assert(telemetry.reserved_slots == 4);
  assert(telemetry.current_use == 4);
  assert(telemetry.peak_use == 4);
  assert(telemetry.available_slots == 2);
  assert(telemetry.dirty_descriptor_slots == 4);
  assert(textures.ConsumeDirtySlots() ==
         (std::vector<std::uint32_t>{0, 1, 2, 3}));
  assert(textures.telemetry().descriptor_update_count == 4);

  const auto first = textures.Allocate();
  const auto second = textures.Allocate();
  assert(first.index == 4);
  assert(second.index == 5);
  assert(first.table_id == second.table_id);
  telemetry = textures.telemetry();
  assert(telemetry.current_use == 6);
  assert(telemetry.peak_use == 6);
  assert(telemetry.available_slots == 0);
  assert(telemetry.allocation_count == 2);
  ExpectError([&] { (void)textures.Allocate(); },
              BindlessSlotErrorCode::Exhausted, "capacity=6");
  assert(textures.telemetry().exhaustion_count == 1);

  ExpectError(
      [&] { textures.Retire(textures.fallback(BindlessFallbackTexture::Error),
                            1); },
      BindlessSlotErrorCode::ReservedSlot, "reserved slot");
  textures.Retire(first, 7);
  assert(!textures.IsActive(first));
  assert(textures.telemetry().retiring_slots == 1);
  assert(textures.Collect(6).empty());
  ExpectError([&] { textures.Retire(first, 8); },
              BindlessSlotErrorCode::SlotRetired, "pending");
  ExpectError([&] { (void)textures.Allocate(); },
              BindlessSlotErrorCode::Exhausted, "retiring=1");

  const auto collected = textures.Collect(7);
  assert(collected == std::vector<BindlessSlotHandle>{first});
  const auto replacement = textures.Allocate();
  assert(replacement.index == first.index);
  assert(replacement.generation == first.generation + 1);
  assert(textures.IsActive(replacement));
  ExpectError([&] { textures.Retire(first, 9); },
              BindlessSlotErrorCode::StaleGeneration, "generation is stale");
  telemetry = textures.telemetry();
  assert(telemetry.reuse_count == 1);
  assert(telemetry.retirement_collection_count == 1);
  assert(telemetry.generation_mismatch_count == 1);
  // Collection and immediate reuse coalesce into one descriptor write.
  assert(textures.ConsumeDirtySlots() ==
         (std::vector<std::uint32_t>{4, 5}));

  BindlessTextureTable other(4);
  ExpectError([&] { other.Retire(replacement, 10); },
              BindlessSlotErrorCode::ForeignHandle, "another table");
}

void TestSamplerDeduplicationAndRetirement() {
  BindlessSamplerTable samplers(2);
  SamplerDescriptor named_linear_repeat;
  named_linear_repeat.label = "first debug label";
  const BindlessSamplerDescriptor linear_repeat(named_linear_repeat);
  named_linear_repeat.label = "same descriptor, different debug label";
  const BindlessSamplerDescriptor relabeled_linear_repeat(
      named_linear_repeat);
  assert(linear_repeat == relabeled_linear_repeat);
  BindlessSamplerDescriptor nearest_repeat;
  nearest_repeat.min_filter = FilterMode::Nearest;
  nearest_repeat.mag_filter = FilterMode::Nearest;
  BindlessSamplerDescriptor linear_clamp;
  linear_clamp.address_u = AddressMode::ClampToEdge;
  linear_clamp.address_v = AddressMode::ClampToEdge;

  const auto shared_a = samplers.Acquire(linear_repeat);
  const auto shared_b = samplers.Acquire(relabeled_linear_repeat);
  assert(shared_a == shared_b);
  const auto distinct = samplers.Acquire(nearest_repeat);
  assert(distinct.index != shared_a.index);
  auto telemetry = samplers.telemetry();
  assert(telemetry.unique_sampler_count == 2);
  assert(telemetry.current_reference_count == 3);
  assert(telemetry.peak_reference_count == 3);
  assert(telemetry.deduplication_hit_count == 1);
  assert(telemetry.slots.current_use == 2);
  assert(samplers.ConsumeDirtySlots() ==
         (std::vector<std::uint32_t>{0, 1}));
  ExpectError([&] { (void)samplers.Acquire(linear_clamp); },
              BindlessSlotErrorCode::Exhausted, "bindless sampler");

  // The first release retains the shared slot and contributes its completion
  // value to the final retirement fence.
  samplers.Release(shared_a, 10);
  assert(samplers.IsActive(shared_b));
  assert(samplers.telemetry().current_reference_count == 2);
  samplers.Release(shared_b, 5);
  assert(!samplers.IsActive(shared_a));
  assert(samplers.telemetry().unique_sampler_count == 1);
  assert(samplers.Collect(9).empty());
  ExpectError([&] { (void)samplers.Acquire(linear_clamp); },
              BindlessSlotErrorCode::Exhausted, "retiring=1");

  assert(samplers.Collect(10) ==
         std::vector<BindlessSlotHandle>{shared_a});
  const auto replacement = samplers.Acquire(linear_clamp);
  assert(replacement.index == shared_a.index);
  assert(replacement.generation == shared_a.generation + 1);
  ExpectError([&] { samplers.Release(shared_a, 11); },
              BindlessSlotErrorCode::StaleGeneration, "stale");

  telemetry = samplers.telemetry();
  assert(telemetry.unique_sampler_count == 2);
  assert(telemetry.current_reference_count == 2);
  assert(telemetry.slots.reuse_count == 1);
  assert(telemetry.slots.retirement_count == 1);
  assert(telemetry.slots.retirement_collection_count == 1);
  assert(telemetry.slots.generation_mismatch_count == 1);
  assert(samplers.ConsumeDirtySlots() ==
         std::vector<std::uint32_t>{replacement.index});

  samplers.Release(distinct, 12);
  samplers.Release(replacement, 12);
  assert(samplers.Collect(12).size() == 2);
  assert(samplers.telemetry().current_reference_count == 0);
  assert(samplers.telemetry().unique_sampler_count == 0);
}

}  // namespace

int main() {
  TestTextureSlots();
  TestSamplerDeduplicationAndRetirement();
  std::cout << "bindless resource table tests passed\n";
  return 0;
}
