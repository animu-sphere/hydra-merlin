#include "renderer_impl.hpp"

namespace merlin::vulkan {

void Renderer::Impl::CreateGaussianCornerBuffer() {
  constexpr std::array<Vec2, 6> corners{{
      {-1.0F, -1.0F},
      {1.0F, -1.0F},
      {1.0F, 1.0F},
      {-1.0F, -1.0F},
      {1.0F, 1.0F},
      {-1.0F, 1.0F},
  }};
  gaussian_corner_vertices_ = CreateBuffer(
      sizeof(corners), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  void* mapped{};
  Check(vkMapMemory(device_, gaussian_corner_vertices_.memory, 0,
            sizeof(corners), 0, &mapped),
      "map Gaussian corner vertices");
  std::memcpy(mapped, corners.data(), sizeof(corners));
  vkUnmapMemory(device_, gaussian_corner_vertices_.memory);
}

std::filesystem::path Renderer::Impl::GaussianPrepareComputePath(
    const ShaderPaths& shaders) {
  // gaussian_vertex is itself optional and then resolves beside the base
  // vertex artifact; an empty parent_path() would look in the working
  // directory instead of the packaged shader directory.
  const auto gaussian_vertex_directory =
      shaders.gaussian_vertex.empty()
          ? shaders.vertex.parent_path()
          : shaders.gaussian_vertex.parent_path();
  return shaders.gaussian_prepare_compute.empty()
             ? gaussian_vertex_directory / "gaussian-prepare.comp.spv"
             : shaders.gaussian_prepare_compute;
}

// Prepares and sorts the CPU reference stream, reusing it while the
// Gaussian table, camera, and viewport are unchanged. A frame drawn from
// the GPU-sorted stream without validation does no per-Gaussian CPU work:
// it counts only per-resource totals, and resolve fills the per-particle
// counters from the GPU preparation. Returns the preparation time.
std::uint64_t Renderer::Impl::PrepareGaussianCpuReference(const RenderRequest& request,
    bool required) {
  std::uint64_t preparation_ns{};
  if (!required) {
    // The retained stream no longer matches this frame's inputs, so a later
    // CPU-sorted frame prepares again instead of reusing it.
    gaussian_preparation_cache_valid_ = false;
    ++frame_counters_.gaussian_cpu_preparation_skipped_count;
    std::uint64_t hidden_count{};
    for (const auto& record : request.snapshot->gaussians) {
      if (!record.visible && record.positions) {
        hidden_count += record.positions->size();
      }
    }
    frame_counters_.gaussian_hidden_count = hidden_count;
    frame_counters_.gaussian_sorting_policy_fallback_count =
        detail::SelectGaussianSortingPolicy(*request.snapshot)
            .fallback_resource_count;
    return preparation_ns;
  }
  const bool gaussian_preparation_cache_hit =
      gaussian_preparation_cache_valid_ &&
      gaussian_preparation_source_.table_identity() ==
          request.snapshot->gaussians.table_identity() &&
      gaussian_preparation_view_.values == request.snapshot->view.values &&
      gaussian_preparation_projection_.values ==
          request.snapshot->projection.values &&
      gaussian_preparation_width_ == request.width &&
      gaussian_preparation_height_ == request.height;
  if (gaussian_preparation_cache_hit) {
    ++frame_counters_.gaussian_preparation_cache_hits;
  } else {
    const auto gaussian_preparation_start = CpuClock::now();
    prepared_gaussians_ = detail::PrepareGaussianFrame(
        *request.snapshot, {request.width, request.height});
    preparation_ns = ElapsedNanoseconds(gaussian_preparation_start);
    gaussian_preparation_source_ = request.snapshot->gaussians;
    gaussian_preparation_view_ = request.snapshot->view;
    gaussian_preparation_projection_ = request.snapshot->projection;
    gaussian_preparation_width_ = request.width;
    gaussian_preparation_height_ = request.height;
    gaussian_preparation_cache_valid_ = true;
    ++gaussian_preparation_generation_;
    if (gaussian_preparation_generation_ == 0) {
      ++gaussian_preparation_generation_;
      for (auto& candidate_frame : frames_) {
        candidate_frame.gaussian_preparation_generation = 0;
      }
    }
    ++frame_counters_.gaussian_preparation_cache_misses;
  }
  frame_counters_.gaussian_candidate_count =
      prepared_gaussians_.counters.candidate_count;
  frame_counters_.gaussian_visible_count =
      prepared_gaussians_.counters.visible_count;
  frame_counters_.gaussian_hidden_count =
      prepared_gaussians_.counters.hidden_count;
  frame_counters_.gaussian_opacity_culled_count =
      prepared_gaussians_.counters.opacity_culled_count;
  frame_counters_.gaussian_frustum_culled_count =
      prepared_gaussians_.counters.frustum_culled_count;
  frame_counters_.gaussian_invalid_culled_count =
      prepared_gaussians_.counters.invalid_culled_count;
  frame_counters_.gaussian_sorted_count =
      prepared_gaussians_.counters.sorted_count;
  frame_counters_.gaussian_sorting_policy_fallback_count =
      prepared_gaussians_.counters.sorting_policy_fallback_count;
  return preparation_ns;
}

void Renderer::Impl::PrepareGaussianInstances(FrameContext& frame) {
  if (frame.gaussian_preparation_generation ==
      gaussian_preparation_generation_) {
    return;
  }
  if (prepared_gaussians_.gaussians.size() >
      std::numeric_limits<std::uint32_t>::max()) {
    throw RendererError(RendererErrorCode::Unsupported,
        "upload prepared Gaussian stream",
        "visible Gaussian count exceeds uint32 draw limit");
  }
  if (prepared_gaussians_.gaussians.empty()) {
    frame.gaussian_instance_count = 0;
    frame.gaussian_instance_shadow.clear();
    frame.gaussian_preparation_generation = gaussian_preparation_generation_;
    return;
  }
  std::vector<GaussianGpuInstance> packed;
  packed.reserve(prepared_gaussians_.gaussians.size());
  for (const auto& source : prepared_gaussians_.gaussians) {
    packed.push_back({
        source.center_pixels,
        source.inverse_conic,
        source.radiance,
        source.opacity,
        source.radius_pixels,
        source.depth,
        static_cast<std::uint32_t>(source.resource),
        source.particle,
    });
  }
  const auto bytes = static_cast<VkDeviceSize>(
      packed.size() * sizeof(GaussianGpuInstance));
  bool replaced = false;
  if (frame.gaussian_instances.handle == VK_NULL_HANDLE ||
      frame.gaussian_instances.size < bytes) {
    auto replacement = CreateBuffer(
        bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    DestroyBuffer(frame.gaussian_instances);
    frame.gaussian_instances = replacement;
    replaced = true;
  }
  struct InstanceRange {
    std::size_t first{};
    std::size_t count{};
  };
  std::vector<InstanceRange> changed_ranges;
  if (replaced) {
    changed_ranges.push_back({0, packed.size()});
  } else {
    const auto shared_count =
        std::min(frame.gaussian_instance_shadow.size(), packed.size());
    std::size_t index{};
    while (index < shared_count) {
      if (std::memcmp(&frame.gaussian_instance_shadow[index], &packed[index],
              sizeof(GaussianGpuInstance)) == 0) {
        ++index;
        continue;
      }
      const auto first = index++;
      while (index < shared_count &&
             std::memcmp(&frame.gaussian_instance_shadow[index],
                 &packed[index], sizeof(GaussianGpuInstance)) != 0) {
        ++index;
      }
      changed_ranges.push_back({first, index - first});
    }
    if (packed.size() > shared_count) {
      changed_ranges.push_back(
          {shared_count, packed.size() - shared_count});
    }
  }
  std::uint64_t uploaded_bytes{};
  for (const auto& range : changed_ranges) {
    uploaded_bytes += range.count * sizeof(GaussianGpuInstance);
  }
  if (uploaded_bytes != 0) {
    void* mapped{};
    Check(vkMapMemory(device_, frame.gaussian_instances.memory, 0, bytes, 0,
              &mapped),
        "map prepared Gaussian stream");
    auto* destination = static_cast<GaussianGpuInstance*>(mapped);
    for (const auto& range : changed_ranges) {
      std::memcpy(destination + range.first, packed.data() + range.first,
          range.count * sizeof(GaussianGpuInstance));
    }
    vkUnmapMemory(device_, frame.gaussian_instances.memory);
  }
  frame.gaussian_instance_shadow = std::move(packed);
  frame.gaussian_instance_count =
      static_cast<std::uint32_t>(prepared_gaussians_.gaussians.size());
  frame.gaussian_preparation_generation = gaussian_preparation_generation_;
  frame_counters_.gaussian_upload_bytes += uploaded_bytes;
  frame_counters_.upload_bytes += uploaded_bytes;
}

void Renderer::Impl::ReleaseGaussianAttributes(GaussianAttributeSlot& slot) {
  ReleaseRange(gaussian_position_arena_, slot.positions);
  ReleaseRange(gaussian_covariance_arena_, slot.covariances);
  ReleaseRange(gaussian_opacity_arena_, slot.opacities);
  ReleaseRange(gaussian_radiance_arena_, slot.radiance);
}

// Retains source-space Gaussian attributes independently of the
// camera-dependent prepared stream. Exact snapshot deltas visit only changed
// resources; exact particle ranges visit only changed elements of the
// affected attribute arrays when completion safety permits in-place reuse.
void Renderer::Impl::SyncGaussianAttributes(const extraction::FrameSnapshot& snapshot,
    ResourceSyncMode mode) {
  if (mode == ResourceSyncMode::Unchanged) {
    return;
  }

  std::vector<const extraction::GaussianRecord*> records;
  if (mode == ResourceSyncMode::ReplaceAll) {
    for (auto& [handle, slot] : gaussian_attribute_slots_) {
      (void)handle;
      ReleaseGaussianAttributes(slot);
    }
    gaussian_attribute_slots_.clear();
    records.reserve(snapshot.gaussians.size());
    for (const auto& gaussian : snapshot.gaussians) {
      records.push_back(&gaussian);
    }
  } else if (mode == ResourceSyncMode::Full) {
    std::set<std::uint64_t> snapshot_handles;
    for (const auto& gaussian : snapshot.gaussians) {
      snapshot_handles.insert(gaussian.gaussian);
      records.push_back(&gaussian);
    }
    for (auto slot = gaussian_attribute_slots_.begin();
        slot != gaussian_attribute_slots_.end();) {
      if (!snapshot_handles.contains(slot->first)) {
        ReleaseGaussianAttributes(slot->second);
        slot = gaussian_attribute_slots_.erase(slot);
      } else {
        ++slot;
      }
    }
  } else {
    const auto& delta = snapshot.delta->gaussians;
    for (const auto handle : delta.removals) {
      const auto slot = gaussian_attribute_slots_.find(handle);
      if (slot == gaussian_attribute_slots_.end()) {
        continue;
      }
      ReleaseGaussianAttributes(slot->second);
      gaussian_attribute_slots_.erase(slot);
    }
    records.reserve(delta.upserts.size());
    for (std::size_t index = 0; index < delta.upserts.size(); ++index) {
      const auto* record =
          FindDeltaRecord(snapshot.gaussians, delta, index,
              [](const extraction::GaussianRecord& gaussian) {
                return gaussian.gaussian;
              });
      if (!record) {
        throw RendererError(RendererErrorCode::InvalidRequest,
            "synchronize Gaussian attributes",
            "snapshot Gaussian delta has no matching record");
      }
      records.push_back(record);
    }
  }

  struct Upload {
    const extraction::GaussianRecord* record{};
    GaussianAttributeSlot* slot{};
    bool inserted{};
    bool positions{};
    bool covariances{};
    bool opacities{};
    bool radiance{};
    bool partial{};
    std::uint32_t coefficient_count{};
  };
  std::vector<Upload> uploads;
  VkDeviceSize staging_bytes{};
  const bool resident_ranges_reusable =
      latest_completed_value_ >= timeline_value_;

  const auto checked_bytes = [](std::size_t count, std::size_t element_size,
                                 std::string_view label) -> VkDeviceSize {
    if (count > std::numeric_limits<VkDeviceSize>::max() / element_size) {
      throw RendererError(RendererErrorCode::Unsupported,
          "synchronize Gaussian attributes",
          std::string(label) + " payload is too large");
    }
    return static_cast<VkDeviceSize>(count * element_size);
  };
  for (const auto* record : records) {
    if (!record->positions || !record->covariances || !record->opacities ||
        !record->spherical_harmonics_coefficients ||
        record->positions->size() != record->covariances->size() ||
        record->positions->size() != record->opacities->size() ||
        record->positions->size() >
            std::numeric_limits<std::uint32_t>::max() ||
        record->spherical_harmonics_degree > 3) {
      throw RendererError(RendererErrorCode::InvalidRequest,
          "synchronize Gaussian attributes",
          "Gaussian attribute payload is malformed");
    }
    const auto coefficient_count = (record->spherical_harmonics_degree + 1U) *
                                   (record->spherical_harmonics_degree + 1U);
    if (record->spherical_harmonics_coefficients->size() !=
        record->positions->size() * coefficient_count) {
      throw RendererError(RendererErrorCode::InvalidRequest,
          "synchronize Gaussian attributes",
          "Gaussian SH payload size is inconsistent");
    }
    for (const auto& range : record->particle_ranges) {
      if (range.first > record->positions->size() ||
          range.count > record->positions->size() - range.first) {
        throw RendererError(RendererErrorCode::InvalidRequest,
            "synchronize Gaussian attributes",
            "Gaussian changed range is out of bounds");
      }
    }

    const auto [entry, inserted] =
        gaussian_attribute_slots_.try_emplace(record->gaussian);
    auto& slot = entry->second;
    Upload upload{record, &slot, inserted};
    upload.coefficient_count = coefficient_count;
    upload.positions =
        inserted || slot.positions_revision != record->positions_revision;
    upload.covariances =
        inserted || slot.covariance_revision != record->covariance_revision;
    upload.opacities =
        inserted || slot.opacity_revision != record->opacity_revision;
    upload.radiance = inserted ||
                      slot.radiance_revision != record->radiance_revision ||
                      slot.coefficients_per_particle != coefficient_count;
    const bool attributes_changed = upload.positions || upload.covariances ||
                                    upload.opacities || upload.radiance;
    const bool record_changed =
        inserted || slot.record_revision != record->revision;
    if (!attributes_changed && !record_changed) {
      continue;
    }

    const auto position_bytes =
        checked_bytes(record->positions->size(), sizeof(Vec3), "position");
    const auto covariance_bytes = checked_bytes(
        record->covariances->size(), sizeof(Covariance3), "covariance");
    const auto opacity_bytes =
        checked_bytes(record->opacities->size(), sizeof(float), "opacity");
    const auto radiance_bytes =
        checked_bytes(record->spherical_harmonics_coefficients->size(),
            sizeof(Vec3), "spherical-harmonic");
    upload.partial =
        attributes_changed && resident_ranges_reusable && !inserted &&
        slot.record_revision == record->particle_base_revision &&
        !record->particle_ranges.empty() &&
        slot.particle_count == record->positions->size() &&
        slot.coefficients_per_particle == coefficient_count &&
        (!upload.positions ||
            slot.positions.size ==
                gaussian_position_arena_.aligned_size(position_bytes)) &&
        (!upload.covariances ||
            slot.covariances.size ==
                gaussian_covariance_arena_.aligned_size(covariance_bytes)) &&
        (!upload.opacities ||
            slot.opacities.size ==
                gaussian_opacity_arena_.aligned_size(opacity_bytes)) &&
        (!upload.radiance ||
            slot.radiance.size ==
                gaussian_radiance_arena_.aligned_size(radiance_bytes));

    const auto add_bytes = [&](std::size_t element_size,
                               std::uint32_t multiplier = 1U) {
      if (upload.partial) {
        for (const auto& range : record->particle_ranges) {
          staging_bytes +=
              AlignUp(checked_bytes(static_cast<std::size_t>(range.count) *
                                        multiplier,
                          element_size, "changed range"),
                  kArenaAlignment);
        }
      } else {
        staging_bytes +=
            AlignUp(checked_bytes(record->positions->size() * multiplier,
                        element_size, "attribute"),
                kArenaAlignment);
      }
    };
    if (upload.positions)
      add_bytes(sizeof(Vec3));
    if (upload.covariances)
      add_bytes(sizeof(Covariance3));
    if (upload.opacities)
      add_bytes(sizeof(float));
    if (upload.radiance)
      add_bytes(sizeof(Vec3), coefficient_count);
    uploads.push_back(upload);
  }

  if (uploads.empty()) {
    return;
  }
  StagingRing::Reservation reservation;
  if (staging_bytes != 0) {
    Buffer retired_staging;
    VkDeviceSize growth_bytes{};
    reservation = gaussian_staging_.Reserve(staging_bytes, retired_staging,
        growth_bytes);
    if (growth_bytes != 0) {
      ++frame_counters_.allocation_count;
      ++frame_counters_.buffer_allocation_count;
      frame_counters_.buffer_allocation_bytes += staging_bytes;
      Retire(retired_staging);
    }
  }

  VkDeviceSize cursor{};
  const auto stage = [&](const void* payload, VkDeviceSize bytes,
                         DeviceArena& arena, const BufferRange& range,
                         VkDeviceSize destination_offset) {
    if (bytes == 0)
      return;
    std::memcpy(reservation.mapped + cursor, payload,
        static_cast<std::size_t>(bytes));
    pending_copies_.push_back({reservation.buffer,
        reservation.offset + cursor,
        arena.buffer(range.block),
        range.offset + destination_offset, bytes});
    cursor += AlignUp(bytes, kArenaAlignment);
    frame_counters_.upload_bytes += bytes;
    frame_counters_.gaussian_attribute_upload_bytes += bytes;
    ++frame_counters_.gaussian_attribute_copy_range_count;
  };
  for (auto& upload : uploads) {
    auto& slot = *upload.slot;
    const auto& record = *upload.record;
    const auto particle_count = record.positions->size();
    const auto upload_attribute =
        [&](bool changed, const auto& payload, std::size_t stride,
            std::uint32_t multiplier, DeviceArena& arena,
            BufferRange& range) {
          if (!changed)
            return;
          const auto total_bytes =
              checked_bytes(particle_count * multiplier, stride, "attribute");
          if (!upload.partial) {
            EnsureRange(arena, range, total_bytes);
            stage(payload.data(), total_bytes, arena, range, 0);
            return;
          }
          for (const auto& changed_range : record.particle_ranges) {
            const auto first =
                static_cast<std::size_t>(changed_range.first) * multiplier;
            const auto count =
                static_cast<std::size_t>(changed_range.count) * multiplier;
            stage(payload.data() + first,
                checked_bytes(count, stride, "changed range"), arena, range,
                checked_bytes(first, stride, "changed offset"));
          }
        };
    upload_attribute(upload.positions, *record.positions, sizeof(Vec3), 1,
        gaussian_position_arena_, slot.positions);
    upload_attribute(upload.covariances, *record.covariances,
        sizeof(Covariance3), 1, gaussian_covariance_arena_,
        slot.covariances);
    upload_attribute(upload.opacities, *record.opacities, sizeof(float), 1,
        gaussian_opacity_arena_, slot.opacities);
    upload_attribute(upload.radiance,
        *record.spherical_harmonics_coefficients, sizeof(Vec3),
        upload.coefficient_count, gaussian_radiance_arena_,
        slot.radiance);

    slot.record_revision = record.revision;
    slot.positions_revision = record.positions_revision;
    slot.covariance_revision = record.covariance_revision;
    slot.opacity_revision = record.opacity_revision;
    slot.radiance_revision = record.radiance_revision;
    slot.policy_revision = record.policy_revision;
    slot.transform_revision = record.transform_revision;
    slot.visibility_revision = record.visibility_revision;
    slot.particle_count = static_cast<std::uint32_t>(particle_count);
    slot.coefficients_per_particle = upload.coefficient_count;
    ++slot.generation;
    if (slot.generation == 0)
      ++slot.generation;
    ++frame_counters_.gaussian_attribute_generation_count;
  }
}

bool Renderer::Impl::GaussianFloatColorSupported() {
  if (!gaussian_float_color_supported_) {
    VkFormatProperties source{};
    VkFormatProperties destination{};
    vkGetPhysicalDeviceFormatProperties(physical_device_, kGaussianColorFormat,
        &source);
    vkGetPhysicalDeviceFormatProperties(physical_device_, kColorFormat,
        &destination);
    constexpr auto features = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                              VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                              VK_FORMAT_FEATURE_BLIT_SRC_BIT;
    gaussian_float_color_supported_ =
        (source.optimalTilingFeatures & features) == features &&
        (destination.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
  }
  return *gaussian_float_color_supported_;
}

} // namespace merlin::vulkan
