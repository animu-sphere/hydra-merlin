#include "renderer_impl.hpp"

namespace merlin::vulkan {

Renderer::Impl::ResourceSyncMode Renderer::Impl::SelectResourceSyncMode(
    const extraction::FrameSnapshot& snapshot) const noexcept {
  if (resource_residency_dirty_ ||
      snapshot.source_id != resident_snapshot_source_) {
    return ResourceSyncMode::ReplaceAll;
  }
  if (snapshot.source_id == 0) {
    return ResourceSyncMode::Full;
  }
  if (snapshot.revision == resident_snapshot_revision_) {
    return ResourceSyncMode::Unchanged;
  }
  if (snapshot.delta &&
      snapshot.delta->base_revision == resident_snapshot_revision_) {
    return ResourceSyncMode::Incremental;
  }
  return ResourceSyncMode::Full;
}

void Renderer::Impl::CommitResourceSnapshot(
    const extraction::FrameSnapshot& snapshot) noexcept {
  resident_snapshot_source_ = snapshot.source_id;
  resident_snapshot_revision_ =
      snapshot.source_id == 0 ? 0 : snapshot.revision;
}

Buffer Renderer::Impl::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties,
    VkMemoryPropertyFlags preferred,
    std::uint32_t first_queue_family,
    std::uint32_t second_queue_family) {
  ++frame_counters_.allocation_count;
  ++frame_counters_.buffer_allocation_count;
  frame_counters_.buffer_allocation_bytes += size;
  return CreateBufferRaw(device_, physical_device_, size, usage, properties,
      &memory_budget_, first_queue_family,
      second_queue_family, preferred);
}

void Renderer::Impl::DestroyBuffer(Buffer& buffer) noexcept {
  DestroyBufferRaw(device_, buffer, &memory_budget_);
}

void Renderer::Impl::DestroyTexture(TextureSlot& texture) noexcept {
  if (texture.view != VK_NULL_HANDLE) {
    vkDestroyImageView(device_, texture.view, nullptr);
  }
  if (texture.image != VK_NULL_HANDLE) {
    vkDestroyImage(device_, texture.image, nullptr);
  }
  if (texture.memory != VK_NULL_HANDLE) {
    memory_budget_.Free(texture.memory);
  }
  texture = {};
}

void Renderer::Impl::ResetAbandonedUploads() {
  pending_copies_.clear();
  pending_image_copies_.clear();
  pending_graphics_acquire_images_.clear();
  staging_.AbandonFrame();
  gaussian_staging_.AbandonFrame();
  gpu_scene_staging_.AbandonFrame();
  gpu_driven_staging_.AbandonFrame();
  gpu_scene_buffers_.pending_draw_slot_indices.reset();
  gpu_scene_buffers_.pending_update = false;
  for (const auto handle : pending_texture_handles_) {
    const auto texture = texture_slots_.find(handle);
    if (texture != texture_slots_.end() &&
        texture->second.pending_upload) {
      if (texture->second.bindless_slot) {
        RetireTexture(texture->second);
      } else {
        DestroyTexture(texture->second);
      }
    }
  }
  pending_texture_handles_.clear();
  for (auto& texture : reserved_bindless_textures_) {
    if (texture.pending_upload) {
      DestroyTexture(texture);
      force_reserved_bindless_texture_writes_ = true;
    }
  }
  if (fallback_texture_.pending_upload) {
    DestroyTexture(fallback_texture_);
  }
  for (auto& buffer : frame_upload_buffers_) {
    DestroyBuffer(buffer);
  }
  frame_upload_buffers_.clear();
  if (resource_residency_dirty_) {
    // A failed submission produces no completion token, so Resolve cannot
    // collect resources retired while preparing that submission. Reclaim
    // only generations already known to be complete before retrying.
    // Bindless dirty slots remain queued until PrepareBindlessDescriptors has
    // recreated all required fallback images.
    CollectDeferred(latest_completed_value_, false);
  }
}

