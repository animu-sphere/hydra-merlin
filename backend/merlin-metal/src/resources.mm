#include "backend_impl.hpp"

namespace merlin::metal {

void Backend::Impl::CreateHeap() {
  // GPU Scene tables remain private buffers, but their aligned footprint is
  // reserved from the configured residency budget before sizing the shared
  // scene heap. The two allocation pools therefore cannot exceed the limit.
  std::uint64_t gpu_scene_reservation{};
  if (options_.gpu_scene_capacities) {
    gpu_scene_reservation =
        GpuSceneReservation(device_, *options_.gpu_scene_capacities);
  }
  if (gpu_scene_reservation >= options_.heap_capacity_bytes) {
    throw render::RendererError(
        render::RendererErrorCode::ResourceExhausted,
        "create Metal resource heap",
        "GPU Scene tables require " +
            std::to_string(gpu_scene_reservation) +
            " bytes of the configured " +
            std::to_string(options_.heap_capacity_bytes) +
            "-byte residency budget");
  }
  MTLHeapDescriptor* descriptor =
      SceneHeapDescriptor(options_.heap_capacity_bytes -
                          gpu_scene_reservation);
  heap_ = [device_ newHeapWithDescriptor:descriptor];
  if (heap_ == nil) {
    throw render::RendererError(
        render::RendererErrorCode::ResourceExhausted,
        "create Metal resource heap",
        "newHeap returned nil for " +
            std::to_string(options_.heap_capacity_bytes) + " bytes");
  }
  heap_.label = @"hdMerlin scene residency";
}

id<MTLBuffer> Backend::Impl::HeapBuffer(std::uint64_t length, std::uint64_t* charged,
    FrameBuild& build) {
  if (length == 0) {
    *charged = 0;
    return nil;
  }
  const auto size_and_align = [device_
      heapBufferSizeAndAlignWithLength:length
                               options:MTLResourceStorageModeShared |
                                       MTLResourceCPUCacheModeWriteCombined];
  *charged = AlignUp(size_and_align.size, size_and_align.align);
  id<MTLBuffer> buffer =
      [heap_ newBufferWithLength:length
                         options:MTLResourceStorageModeShared |
                                 MTLResourceCPUCacheModeWriteCombined];
  if (buffer == nil) {
    ++metal_statistics_.heap_exhaustion_count;
    throw render::RendererError(
        render::RendererErrorCode::ResourceExhausted,
        "allocate Metal heap buffer",
        "scene heap exhausted while allocating " + std::to_string(length) +
            " bytes; capacity=" +
            std::to_string(options_.heap_capacity_bytes) + ", resident=" +
            std::to_string(metal_statistics_.heap_resident_bytes));
  }
  ChargeAllocation(*charged, build);
  return buffer;
}

id<MTLTexture> Backend::Impl::HeapTexture(MTLTextureDescriptor* descriptor,
    std::uint64_t* charged, FrameBuild& build) {
  const auto size_and_align =
      [device_ heapTextureSizeAndAlignWithDescriptor:descriptor];
  *charged = AlignUp(size_and_align.size, size_and_align.align);
  id<MTLTexture> texture = [heap_ newTextureWithDescriptor:descriptor];
  if (texture == nil) {
    ++metal_statistics_.heap_exhaustion_count;
    throw render::RendererError(
        render::RendererErrorCode::ResourceExhausted,
        "allocate Metal heap texture",
        "scene heap exhausted while allocating texture; capacity=" +
            std::to_string(options_.heap_capacity_bytes) + ", resident=" +
            std::to_string(metal_statistics_.heap_resident_bytes));
  }
  ChargeAllocation(*charged, build);
  return texture;
}

void Backend::Impl::ChargeAllocation(std::uint64_t bytes, FrameBuild& build) {
  metal_statistics_.heap_resident_bytes += bytes;
  metal_statistics_.heap_peak_resident_bytes =
      std::max(metal_statistics_.heap_peak_resident_bytes,
          metal_statistics_.heap_resident_bytes);
  ++metal_statistics_.heap_allocation_count;
  ++build.telemetry.allocation_count;
  build.telemetry.buffer_allocation_bytes += bytes;
}

void Backend::Impl::Retire(id<MTLResource> first, id<MTLResource> second,
    std::uint64_t bytes) {
  if (first == nil && second == nil) {
    return;
  }
  retirements_.push_back({submitted_value_, bytes, first, second});
  ++metal_statistics_.scene_resource_retirements;
}

void Backend::Impl::CollectRetirements() {
  const auto completed = completed_value_.load(std::memory_order_acquire);
  texture_slots_.Collect(completed);
  sampler_slots_.Collect(completed);
  auto output = retirements_.begin();
  for (auto current = retirements_.begin(); current != retirements_.end();
      ++current) {
    if (current->completion_value <= completed) {
      metal_statistics_.heap_resident_bytes -= current->bytes;
      metal_statistics_.heap_release_count +=
          static_cast<std::uint64_t>(current->first != nil) +
          static_cast<std::uint64_t>(current->second != nil);
    } else {
      if (output != current) {
        *output = std::move(*current);
      }
      ++output;
    }
  }
  retirements_.erase(output, retirements_.end());
}

void Backend::Impl::Reconcile(const extraction::FrameSnapshot& snapshot, FrameBuild& build) {
  const auto begin = Clock::now();
  std::unordered_set<std::uint64_t> active;
  for (const auto& record : snapshot.geometries) {
    active.insert(record.mesh);
  }
  for (auto it = geometries_.begin(); it != geometries_.end();) {
    if (!active.contains(it->first)) {
      Retire(it->second.vertices, it->second.indices, it->second.bytes);
      it = geometries_.erase(it);
    } else {
      ++it;
    }
  }
  CollectRetirements();
  for (const auto& record : snapshot.geometries) {
    const auto found = geometries_.find(record.mesh);
    if (found != geometries_.end() &&
        found->second.vertex_revision == record.vertex_revision &&
        found->second.index_revision == record.index_revision) {
      continue;
    }
    if (!record.vertices || !record.indices) {
      throw render::RendererError(render::RendererErrorCode::InvalidRequest,
          "upload Metal geometry",
          "geometry payload is null");
    }
    Geometry replacement;
    replacement.vertex_revision = record.vertex_revision;
    replacement.index_revision = record.index_revision;
    replacement.index_count =
        static_cast<std::uint32_t>(record.indices->size());
    const auto vertex_bytes =
        record.vertices->size() * sizeof(extraction::DrawVertex);
    const auto index_bytes = record.indices->size() * sizeof(std::uint32_t);
    std::uint64_t vertex_charged{};
    std::uint64_t index_charged{};
    try {
      replacement.vertices = HeapBuffer(vertex_bytes, &vertex_charged, build);
      replacement.indices = HeapBuffer(index_bytes, &index_charged, build);
    } catch (...) {
      if (replacement.vertices != nil) {
        metal_statistics_.heap_resident_bytes -= vertex_charged;
        ++metal_statistics_.heap_release_count;
      }
      throw;
    }
    if (vertex_bytes != 0) {
      std::memcpy(replacement.vertices.contents, record.vertices->data(),
          vertex_bytes);
    }
    if (index_bytes != 0) {
      std::memcpy(replacement.indices.contents, record.indices->data(),
          index_bytes);
    }
    replacement.bytes = vertex_charged + index_charged;
    build.telemetry.upload_bytes += vertex_bytes + index_bytes;
    build.telemetry.geometry_cache_misses++;
    build.telemetry.geometry_reconcile_count++;
    if (found != geometries_.end()) {
      Retire(found->second.vertices, found->second.indices,
          found->second.bytes);
      found->second = std::move(replacement);
    } else {
      geometries_.emplace(record.mesh, std::move(replacement));
    }
  }
  geometry_peak_resident_bytes_ =
      std::max(geometry_peak_resident_bytes_, GeometryResidentBytes());

  active.clear();
  for (const auto& record : snapshot.textures) {
    active.insert(record.texture);
  }
  for (auto it = textures_.begin(); it != textures_.end();) {
    if (!active.contains(it->first)) {
      texture_slots_.Release(it->first, submitted_value_);
      Retire(it->second.texture, nil, it->second.bytes);
      it = textures_.erase(it);
    } else {
      ++it;
    }
  }
  CollectRetirements();
  for (const auto& record : snapshot.textures) {
    const auto found = textures_.find(record.texture);
    if (found != textures_.end() &&
        found->second.revision == record.revision) {
      ++build.telemetry.texture_cache_hits;
      continue;
    }
    if (!record.pixels || record.width == 0 || record.height == 0 ||
        record.pixels->size() !=
            static_cast<std::size_t>(record.width) * record.height * 4U) {
      throw render::RendererError(render::RendererErrorCode::InvalidRequest,
          "upload Metal texture",
          "texture payload is invalid");
    }
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                     width:record.width
                                    height:record.height
                                 mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.cpuCacheMode = MTLCPUCacheModeWriteCombined;
    descriptor.usage = MTLTextureUsageShaderRead;
    Texture replacement;
    replacement.revision = record.revision;
    const bool new_resource = found == textures_.end();
    if (new_resource) {
      (void)AcquireSlot(texture_slots_, record.texture,
          "allocate Metal texture slot");
    }
    try {
      replacement.texture =
          HeapTexture(descriptor, &replacement.bytes, build);
    } catch (...) {
      if (new_resource) {
        texture_slots_.Release(record.texture, CompletedValue());
        texture_slots_.Collect(CompletedValue());
      }
      throw;
    }
    const MTLRegion region =
        MTLRegionMake2D(0, 0, record.width, record.height);
    [replacement.texture replaceRegion:region
                           mipmapLevel:0
                             withBytes:record.pixels->data()
                           bytesPerRow:record.width * 4U];
    build.telemetry.upload_bytes += record.pixels->size();
    ++build.telemetry.texture_cache_misses;
    ++build.telemetry.texture_reconcile_count;
    if (found != textures_.end()) {
      Retire(found->second.texture, nil, found->second.bytes);
      found->second = std::move(replacement);
    } else {
      textures_.emplace(record.texture, std::move(replacement));
    }
  }

  active.clear();
  for (const auto& record : snapshot.samplers) {
    active.insert(record.sampler);
  }
  for (auto it = samplers_.begin(); it != samplers_.end();) {
    if (!active.contains(it->first)) {
      sampler_slots_.Release(it->first, submitted_value_);
      it = samplers_.erase(it);
    } else {
      ++it;
    }
  }
  sampler_slots_.Collect(CompletedValue());
  for (const auto& record : snapshot.samplers) {
    const auto found = samplers_.find(record.sampler);
    if (found != samplers_.end() &&
        found->second.revision == record.revision) {
      continue;
    }
    MTLSamplerDescriptor* descriptor = [MTLSamplerDescriptor new];
    descriptor.minFilter = Filter(record.min_filter);
    descriptor.magFilter = Filter(record.mag_filter);
    descriptor.sAddressMode = Address(record.address_u);
    descriptor.tAddressMode = Address(record.address_v);
    descriptor.normalizedCoordinates = YES;
    descriptor.supportArgumentBuffers = bindless_;
    const bool new_resource = found == samplers_.end();
    if (new_resource) {
      (void)AcquireSlot(sampler_slots_, record.sampler,
          "allocate Metal sampler slot");
    }
    Sampler replacement{record.revision,
        [device_ newSamplerStateWithDescriptor:descriptor]};
    if (replacement.sampler == nil) {
      if (new_resource) {
        sampler_slots_.Release(record.sampler, CompletedValue());
        sampler_slots_.Collect(CompletedValue());
      }
      throw render::RendererError(
          render::RendererErrorCode::ResourceExhausted,
          "create Metal sampler", "newSamplerState returned nil");
    }
    ++build.telemetry.sampler_reconcile_count;
    if (found != samplers_.end()) {
      found->second = replacement;
    } else {
      samplers_.emplace(record.sampler, replacement);
    }
  }
  build.upload_ns = DurationNs(begin, Clock::now());
}

ResourceSlot Backend::Impl::AcquireSlot(StableResourceTable& table, std::uint64_t resource,
    const char* operation) {
  try {
    return table.Acquire(resource, CompletedValue());
  } catch (const std::length_error&) {
    const auto telemetry = table.telemetry();
    throw render::RendererError(
        render::RendererErrorCode::ResourceExhausted, operation,
        "stable resource table exhausted; capacity=" +
            std::to_string(telemetry.capacity) +
            ", in_use=" + std::to_string(telemetry.in_use) +
            ", retiring=" + std::to_string(telemetry.retiring));
  }
}

std::uint64_t Backend::Impl::CompletedValue() const noexcept {
  return completed_value_.load(std::memory_order_acquire);
}

void Backend::Impl::EnsureTargets(FrameContext& frame, std::uint32_t width,
    std::uint32_t height, bool gaussian_color, FrameBuild& build) {
  const bool reuse = frame.width == width && frame.height == height && frame.color != nil;
  auto texture = [&](MTLPixelFormat format, MTLTextureUsage usage) {
    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = usage;
    id<MTLTexture> value = [device_ newTextureWithDescriptor:descriptor];
    if (value == nil) {
      throw render::RendererError(
          render::RendererErrorCode::ResourceExhausted,
          "allocate Metal render target", "newTexture returned nil");
    }
    ++build.telemetry.allocation_count;
    build.telemetry.image_allocation_bytes +=
        static_cast<std::uint64_t>(width) * height * (format == MTLPixelFormatRGBA32Float ? 16U : 4U);
    return value;
  };
  const auto gaussian_usage = MTLTextureUsageRenderTarget |
                              MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
  if (reuse) {
    if (gaussian_color && !frame.gaussian_color)
      frame.gaussian_color = texture(MTLPixelFormatRGBA32Float, gaussian_usage);
    return;
  }
  id<MTLTexture> float_color = gaussian_color
                                   ? texture(MTLPixelFormatRGBA32Float, gaussian_usage)
                                   : nil;
  id<MTLTexture> color =
      texture(MTLPixelFormatRGBA8Unorm, gaussian_usage);
  id<MTLTexture> depth =
      texture(MTLPixelFormatDepth32Float, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
  id<MTLTexture> prim_id =
      texture(MTLPixelFormatR32Uint, gaussian_usage);
  id<MTLTexture> instance_id =
      texture(MTLPixelFormatR32Uint, gaussian_usage);
  const auto bytes = static_cast<NSUInteger>(AlignedRowPitch(width)) * height;
  auto buffer = [&] {
    id<MTLBuffer> value =
        [device_ newBufferWithLength:bytes
                             options:MTLResourceStorageModeShared |
                                     MTLResourceCPUCacheModeDefaultCache];
    if (value == nil) {
      throw render::RendererError(
          render::RendererErrorCode::ResourceExhausted,
          "allocate Metal readback", "newBuffer returned nil");
    }
    ++build.telemetry.allocation_count;
    build.telemetry.buffer_allocation_bytes += bytes;
    return value;
  };
  id<MTLBuffer> color_readback = buffer();
  id<MTLBuffer> depth_readback = buffer();
  id<MTLBuffer> prim_readback = buffer();
  id<MTLBuffer> instance_readback = buffer();
  frame.width = width;
  frame.height = height;
  frame.color = color;
  frame.gaussian_color = float_color;
  frame.depth = depth;
  frame.prim_id = prim_id;
  frame.instance_id = instance_id;
  frame.color_readback = color_readback;
  frame.depth_readback = depth_readback;
  frame.prim_readback = prim_readback;
  frame.instance_readback = instance_readback;
}

void Backend::Impl::EncodeArgumentBuffer(FrameContext& frame, FrameBuild& build) {
  [argument_encoder_ setArgumentBuffer:frame.argument_buffer offset:0];
  std::vector<const Texture*> by_texture(options_.texture_capacity);
  std::vector<std::uint64_t> texture_handles(options_.texture_capacity);
  for (const auto& [handle, texture] : textures_) {
    const auto slot = texture_slots_.Find(handle);
    if (slot) {
      by_texture[slot->index] = &texture;
      texture_handles[slot->index] = handle;
    }
  }
  std::vector<const Sampler*> by_sampler(options_.sampler_capacity);
  std::vector<std::uint64_t> sampler_handles(options_.sampler_capacity);
  for (const auto& [handle, sampler] : samplers_) {
    const auto slot = sampler_slots_.Find(handle);
    if (slot) {
      by_sampler[slot->index] = &sampler;
      sampler_handles[slot->index] = handle;
    }
  }

  std::uint64_t updates{};
  for (std::uint32_t index = 0; index < options_.texture_capacity; ++index) {
    const auto revision =
        by_texture[index] == nullptr ? 0 : by_texture[index]->revision;
    if (frame.encoded_textures[index] != texture_handles[index] ||
        frame.encoded_texture_revisions[index] != revision) {
      [argument_encoder_
          setTexture:by_texture[index] == nullptr ? nil
                                                  : by_texture[index]->texture
             atIndex:index];
      frame.encoded_textures[index] = texture_handles[index];
      frame.encoded_texture_revisions[index] = revision;
      ++updates;
      ++build.telemetry.bindless_sampled_image_descriptor_update_count;
    }
  }
  for (std::uint32_t index = 0; index < options_.sampler_capacity; ++index) {
    const auto revision =
        by_sampler[index] == nullptr ? 0 : by_sampler[index]->revision;
    if (frame.encoded_samplers[index] != sampler_handles[index] ||
        frame.encoded_sampler_revisions[index] != revision) {
      [argument_encoder_ setSamplerState:by_sampler[index] == nullptr
                                             ? nil
                                             : by_sampler[index]->sampler
                                 atIndex:kShaderTextureCapacity + index];
      frame.encoded_samplers[index] = sampler_handles[index];
      frame.encoded_sampler_revisions[index] = revision;
      ++updates;
      ++build.telemetry.bindless_sampler_descriptor_update_count;
    }
  }
  if (updates != 0) {
    ++metal_statistics_.argument_buffer_encode_count;
    metal_statistics_.argument_buffer_update_count += updates;
    build.telemetry.descriptor_update_count += updates;
  }
}

std::uint64_t Backend::Impl::GeometryResidentBytes() const noexcept {
  std::uint64_t result{};
  for (const auto& [handle, geometry] : geometries_) {
    (void)handle;
    result += geometry.bytes;
  }
  return result;
}

std::uint64_t Backend::Impl::RetiringBytes() const noexcept {
  std::uint64_t result{};
  for (const auto& retirement : retirements_) {
    result += retirement.bytes;
  }
  return result;
}

} // namespace merlin::metal