Renderer::Impl::TextureSlot Renderer::Impl::CreateTextureResource(
    std::uint32_t width, std::uint32_t height, std::uint64_t revision,
    const std::vector<std::uint8_t>& pixels, bool count_upload) {
  TextureSlot slot;
  slot.revision = revision;
  Buffer staging;
  try {
    slot.image = CreateImage(
        width, height, kColorFormat,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        slot.memory);
    slot.view =
        CreateImageView(slot.image, kColorFormat, VK_IMAGE_ASPECT_COLOR_BIT);
    slot.pending_upload = true;
    staging = CreateBuffer(
        static_cast<VkDeviceSize>(pixels.size()),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* mapped{};
    Check(vkMapMemory(device_, staging.memory, 0, staging.size, 0, &mapped),
        "map texture staging buffer");
    std::memcpy(mapped, pixels.data(), pixels.size());
    vkUnmapMemory(device_, staging.memory);
    pending_image_copies_.push_back(
        {staging.handle, slot.image, width, height});
    frame_upload_buffers_.push_back(staging);
    staging = {};
    if (count_upload) {
      frame_counters_.upload_bytes += pixels.size();
      frame_counters_.texture_upload_bytes += pixels.size();
    }
  } catch (...) {
    DestroyBuffer(staging);
    DestroyTexture(slot);
    throw;
  }
  return slot;
}

void Renderer::Impl::CommitTextureUploads() noexcept {
  for (const auto handle : pending_texture_handles_) {
    const auto texture = texture_slots_.find(handle);
    if (texture != texture_slots_.end()) {
      texture->second.pending_upload = false;
    }
  }
  pending_texture_handles_.clear();
  for (auto& texture : reserved_bindless_textures_) {
    texture.pending_upload = false;
  }
  fallback_texture_.pending_upload = false;
}

VkSampler Renderer::Impl::CreateSamplerResource(FilterMode min_filter, FilterMode mag_filter,
    AddressMode address_u,
    AddressMode address_v) {
  const auto filter = [](FilterMode value) {
    return value == FilterMode::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  };
  const auto address = [](AddressMode value) {
    switch (value) {
    case AddressMode::Repeat:
      return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case AddressMode::MirroredRepeat:
      return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case AddressMode::ClampToEdge:
      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
  };
  VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  info.minFilter = filter(min_filter);
  info.magFilter = filter(mag_filter);
  info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  info.addressModeU = address(address_u);
  info.addressModeV = address(address_v);
  info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  info.minLod = 0.0F;
  info.maxLod = 0.0F;
  VkSampler sampler{};
  Check(vkCreateSampler(device_, &info, nullptr, &sampler), "create sampler");
  return sampler;
}

void Renderer::Impl::CollectCompletedBindlessSlots(std::uint64_t completed) {
  if (!bindless_texture_table_) {
    return;
  }
  for (const auto slot : bindless_texture_table_->Collect(completed)) {
    bindless_texture_views_[slot.index] = VK_NULL_HANDLE;
  }
  for (const auto slot : bindless_sampler_table_->Collect(completed)) {
    bindless_samplers_[slot.index] = VK_NULL_HANDLE;
  }
}

void Renderer::Impl::RetireTexture(TextureSlot& texture) {
  if (texture.image != VK_NULL_HANDLE) {
    if (texture.bindless_slot) {
      bindless_texture_table_->Retire(texture.bindless_slot,
          timeline_value_);
    }
    retired_textures_.push_back({texture, timeline_value_});
    texture = {};
    CollectCompletedBindlessSlots(latest_completed_value_);
  }
}

void Renderer::Impl::RetireSampler(SamplerSlot& sampler) {
  if (sampler.sampler != VK_NULL_HANDLE) {
    bool bindless_slot_retired{};
    if (sampler.bindless_slot) {
      const auto slot = sampler.bindless_slot;
      bindless_sampler_table_->Release(slot, timeline_value_);
      if (!bindless_sampler_table_->IsActive(slot)) {
        retired_samplers_.push_back(
            {sampler.sampler, timeline_value_, slot});
        bindless_slot_retired = true;
      }
    } else {
      retired_samplers_.push_back(
          {sampler.sampler, timeline_value_, {}});
    }
    sampler = {};
    if (bindless_slot_retired) {
      CollectCompletedBindlessSlots(latest_completed_value_);
    }
  }
}

void Renderer::Impl::SyncTextures(const extraction::FrameSnapshot& snapshot,
    ResourceSyncMode mode) {
  if (mode == ResourceSyncMode::Unchanged) {
    frame_counters_.texture_cache_hits += snapshot.textures.size();
    return;
  }

  std::vector<const extraction::TextureRecord*> records;
  if (mode == ResourceSyncMode::ReplaceAll) {
    for (auto& [handle, slot] : texture_slots_) {
      (void)handle;
      RetireTexture(slot);
      ++frame_counters_.texture_reconcile_count;
    }
    texture_slots_.clear();
    records.reserve(snapshot.textures.size());
    for (const auto& texture : snapshot.textures) {
      records.push_back(&texture);
    }
  } else if (mode == ResourceSyncMode::Full) {
    std::set<std::uint64_t> snapshot_handles;
    for (const auto& texture : snapshot.textures) {
      snapshot_handles.insert(texture.texture);
    }
    for (auto slot = texture_slots_.begin(); slot != texture_slots_.end();) {
      if (!snapshot_handles.contains(slot->first)) {
        RetireTexture(slot->second);
        slot = texture_slots_.erase(slot);
        ++frame_counters_.texture_reconcile_count;
      } else {
        ++slot;
      }
    }
    records.reserve(snapshot.textures.size());
    for (const auto& texture : snapshot.textures) {
      records.push_back(&texture);
    }
  } else {
    const auto& delta = snapshot.delta->textures;
    for (const auto handle : delta.removals) {
      ++frame_counters_.texture_reconcile_count;
      const auto slot = texture_slots_.find(handle);
      if (slot != texture_slots_.end()) {
        RetireTexture(slot->second);
        texture_slots_.erase(slot);
      }
    }
    records.reserve(delta.upserts.size());
    for (std::size_t i = 0; i < delta.upserts.size(); ++i) {
      const auto* record = FindDeltaRecord(
          snapshot.textures, delta, i,
          [](const extraction::TextureRecord& texture) {
            return texture.texture;
          });
      if (!record) {
        throw RendererError(RendererErrorCode::InvalidRequest,
            "synchronize textures",
            "snapshot texture delta has no matching record");
      }
      records.push_back(record);
    }
    frame_counters_.texture_cache_hits +=
        snapshot.textures.size() - records.size();
  }

  frame_counters_.texture_reconcile_count += records.size();
  for (const auto* texture_record : records) {
    const auto& texture = *texture_record;
    if (!texture.pixels) {
      throw RendererError(RendererErrorCode::InvalidRequest,
          "synchronize textures",
          "texture payload is null");
    }
    const auto [entry, inserted] =
        texture_slots_.try_emplace(texture.texture);
    if (!inserted && entry->second.revision == texture.revision) {
      ++frame_counters_.texture_cache_hits;
      continue;
    }
    ++frame_counters_.texture_cache_misses;
    RetireTexture(entry->second);
    auto replacement = CreateTextureResource(
        texture.width, texture.height, texture.revision, *texture.pixels);
    try {
      if (bindless_texture_table_) {
        replacement.bindless_slot = bindless_texture_table_->Allocate();
        bindless_texture_views_[replacement.bindless_slot.index] =
            replacement.view;
      }
    } catch (...) {
      DestroyTexture(replacement);
      throw;
    }
    entry->second = replacement;
    pending_texture_handles_.push_back(texture.texture);
  }
}

void Renderer::Impl::SyncSamplers(const extraction::FrameSnapshot& snapshot,
    ResourceSyncMode mode) {
  if (mode == ResourceSyncMode::Unchanged) {
    frame_counters_.sampler_cache_hits += snapshot.samplers.size();
    return;
  }

  std::vector<const extraction::SamplerRecord*> records;
  if (mode == ResourceSyncMode::ReplaceAll) {
    for (auto& [handle, slot] : sampler_slots_) {
      (void)handle;
      RetireSampler(slot);
      ++frame_counters_.sampler_reconcile_count;
    }
    sampler_slots_.clear();
    records.reserve(snapshot.samplers.size());
    for (const auto& sampler : snapshot.samplers) {
      records.push_back(&sampler);
    }
  } else if (mode == ResourceSyncMode::Full) {
    std::set<std::uint64_t> snapshot_handles;
    for (const auto& sampler : snapshot.samplers) {
      snapshot_handles.insert(sampler.sampler);
    }
    for (auto slot = sampler_slots_.begin(); slot != sampler_slots_.end();) {
      if (!snapshot_handles.contains(slot->first)) {
        RetireSampler(slot->second);
        slot = sampler_slots_.erase(slot);
        ++frame_counters_.sampler_reconcile_count;
      } else {
        ++slot;
      }
    }
    records.reserve(snapshot.samplers.size());
    for (const auto& sampler : snapshot.samplers) {
      records.push_back(&sampler);
    }
  } else {
    const auto& delta = snapshot.delta->samplers;
    for (const auto handle : delta.removals) {
      ++frame_counters_.sampler_reconcile_count;
      const auto slot = sampler_slots_.find(handle);
      if (slot != sampler_slots_.end()) {
        RetireSampler(slot->second);
        sampler_slots_.erase(slot);
      }
    }
    records.reserve(delta.upserts.size());
    for (std::size_t i = 0; i < delta.upserts.size(); ++i) {
      const auto* record = FindDeltaRecord(
          snapshot.samplers, delta, i,
          [](const extraction::SamplerRecord& sampler) {
            return sampler.sampler;
          });
      if (!record) {
        throw RendererError(RendererErrorCode::InvalidRequest,
            "synchronize samplers",
            "snapshot sampler delta has no matching record");
      }
      records.push_back(record);
    }
    frame_counters_.sampler_cache_hits +=
        snapshot.samplers.size() - records.size();
  }

  frame_counters_.sampler_reconcile_count += records.size();
  for (const auto* sampler_record : records) {
    const auto& sampler = *sampler_record;
    const auto [entry, inserted] = sampler_slots_.try_emplace(sampler.sampler);
    if (!inserted && entry->second.revision == sampler.revision) {
      ++frame_counters_.sampler_cache_hits;
      continue;
    }
    ++frame_counters_.sampler_cache_misses;
    RetireSampler(entry->second);
    SamplerSlot replacement;
    replacement.revision = sampler.revision;
    if (bindless_sampler_table_) {
      BindlessSamplerDescriptor descriptor;
      descriptor.min_filter = sampler.min_filter;
      descriptor.mag_filter = sampler.mag_filter;
      descriptor.address_u = sampler.address_u;
      descriptor.address_v = sampler.address_v;
      const auto slot = bindless_sampler_table_->Acquire(descriptor);
      replacement.bindless_slot = slot;
      if (bindless_samplers_[slot.index] == VK_NULL_HANDLE) {
        try {
          bindless_samplers_[slot.index] = CreateSamplerResource(
              sampler.min_filter, sampler.mag_filter, sampler.address_u,
              sampler.address_v);
        } catch (...) {
          bindless_sampler_table_->Release(slot, latest_completed_value_);
          (void)bindless_sampler_table_->Collect(latest_completed_value_);
          throw;
        }
      }
      replacement.sampler = bindless_samplers_[slot.index];
    } else {
      replacement.sampler = CreateSamplerResource(
          sampler.min_filter, sampler.mag_filter, sampler.address_u,
          sampler.address_v);
    }
    entry->second = replacement;
  }
}

void Renderer::Impl::EnsureFallbackTextureAndSampler() {
  if (bindless_texture_table_) {
    EnsureReservedBindlessTextures();
  } else if (fallback_texture_.image == VK_NULL_HANDLE) {
    const std::vector<std::uint8_t> white{255U, 255U, 255U, 255U};
    fallback_texture_ = CreateTextureResource(1, 1, 1, white, false);
  }
  if (fallback_sampler_.sampler == VK_NULL_HANDLE) {
    fallback_sampler_.revision = 1;
    fallback_sampler_.sampler = CreateSamplerResource(
        FilterMode::Linear, FilterMode::Linear, AddressMode::Repeat,
        AddressMode::Repeat);
  }
}

void Renderer::Impl::EnsureReservedBindlessTextures() {
  if (!bindless_texture_table_) {
    return;
  }
  static const std::array<std::array<std::uint8_t, 4>,
      kReservedBindlessTextureSlots>
      pixels{{
          {255U, 255U, 255U, 255U},
          {0U, 0U, 0U, 255U},
          {128U, 128U, 255U, 255U},
          {255U, 0U, 255U, 255U},
      }};
  for (std::uint32_t index = 0; index < pixels.size(); ++index) {
    auto& texture = reserved_bindless_textures_[index];
    if (texture.image == VK_NULL_HANDLE) {
      texture = CreateTextureResource(
          1, 1, 1,
          std::vector<std::uint8_t>(pixels[index].begin(),
              pixels[index].end()),
          false);
      bindless_texture_views_[index] = texture.view;
      force_reserved_bindless_texture_writes_ = true;
    }
  }
}

VkImageView Renderer::Impl::FallbackTextureView() const noexcept {
  if (bindless_texture_table_) {
    return reserved_bindless_textures_[static_cast<std::uint32_t>(
                                           BindlessFallbackTexture::White)]
        .view;
  }
  return fallback_texture_.view;
}

void Renderer::Impl::EnsureBindlessDescriptorSet() {
  if (!bindless_texture_table_ ||
      bindless_descriptor_set_ != VK_NULL_HANDLE) {
    return;
  }
  const auto& selection = capabilities_.descriptor_indexing_selection;
  std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
  bindings[0].binding = shader_abi::kBindlessSamplers.binding;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  bindings[0].descriptorCount = selection.sampler_capacity;
  bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  bindings[1].binding = shader_abi::kBindlessTextures.binding;
  bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  bindings[1].descriptorCount = selection.texture_capacity;
  bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  constexpr auto common_flags =
      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
  const std::array<VkDescriptorBindingFlags, 2> binding_flags{
      common_flags,
      common_flags | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT};
  VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
  flags_info.bindingCount = static_cast<std::uint32_t>(binding_flags.size());
  flags_info.pBindingFlags = binding_flags.data();
  VkDescriptorSetLayoutCreateInfo layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layout_info.pNext = &flags_info;
  layout_info.flags =
      VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
  layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
  layout_info.pBindings = bindings.data();
  Check(vkCreateDescriptorSetLayout(device_, &layout_info, nullptr,
            &bindless_descriptor_set_layout_),
      "create bindless resource descriptor layout");
  ++frame_counters_.descriptor_layout_cache_misses;

  const std::array<VkDescriptorPoolSize, 2> sizes{{
      {VK_DESCRIPTOR_TYPE_SAMPLER, selection.sampler_capacity},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, selection.texture_capacity},
  }};
  VkDescriptorPoolCreateInfo pool_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
  pool_info.pPoolSizes = sizes.data();
  Check(vkCreateDescriptorPool(device_, &pool_info, nullptr,
            &bindless_descriptor_pool_),
      "create bindless resource descriptor pool");
  ++frame_counters_.descriptor_pool_creation_count;

  VkDescriptorSetVariableDescriptorCountAllocateInfo variable_count{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
  variable_count.descriptorSetCount = 1;
  variable_count.pDescriptorCounts = &selection.texture_capacity;
  VkDescriptorSetAllocateInfo allocate{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.pNext = &variable_count;
  allocate.descriptorPool = bindless_descriptor_pool_;
  allocate.descriptorSetCount = 1;
  allocate.pSetLayouts = &bindless_descriptor_set_layout_;
  Check(vkAllocateDescriptorSets(device_, &allocate,
            &bindless_descriptor_set_),
      "allocate bindless resource descriptor set");
  ++frame_counters_.descriptor_allocation_count;
}

void Renderer::Impl::WriteBindlessDescriptors(
    const std::vector<std::uint32_t>& texture_indices,
    const std::vector<std::uint32_t>& sampler_indices) {
  std::vector<VkDescriptorImageInfo> texture_infos(texture_indices.size());
  std::vector<VkDescriptorImageInfo> sampler_infos(sampler_indices.size());
  std::vector<VkWriteDescriptorSet> writes;
  writes.reserve(texture_indices.size() + sampler_indices.size());
  const auto error_view =
      reserved_bindless_textures_[static_cast<std::uint32_t>(
                                      BindlessFallbackTexture::Error)]
          .view;
  for (std::size_t i = 0; i < texture_indices.size(); ++i) {
    const auto index = texture_indices[i];
    const auto view = bindless_texture_views_[index] == VK_NULL_HANDLE
                          ? error_view
                          : bindless_texture_views_[index];
    texture_infos[i] =
        {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = bindless_descriptor_set_;
    write.dstBinding = shader_abi::kBindlessTextures.binding;
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &texture_infos[i];
    writes.push_back(write);
  }
  for (std::size_t i = 0; i < sampler_indices.size(); ++i) {
    const auto index = sampler_indices[i];
    const auto sampler = bindless_samplers_[index] == VK_NULL_HANDLE
                             ? fallback_sampler_.sampler
                             : bindless_samplers_[index];
    sampler_infos[i] = {sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = bindless_descriptor_set_;
    write.dstBinding = shader_abi::kBindlessSamplers.binding;
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = &sampler_infos[i];
    writes.push_back(write);
  }
  if (!writes.empty()) {
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
        writes.data(), 0, nullptr);
    frame_counters_.descriptor_update_count += writes.size();
    frame_counters_.bindless_sampled_image_descriptor_update_count +=
        texture_indices.size();
    frame_counters_.bindless_sampler_descriptor_update_count +=
        sampler_indices.size();
  }
}

void Renderer::Impl::PrepareBindlessDescriptors() {
  if (!bindless_texture_table_) {
    return;
  }
  EnsureFallbackTextureAndSampler();
  EnsureBindlessDescriptorSet();

  auto texture_indices = bindless_texture_table_->ConsumeDirtySlots();
  if (force_reserved_bindless_texture_writes_) {
    for (std::uint32_t index = 0;
        index < kReservedBindlessTextureSlots; ++index) {
      texture_indices.push_back(index);
    }
    std::sort(texture_indices.begin(), texture_indices.end());
    texture_indices.erase(
        std::unique(texture_indices.begin(), texture_indices.end()),
        texture_indices.end());
  }
  const auto sampler_indices =
      bindless_sampler_table_->ConsumeDirtySlots();
  WriteBindlessDescriptors(texture_indices, sampler_indices);
  force_reserved_bindless_texture_writes_ = false;
}

// Reconciles GPU geometry residency with one immutable snapshot. Continuous
// SceneExtractor revisions select only delta records; unrelated scene edits
// and frames do not walk the full geometry table. Revision gaps,
// foreign sources, and manually constructed snapshots use the full path.
void Renderer::Impl::SyncGeometry(const extraction::FrameSnapshot& snapshot,
    ResourceSyncMode mode) {
  if (mode == ResourceSyncMode::Unchanged) {
    frame_counters_.geometry_cache_hits += snapshot.geometries.size();
    ++frame_counters_.scene_cache_hits;
    return;
  }

  // Invalidate batch placement before any operation that can replace arena
  // ranges, including failed residency updates and full reconciliation.
  if (mode != ResourceSyncMode::Incremental ||
      !snapshot.delta->geometries.upserts.empty() ||
      !snapshot.delta->geometries.removals.empty()) {
    ++geometry_residency_generation_;
  }

  bool structural_change = false;
  std::vector<const extraction::GeometryRecord*> records;
  if (mode == ResourceSyncMode::ReplaceAll) {
    for (auto& [handle, slot] : geometry_slots_) {
      (void)handle;
      ReleaseRange(vertex_arena_, slot.vertices);
      ReleaseRange(index_arena_, slot.indices);
      ++frame_counters_.geometry_reconcile_count;
    }
    structural_change = !geometry_slots_.empty();
    geometry_slots_.clear();
    records.reserve(snapshot.geometries.size());
    for (const auto& geometry : snapshot.geometries) {
      records.push_back(&geometry);
    }
  } else if (mode == ResourceSyncMode::Full) {
    std::set<std::uint64_t> snapshot_handles;
    for (const auto& geometry : snapshot.geometries) {
      snapshot_handles.insert(geometry.mesh);
    }
    for (auto slot = geometry_slots_.begin(); slot != geometry_slots_.end();) {
      if (!snapshot_handles.contains(slot->first)) {
        ReleaseRange(vertex_arena_, slot->second.vertices);
        ReleaseRange(index_arena_, slot->second.indices);
        slot = geometry_slots_.erase(slot);
        structural_change = true;
        ++frame_counters_.geometry_reconcile_count;
      } else {
        ++slot;
      }
    }
    records.reserve(snapshot.geometries.size());
    for (const auto& geometry : snapshot.geometries) {
      records.push_back(&geometry);
    }
  } else {
    const auto& delta = snapshot.delta->geometries;
    for (const auto handle : delta.removals) {
      ++frame_counters_.geometry_reconcile_count;
      const auto slot = geometry_slots_.find(handle);
      if (slot == geometry_slots_.end()) {
        continue;
      }
      ReleaseRange(vertex_arena_, slot->second.vertices);
      ReleaseRange(index_arena_, slot->second.indices);
      geometry_slots_.erase(slot);
      structural_change = true;
    }
    records.reserve(delta.upserts.size());
    for (std::size_t i = 0; i < delta.upserts.size(); ++i) {
      const auto* record = FindDeltaRecord(
          snapshot.geometries, delta, i,
          [](const extraction::GeometryRecord& geometry) {
            return geometry.mesh;
          });
      if (!record) {
        throw RendererError(RendererErrorCode::InvalidRequest,
            "synchronize geometry",
            "snapshot geometry delta has no matching record");
      }
      records.push_back(record);
    }
    frame_counters_.geometry_cache_hits +=
        snapshot.geometries.size() - records.size();
  }
  frame_counters_.geometry_reconcile_count += records.size();

  struct Upload {
    const extraction::GeometryRecord* record{};
    GeometrySlot* slot{};
    bool vertices{};
    bool indices{};
    bool partial_vertices{};
    bool partial_indices{};
    VkDeviceSize vertex_bytes{};
    VkDeviceSize index_bytes{};
  };
  std::vector<Upload> uploads;
  VkDeviceSize staging_bytes{};
  // Partial copies overwrite the resident range, so they are safe only after
  // every submission that could reference that generation has completed.
  const bool resident_ranges_reusable =
      latest_completed_value_ >= timeline_value_;
  for (const auto* geometry_record : records) {
    const auto& geometry = *geometry_record;
    const auto [entry, inserted] = geometry_slots_.try_emplace(geometry.mesh);
    auto& slot = entry->second;
    Upload upload{&geometry, &slot};
    upload.vertices =
        inserted || slot.vertex_revision != geometry.vertex_revision;
    upload.indices =
        inserted || slot.topology_revision != geometry.index_revision;
    if (!upload.vertices && !upload.indices) {
      ++frame_counters_.geometry_cache_hits;
      continue;
    }
    ++frame_counters_.geometry_cache_misses;
    upload.vertex_bytes = static_cast<VkDeviceSize>(
        geometry.vertices->size() * sizeof(extraction::DrawVertex));
    upload.index_bytes = static_cast<VkDeviceSize>(
        geometry.indices->size() * sizeof(std::uint32_t));
    if (upload.vertices) {
      upload.partial_vertices =
          resident_ranges_reusable && !inserted &&
          slot.vertex_revision == geometry.vertex_base_revision &&
          !geometry.vertex_ranges.empty() && slot.vertices.valid() &&
          slot.vertices.size ==
              AlignUp(upload.vertex_bytes, kArenaAlignment);
      if (upload.partial_vertices) {
        ++frame_counters_.geometry_range_reuse_count;
        for (const auto& range : geometry.vertex_ranges) {
          staging_bytes += AlignUp(
              static_cast<VkDeviceSize>(range.count) *
                  sizeof(extraction::DrawVertex),
              kArenaAlignment);
        }
      } else {
        staging_bytes += AlignUp(upload.vertex_bytes, kArenaAlignment);
      }
    }
    if (upload.indices) {
      upload.partial_indices =
          resident_ranges_reusable && !inserted &&
          slot.topology_revision == geometry.index_base_revision &&
          !geometry.index_ranges.empty() && slot.indices.valid() &&
          slot.indices.size == AlignUp(upload.index_bytes, kArenaAlignment);
      if (upload.partial_indices) {
        ++frame_counters_.geometry_range_reuse_count;
        for (const auto& range : geometry.index_ranges) {
          staging_bytes += AlignUp(
              static_cast<VkDeviceSize>(range.count) *
                  sizeof(std::uint32_t),
              kArenaAlignment);
        }
      } else {
        staging_bytes += AlignUp(upload.index_bytes, kArenaAlignment);
      }
    }
    uploads.push_back(upload);
  }

  if (uploads.empty() && !structural_change) {
    ++frame_counters_.scene_cache_hits;
    return;
  }
  ++frame_counters_.scene_cache_misses;
  if (uploads.empty()) {
    return;
  }

  StagingRing::Reservation reservation;
  if (staging_bytes != 0) {
    Buffer retired_staging;
    VkDeviceSize staging_growth_bytes{};
    reservation =
        staging_.Reserve(staging_bytes, retired_staging,
            staging_growth_bytes);
    frame_counters_.upload_ring_reserved_bytes += staging_bytes;
    if (staging_growth_bytes != 0) {
      ++frame_counters_.allocation_count;
      ++frame_counters_.buffer_allocation_count;
      // Preserve the v3 allocation counter's requested-payload convention;
      // upload_ring_growth_bytes reports the physical ring capacity added.
      frame_counters_.buffer_allocation_bytes += staging_bytes;
      ++frame_counters_.upload_ring_growth_count;
      frame_counters_.upload_ring_growth_bytes += staging_growth_bytes;
      Retire(retired_staging);
    }
  }

  VkDeviceSize cursor{};
  auto stage = [&](const void* payload, VkDeviceSize bytes,
                   DeviceArena& arena, const BufferRange& range,
                   std::uint64_t& resource_upload_bytes,
                   VkDeviceSize destination_offset = 0) {
    if (bytes == 0) {
      return;
    }
    std::memcpy(reservation.mapped + cursor, payload,
        static_cast<std::size_t>(bytes));
    pending_copies_.push_back({reservation.buffer,
        reservation.offset + cursor,
        arena.buffer(range.block), range.offset,
        bytes});
    pending_copies_.back().destination_offset += destination_offset;
    cursor += AlignUp(bytes, kArenaAlignment);
    frame_counters_.upload_bytes += bytes;
    resource_upload_bytes += bytes;
  };
  for (auto& upload : uploads) {
    auto& slot = *upload.slot;
    if (upload.vertices) {
      if (upload.partial_vertices) {
        for (const auto& range : upload.record->vertex_ranges) {
          const auto byte_offset = static_cast<VkDeviceSize>(range.first) *
                                   sizeof(extraction::DrawVertex);
          const auto bytes = static_cast<VkDeviceSize>(range.count) *
                             sizeof(extraction::DrawVertex);
          stage(upload.record->vertices->data() + range.first, bytes,
              vertex_arena_, slot.vertices,
              frame_counters_.vertex_upload_bytes, byte_offset);
        }
      } else {
        EnsureRange(vertex_arena_, slot.vertices, upload.vertex_bytes);
        stage(upload.record->vertices->data(), upload.vertex_bytes,
            vertex_arena_, slot.vertices,
            frame_counters_.vertex_upload_bytes);
      }
      slot.vertex_revision = upload.record->vertex_revision;
    }
    if (upload.indices) {
      if (upload.partial_indices) {
        for (const auto& range : upload.record->index_ranges) {
          const auto byte_offset = static_cast<VkDeviceSize>(range.first) *
                                   sizeof(std::uint32_t);
          const auto bytes = static_cast<VkDeviceSize>(range.count) *
                             sizeof(std::uint32_t);
          stage(upload.record->indices->data() + range.first, bytes,
              index_arena_, slot.indices,
              frame_counters_.index_upload_bytes, byte_offset);
        }
      } else {
        EnsureRange(index_arena_, slot.indices, upload.index_bytes);
        stage(upload.record->indices->data(), upload.index_bytes,
            index_arena_, slot.indices,
            frame_counters_.index_upload_bytes);
      }
      slot.index_count =
          static_cast<std::uint32_t>(upload.record->indices->size());
      slot.topology_revision = upload.record->index_revision;
    }
  }
}

void Renderer::Impl::EnsureRange(DeviceArena& arena, BufferRange& range,
    VkDeviceSize bytes) {
  if (bytes == 0) {
    ReleaseRange(arena, range);
    return;
  }
  const auto aligned = arena.aligned_size(bytes);
  if (range.valid() && range.size == aligned &&
      latest_completed_value_ >= timeline_value_) {
    // Reuse in place only when every prior submission has completed. If an
    // older command buffer can still read this range, allocate a new range
    // and retire the old generation at its last possible completion value.
    ++frame_counters_.geometry_range_reuse_count;
    return;
  }
  ReleaseRange(arena, range);
  const auto allocation = arena.Allocate(bytes);
  if (allocation.created_block) {
    ++frame_counters_.allocation_count;
    ++frame_counters_.buffer_allocation_count;
    frame_counters_.buffer_allocation_bytes += allocation.block_bytes;
    ++frame_counters_.geometry_arena_growth_count;
    frame_counters_.geometry_arena_growth_bytes += allocation.block_bytes;
  }
  ++frame_counters_.buffer_suballocation_count;
  range = allocation.range;
}

void Renderer::Impl::ReleaseRange(DeviceArena& arena, BufferRange& range) {
  if (!range.valid()) {
    return;
  }
  ++frame_counters_.buffer_range_release_count;
  retired_ranges_.push_back({&arena, range, timeline_value_});
  range = {};
}

void Renderer::Impl::RecordUploads(VkCommandBuffer command, bool release_to_graphics) {
  if (pending_copies_.empty() && pending_image_copies_.empty()) {
    return;
  }
  for (const auto& copy : pending_copies_) {
    const VkBufferCopy region{copy.source_offset, copy.destination_offset,
        copy.size};
    vkCmdCopyBuffer(command, copy.source, copy.destination, 1, &region);
  }
  if (!pending_copies_.empty() && !release_to_graphics) {
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
        VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);
  }
  pending_copies_.clear();

  if (!pending_image_copies_.empty()) {
    std::vector<VkImageMemoryBarrier> to_transfer;
    to_transfer.reserve(pending_image_copies_.size());
    for (const auto& copy : pending_image_copies_) {
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = copy.destination;
      barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      barrier.subresourceRange.levelCount = 1;
      barrier.subresourceRange.layerCount = 1;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      to_transfer.push_back(barrier);
    }
    vkCmdPipelineBarrier(
        command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
        static_cast<std::uint32_t>(to_transfer.size()), to_transfer.data());
    for (const auto& copy : pending_image_copies_) {
      VkBufferImageCopy region{};
      region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      region.imageSubresource.layerCount = 1;
      region.imageExtent = {copy.width, copy.height, 1};
      vkCmdCopyBufferToImage(command, copy.source, copy.destination,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
          &region);
    }
    std::vector<VkImageMemoryBarrier> to_shader;
    to_shader.reserve(pending_image_copies_.size());
    for (const auto& copy : pending_image_copies_) {
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      barrier.srcQueueFamilyIndex = release_to_graphics
                                        ? transfer_queue_family_
                                        : VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = release_to_graphics
                                        ? queue_family_
                                        : VK_QUEUE_FAMILY_IGNORED;
      barrier.image = copy.destination;
      barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      barrier.subresourceRange.levelCount = 1;
      barrier.subresourceRange.layerCount = 1;
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask =
          release_to_graphics ? 0 : VK_ACCESS_SHADER_READ_BIT;
      to_shader.push_back(barrier);
      if (release_to_graphics) {
        pending_graphics_acquire_images_.push_back(copy.destination);
        ++frame_counters_.queue_ownership_transfer_count;
      }
    }
    vkCmdPipelineBarrier(
        command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        release_to_graphics ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
                            : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr,
        static_cast<std::uint32_t>(to_shader.size()), to_shader.data());
    pending_image_copies_.clear();
  }
}

void Renderer::Impl::RecordUploadAcquires(VkCommandBuffer command) {
  if (pending_graphics_acquire_images_.empty()) {
    return;
  }
  std::vector<VkImageMemoryBarrier> barriers;
  barriers.reserve(pending_graphics_acquire_images_.size());
  for (const auto image : pending_graphics_acquire_images_) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = transfer_queue_family_;
    barrier.dstQueueFamilyIndex = queue_family_;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers.push_back(barrier);
  }
  vkCmdPipelineBarrier(
      command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
      static_cast<std::uint32_t>(barriers.size()), barriers.data());
  pending_graphics_acquire_images_.clear();
}

void Renderer::Impl::Retire(Buffer& buffer) {
  if (buffer.handle != VK_NULL_HANDLE) {
    deferred_.push_back({buffer, timeline_value_});
    buffer = {};
  }
}

void Renderer::Impl::CollectDeferred(std::uint64_t completed,
    bool write_bindless_descriptors) {
  auto iterator = deferred_.begin();
  while (iterator != deferred_.end()) {
    if (iterator->retire_value <= completed) {
      DestroyBuffer(iterator->buffer);
      iterator = deferred_.erase(iterator);
    } else {
      ++iterator;
    }
  }
  auto range = retired_ranges_.begin();
  while (range != retired_ranges_.end()) {
    if (range->retire_value <= completed) {
      range->arena->Release(range->range);
      if (range->arena == &gaussian_position_arena_ ||
          range->arena == &gaussian_covariance_arena_ ||
          range->arena == &gaussian_opacity_arena_ ||
          range->arena == &gaussian_radiance_arena_) {
        ++statistics_.gaussian_attribute_range_retirements;
      } else {
        ++statistics_.geometry_range_retirements;
      }
      range = retired_ranges_.erase(range);
    } else {
      ++range;
    }
  }
  if (bindless_texture_table_) {
    CollectCompletedBindlessSlots(completed);
    if (write_bindless_descriptors) {
      WriteBindlessDescriptors(
          bindless_texture_table_->ConsumeDirtySlots(),
          bindless_sampler_table_->ConsumeDirtySlots());
    }
  }
  auto texture = retired_textures_.begin();
  while (texture != retired_textures_.end()) {
    if (texture->retire_value <= completed) {
      DestroyTexture(texture->texture);
      texture = retired_textures_.erase(texture);
    } else {
      ++texture;
    }
  }
  auto sampler = retired_samplers_.begin();
  while (sampler != retired_samplers_.end()) {
    if (sampler->retire_value <= completed) {
      vkDestroySampler(device_, sampler->sampler, nullptr);
      sampler = retired_samplers_.erase(sampler);
    } else {
      ++sampler;
    }
  }
}

void Renderer::Impl::Destroy(bool wait_for_submissions) noexcept {
  if (device_ != VK_NULL_HANDLE) {
    if (wait_for_submissions && owns_vulkan_context_) {
      (void)vkDeviceWaitIdle(device_);
    } else if (wait_for_submissions) {
      // Waiting only for Merlin submissions avoids idling unrelated Hgi or
      // host work on the application-owned device.
      for (auto& frame : frames_) {
        if (frame.outstanding) {
          try {
            WaitForFrame(frame, std::chrono::nanoseconds::max());
          } catch (const std::exception&) {
            // Destructors cannot report a device-loss-style failure. Keep
            // teardown best-effort while never escalating to device idle.
          }
        }
      }
    }
    ShutdownPresentationOverlay();
    for (auto& retired : deferred_) {
      DestroyBuffer(retired.buffer);
    }
    deferred_.clear();
    for (auto& retired : retired_textures_) {
      DestroyTexture(retired.texture);
    }
    retired_textures_.clear();
    for (const auto& retired : retired_samplers_) {
      vkDestroySampler(device_, retired.sampler, nullptr);
      if (retired.bindless_slot &&
          retired.bindless_slot.index < bindless_samplers_.size() &&
          bindless_samplers_[retired.bindless_slot.index] ==
              retired.sampler) {
        bindless_samplers_[retired.bindless_slot.index] = VK_NULL_HANDLE;
      }
    }
    retired_samplers_.clear();
    for (auto& [handle, texture] : texture_slots_) {
      (void)handle;
      DestroyTexture(texture);
    }
    texture_slots_.clear();
    if (bindless_sampler_table_) {
      for (const auto sampler : bindless_samplers_) {
        if (sampler != VK_NULL_HANDLE) {
          vkDestroySampler(device_, sampler, nullptr);
        }
      }
    } else {
      for (const auto& [handle, sampler] : sampler_slots_) {
        (void)handle;
        vkDestroySampler(device_, sampler.sampler, nullptr);
      }
    }
    sampler_slots_.clear();
    bindless_samplers_.clear();
    bindless_texture_views_.clear();
    for (auto& texture : reserved_bindless_textures_) {
      DestroyTexture(texture);
    }
    reserved_bindless_textures_.clear();
    DestroyTexture(fallback_texture_);
    if (fallback_sampler_.sampler != VK_NULL_HANDLE) {
      vkDestroySampler(device_, fallback_sampler_.sampler, nullptr);
    }
    for (auto& buffer : frame_upload_buffers_) {
      DestroyBuffer(buffer);
    }
    frame_upload_buffers_.clear();
    retired_ranges_.clear();
    staging_.Destroy();
    gaussian_staging_.Destroy();
    gpu_scene_staging_.Destroy();
    gpu_driven_staging_.Destroy();
    gaussian_position_arena_.Destroy();
    gaussian_covariance_arena_.Destroy();
    gaussian_opacity_arena_.Destroy();
    gaussian_radiance_arena_.Destroy();
    vertex_arena_.Destroy();
    index_arena_.Destroy();
    DestroyGpuSceneBuffers();
    DestroyBuffer(gaussian_corner_vertices_);
    for (auto& frame : frames_) {
      DestroyTarget(frame.target);
      DestroyBuffer(frame.material_uniforms);
      DestroyBuffer(frame.generated_parameter_uniforms);
      DestroyBuffer(frame.gaussian_instances);
      DestroyGpuDrivenFrameResources(frame.gpu_driven);
      DestroyGaussianGpuPreparationFrameResources(
          frame.gaussian_gpu_preparation);
      DestroyGaussianGpuSortFrameResources(frame.gaussian_gpu_sort);
      DestroyGaussianGpuRasterFrameResources(frame.gaussian_gpu_raster);
      DestroyGaussianGpuTileFrameResources(frame.gaussian_gpu_tiles);
      DestroyGaussianGpuTileRasterFrameResources(
          frame.gaussian_gpu_tile_raster);
      if (frame.image_available != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, frame.image_available, nullptr);
      }
      if (frame.timestamp_pool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device_, frame.timestamp_pool, nullptr);
      }
      if (frame.descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, frame.descriptor_pool, nullptr);
      }
      if (frame.fence != VK_NULL_HANDLE) {
        vkDestroyFence(device_, frame.fence, nullptr);
      }
      if (frame.command_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, frame.command_pool, nullptr);
      }
      if (frame.transfer_command_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, frame.transfer_command_pool, nullptr);
      }
    }
    if (bindless_descriptor_pool_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(device_, bindless_descriptor_pool_, nullptr);
    }
    for (const auto& [path, module] : shader_modules_) {
      (void)path;
      vkDestroyShaderModule(device_, module, nullptr);
    }
    shader_modules_.clear();
    if (descriptor_set_layout_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
    }
    for (const auto& [key, layout] : generated_descriptor_set_layouts_) {
      (void)key;
      vkDestroyDescriptorSetLayout(device_, layout, nullptr);
    }
    generated_descriptor_set_layouts_.clear();
    if (bindless_descriptor_set_layout_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(device_, bindless_descriptor_set_layout_,
          nullptr);
    }
    if (bindless_material_descriptor_set_layout_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(
          device_, bindless_material_descriptor_set_layout_, nullptr);
    }
    for (const auto& [path, pipeline] : gpu_driven_pipelines_) {
      (void)path;
      vkDestroyPipeline(device_, pipeline, nullptr);
    }
    gpu_driven_pipelines_.clear();
    if (gpu_driven_pipeline_layout_ != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(device_, gpu_driven_pipeline_layout_, nullptr);
    }
    if (gpu_driven_descriptor_set_layout_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(device_,
          gpu_driven_descriptor_set_layout_,
          nullptr);
    }
    for (const auto& [path, pipeline] :
        gaussian_prepare_compute_pipelines_) {
      (void)path;
      vkDestroyPipeline(device_, pipeline, nullptr);
    }
    gaussian_prepare_compute_pipelines_.clear();
    if (gaussian_prepare_pipeline_layout_ != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(device_, gaussian_prepare_pipeline_layout_,
          nullptr);
    }
    if (gaussian_prepare_descriptor_set_layout_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(
          device_, gaussian_prepare_descriptor_set_layout_, nullptr);
    }
    if (gaussian_prepare_empty_descriptor_set_layout_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(
          device_, gaussian_prepare_empty_descriptor_set_layout_, nullptr);
    }
    for (const auto& [path, pipeline] : gaussian_sort_compute_pipelines_) {
      (void)path;
      vkDestroyPipeline(device_, pipeline, nullptr);
    }
    gaussian_sort_compute_pipelines_.clear();
    if (gaussian_sort_pipeline_layout_ != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(device_, gaussian_sort_pipeline_layout_,
          nullptr);
    }
    if (gaussian_sort_descriptor_set_layout_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(
          device_, gaussian_sort_descriptor_set_layout_, nullptr);
    }
    for (const auto& [path, pipeline] :
        gaussian_tile_raster_compute_pipelines_) {
      (void)path;
      vkDestroyPipeline(device_, pipeline, nullptr);
    }
    gaussian_tile_raster_compute_pipelines_.clear();
    if (gaussian_tile_raster_pipeline_layout_ != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(device_, gaussian_tile_raster_pipeline_layout_,
          nullptr);
    }
    if (gaussian_tile_raster_descriptor_set_layout_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(
          device_, gaussian_tile_raster_descriptor_set_layout_, nullptr);
    }
    if (timeline_semaphore_ != VK_NULL_HANDLE) {
      vkDestroySemaphore(device_, timeline_semaphore_, nullptr);
    }
    if (transfer_timeline_semaphore_ != VK_NULL_HANDLE) {
      vkDestroySemaphore(device_, transfer_timeline_semaphore_, nullptr);
    }
    DestroySwapchain();
    if (owns_vulkan_context_) {
      vkDestroyDevice(device_, nullptr);
    }
  }
  if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
    vkDestroySurfaceKHR(instance_, surface_, nullptr);
  }
  if (debug_messenger_ != VK_NULL_HANDLE) {
    const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
    if (destroy != nullptr) {
      destroy(instance_, debug_messenger_, nullptr);
    }
  }
  if (owns_vulkan_context_ && instance_ != VK_NULL_HANDLE) {
    vkDestroyInstance(instance_, nullptr);
  }
}

} // namespace merlin::vulkan
