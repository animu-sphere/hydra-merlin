#include "gaussian_execution.hpp"
#include "gaussian_raster_abi.hpp"

#include <merlin/render/backend.hpp>

#include <algorithm>
#include <bit>
#include <limits>

namespace merlin::metal {
namespace {
using namespace gaussian_compute;

[[noreturn]] void Fail(render::RendererErrorCode code, const char* message) {
  throw render::RendererError(code, "encode Metal Gaussian execution", message);
}

std::uint32_t Groups(std::uint32_t count, std::uint32_t size) {
  return count / size + (count % size != 0);
}

Mat4 Multiply(const Mat4& a, const Mat4& b) {
  Mat4 result;
  result.values.fill(0);
  for (std::size_t column = 0; column < 4; ++column)
    for (std::size_t row = 0; row < 4; ++row)
      for (std::size_t k = 0; k < 4; ++k)
        result.values[column * 4 + row] += a.values[k * 4 + row] * b.values[column * 4 + k];
  return result;
}

id<MTLComputePipelineState> Pipeline(id<MTLDevice> device, id<MTLLibrary> library,
    NSString* name, NSUInteger threads = 256) {
  auto function = [library newFunctionWithName:name];
  if (!function) Fail(render::RendererErrorCode::BackendFailure, "Missing Gaussian compute entry point");
  NSError* error = nil;
  auto pipeline = [device newComputePipelineStateWithFunction:function error:&error];
  if (!pipeline) Fail(render::RendererErrorCode::BackendFailure, "Metal compute pipeline creation failed");
  if (pipeline.maxTotalThreadsPerThreadgroup < threads)
    Fail(render::RendererErrorCode::Unsupported, "Metal device cannot run the portable radix workgroup");
  return pipeline;
}
} // namespace

GaussianExecution::Scratch::~Scratch() {
  if (budget) budget->live.fetch_sub(bytes, std::memory_order_relaxed);
}

GaussianExecution::GaussianExecution(id<MTLDevice> device, id<MTLLibrary> library,
    std::uint64_t scratch_byte_budget) : device_(device), budget_(std::make_shared<Budget>()) {
  if (!device || !library || library.device != device)
    Fail(render::RendererErrorCode::InvalidRequest, "Expected a library from the execution device");
  budget_->limit = scratch_byte_budget;
  prepare_ = Pipeline(device, library, @"gaussian_prepare_compact", 64);
  keys_ = Pipeline(device, library, @"gaussian_sort_keys");
  histogram_ = Pipeline(device, library, @"gaussian_sort_histogram");
  scan_ = Pipeline(device, library, @"gaussian_sort_scan_blocks");
  add_ = Pipeline(device, library, @"gaussian_sort_scan_add");
  scatter_ = Pipeline(device, library, @"gaussian_sort_scatter");
  verify_ = Pipeline(device, library, @"gaussian_sort_verify");
  gather_ = Pipeline(device, library, @"gaussian_metal_gather");
  try {
    tile_count_ = Pipeline(device, library, @"gaussian_tile_count");
    tile_emit_ = Pipeline(device, library, @"gaussian_tile_emit");
    tile_ranges_ = Pipeline(device, library, @"gaussian_tile_ranges");
    tile_verify_ = Pipeline(device, library, @"gaussian_tile_verify");
    tile_select_ = Pipeline(device, library, @"gaussian_tile_raster_select", 1);
    tile_raster_ = Pipeline(device, library, @"gaussian_tile_raster");
  } catch (const render::RendererError& error) {
    if (error.code() != render::RendererErrorCode::Unsupported &&
        error.code() != render::RendererErrorCode::BackendFailure) throw;
    tile_raster_ = nil;
  }
}

std::uint64_t GaussianExecution::live_bytes() const noexcept {
  return budget_->live.load(std::memory_order_relaxed);
}

void GaussianExecution::Reset() { pool_.clear(); }

std::shared_ptr<GaussianExecution::Scratch> GaussianExecution::Acquire(
    std::uint32_t particles, std::uint32_t resources, std::uint32_t pairs,
    std::uint32_t tile_words, std::uint64_t& allocations) {
  for (const auto& scratch : pool_) {
    // Both the caller's Frame and the completion handler hold a lease. Neither
    // an unresolved GPU consumer nor a retained output can be overwritten.
    if (scratch.use_count() == 1 && scratch->particle_capacity >= particles &&
        scratch->resource_capacity >= resources && scratch->pair_capacity >= pairs &&
        scratch->tile_control_words >= tile_words) return scratch;
  }
  // Idle undersized buffers have no consumers; retire them before growing.
  std::erase_if(pool_, [](const auto& scratch) { return scratch.use_count() == 1; });
  auto scratch = std::make_shared<Scratch>();
  scratch->budget = budget_;
  scratch->particle_capacity = particles;
  scratch->resource_capacity = resources;
  scratch->pair_capacity = pairs;
  scratch->tile_control_words = tile_words;
  const auto allocate = [&](std::uint64_t bytes) {
    bytes = std::max(bytes, std::uint64_t{16});
    if (bytes > std::numeric_limits<std::uint32_t>::max() || bytes > device_.maxBufferLength)
      Fail(render::RendererErrorCode::Unsupported, "Gaussian scratch exceeds the Metal buffer or shader address limit");
    const auto live = live_bytes();
    if (live > budget_->limit || bytes > budget_->limit - live)
      Fail(render::RendererErrorCode::ResourceExhausted, "Gaussian scratch exceeds the live-byte budget");
    auto buffer = [device_ newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
    if (!buffer) Fail(render::RendererErrorCode::BackendFailure, "Metal scratch buffer allocation failed");
    budget_->live.fetch_add(buffer.length, std::memory_order_relaxed);
    scratch->bytes += buffer.length;
    ++allocations;
    return buffer;
  };
  scratch->prepared = allocate(std::uint64_t{particles} * sizeof(PreparedRecord));
  for (auto& buffer : scratch->sort) buffer = allocate(std::uint64_t{particles} * sizeof(SortElement));
  // Histogram has 256 bins per 256-element group; all scan levels together
  // require less than another particle-capacity words (plus the final sum).
  scratch->control = allocate((4ULL + resources + 2ULL * particles + 16) * sizeof(std::uint32_t));
  scratch->counters = allocate(std::uint64_t{resources} * sizeof(PrepareCounters));
  scratch->classifications = allocate(std::uint64_t{particles} * sizeof(std::uint32_t));
  scratch->instances = allocate((std::uint64_t{particles} + 1) * sizeof(GaussianInstance));
  scratch->draw = allocate(sizeof(MTLDrawPrimitivesIndirectArguments));
  if (pairs) {
    scratch->tile_sorted = allocate(std::uint64_t{particles} * sizeof(PreparedRecord));
    for (auto& buffer : scratch->tile_pairs)
      buffer = allocate(std::uint64_t{pairs} * sizeof(SortElement));
    scratch->tile_control = allocate(std::uint64_t{tile_words} * sizeof(std::uint32_t));
  }
  pool_.push_back(scratch);
  return scratch;
}

std::shared_ptr<const GaussianExecution::Frame> GaussianExecution::Encode(
    const std::shared_ptr<GaussianResidency::Update>& attributes,
    const Mat4& view, const Mat4& projection, std::uint32_t width, std::uint32_t height,
    id<MTLCommandBuffer> command, bool device_count, bool validate,
    id<MTLCounterSampleBuffer> timestamps, bool tiled,
    std::uint32_t requested_pair_capacity) {
  if (!attributes || !width || !height || !command || command.device != device_ ||
      command.status != MTLCommandBufferStatusNotEnqueued || !command.retainedReferences ||
      (device_count && attributes->resources.size() != 1))
    Fail(render::RendererErrorCode::InvalidRequest, "Invalid Gaussian execution frame or command");
  auto frame = std::make_shared<Frame>();
  std::uint64_t total = 0;
  extraction::FrameSnapshot metadata;
  for (const auto& resource : attributes->resources) {
    if (!resource.record.positions || !resource.positions || !resource.covariances ||
        !resource.opacities || !resource.radiance || resource.positions->metal.device != device_ ||
        resource.covariances->metal.device != device_ || resource.opacities->metal.device != device_ ||
        resource.radiance->metal.device != device_)
      Fail(render::RendererErrorCode::InvalidRequest, "Invalid Gaussian resident resource");
    total += resource.record.positions->size();
    metadata.gaussians.push_back(resource.record);
  }
  // Prepared records use uint byte offsets. Check before padding/narrowing or
  // allocation; this also bounds every radix and gather index calculation.
  const auto padded = std::max(std::uint64_t{256}, ((total + 255) / 256) * 256);
  if (padded > std::numeric_limits<std::uint32_t>::max() / sizeof(PreparedRecord) ||
      attributes->resources.size() > std::numeric_limits<std::uint32_t>::max() / sizeof(PrepareCounters))
    Fail(render::RendererErrorCode::Unsupported, "Gaussian frame exceeds the shader address limit");
  frame->particle_count = static_cast<std::uint32_t>(total);
  frame->padded_count = static_cast<std::uint32_t>(padded);
  frame->resource_count = static_cast<std::uint32_t>(attributes->resources.size());
  frame->sorting_policy = extraction::SelectGaussianSortingPolicy(metadata);
  TileConstants tile{};
  std::uint64_t tile_words = 0;
  if (tiled) {
    if (!tile_raster_) Fail(render::RendererErrorCode::Unsupported, "Metal tile workgroup is unsupported");
    tile.record_bound = frame->padded_count;
    tile.tile_count_x = Groups(width, 16);
    tile.tile_count_y = Groups(height, 16);
    const auto tile_count = std::uint64_t{tile.tile_count_x} * tile.tile_count_y;
    const auto capacity = requested_pair_capacity
        ? ((std::uint64_t{requested_pair_capacity} + 255) / 256) * 256
        : std::max<std::uint64_t>(65536, std::uint64_t{frame->padded_count} * 8);
    if (!tile_count || tile_count > std::numeric_limits<std::uint32_t>::max() / 2 ||
        capacity > std::min<std::uint64_t>(device_.maxBufferLength / sizeof(SortElement),
            std::numeric_limits<std::uint32_t>::max() / sizeof(SortElement)))
      Fail(render::RendererErrorCode::Unsupported, "Metal tile grid or pair capacity exceeds shader addressing");
    tile.pair_capacity = static_cast<std::uint32_t>(capacity);
    tile.viewport_width = width;
    tile.viewport_height = height;
    tile.offsets_offset = 11;
    const auto scan_end = [](std::uint64_t offset, std::uint64_t count) {
      for (;;) {
        offset += count;
        count = (count + 1023) / 1024;
        if (count == 1) return offset + 1;
      }
    };
    const auto histogram = scan_end(tile.offsets_offset, tile.record_bound);
    const auto ranges = scan_end(histogram, tile.pair_capacity);
    tile_words = ranges + 2 * tile_count;
    if (tile_words > std::min<std::uint64_t>(device_.maxBufferLength / 4,
            std::numeric_limits<std::uint32_t>::max() / 4))
      Fail(render::RendererErrorCode::Unsupported, "Metal tile scan exceeds shader addressing");
    tile.ranges_offset = static_cast<std::uint32_t>(ranges);
    tile.record_pair_limit = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        tile_count, std::numeric_limits<std::uint32_t>::max() / tile.record_bound));
    frame->tile_raster = {tile.tile_count_x, tile.tile_count_y, width, height,
        tile.ranges_offset, tile.pair_capacity, {}};
  }
  auto scratch = Acquire(frame->padded_count, frame->resource_count, tile.pair_capacity,
      static_cast<std::uint32_t>(tile_words), frame->allocation_count);
  frame->tiled = tiled;
  frame->scratch = scratch;
  // Install the lease before the first encoder: an exception must not make
  // already encoded scratch available to another frame. Discard failed commands.
  __block auto retained = frame;
  __block auto retained_attributes = attributes;
  [command addCompletedHandler:^(id<MTLCommandBuffer>) {
    retained.reset();
    retained_attributes.reset();
  }];
  id<MTLBlitCommandEncoder> clear = nil;
  if (timestamps) {
    auto pass = [MTLBlitPassDescriptor blitPassDescriptor];
    pass.sampleBufferAttachments[0].sampleBuffer = timestamps;
    pass.sampleBufferAttachments[0].startOfEncoderSampleIndex = kGaussianPrepareBegin;
    clear = [command blitCommandEncoderWithDescriptor:pass];
  } else {
    clear = [command blitCommandEncoder];
  }
  if (!clear) Fail(render::RendererErrorCode::BackendFailure, "Metal scratch clear encoder allocation failed");
  [clear fillBuffer:scratch->control range:NSMakeRange(0, scratch->control.length) value:0];
  [clear fillBuffer:scratch->counters range:NSMakeRange(0, scratch->counters.length) value:0];
  if (tiled)
    [clear fillBuffer:scratch->tile_control range:NSMakeRange(0, scratch->tile_control.length) value:0];
  if (validate) {
    [clear fillBuffer:scratch->instances range:NSMakeRange(0, scratch->instances.length) value:0xA5];
    [clear fillBuffer:scratch->draw range:NSMakeRange(0, scratch->draw.length) value:0xA5];
  }
  [clear endEncoding];
  auto source = scratch->sort[0];
  auto destination = scratch->sort[1];
  auto scan_buffer = scratch->control;
  auto record_buffer = scratch->prepared;
  const auto sort_dispatch = [&](id<MTLComputePipelineState> pipeline,
                                 const SortConstants& constants, std::uint32_t groups,
                                 NSUInteger start_sample = MTLCounterDontSample,
                                 NSUInteger end_sample = MTLCounterDontSample) {
    id<MTLComputeCommandEncoder> encoder = nil;
    if (timestamps && (start_sample != MTLCounterDontSample || end_sample != MTLCounterDontSample)) {
      auto pass = [MTLComputePassDescriptor computePassDescriptor];
      pass.sampleBufferAttachments[0].sampleBuffer = timestamps;
      pass.sampleBufferAttachments[0].startOfEncoderSampleIndex = start_sample;
      pass.sampleBufferAttachments[0].endOfEncoderSampleIndex = end_sample;
      encoder = [command computeCommandEncoderWithDescriptor:pass];
    } else {
      encoder = [command computeCommandEncoder];
    }
    if (!encoder) Fail(render::RendererErrorCode::BackendFailure, "Metal sort encoder allocation failed");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:source offset:0 atIndex:0];
    [encoder setBuffer:destination offset:0 atIndex:1];
    [encoder setBuffer:scan_buffer offset:0 atIndex:2];
    [encoder setBuffer:record_buffer offset:0 atIndex:3];
    [encoder setBytes:&constants length:sizeof(constants) atIndex:4];
    [encoder dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    [encoder endEncoding];
    ++frame->dispatch_count;
  };
  std::uint32_t base = 0;
  for (std::uint32_t i = 0; i < frame->resource_count; ++i) {
    const auto& resident = attributes->resources[i];
    const auto& record = resident.record;
    const auto count = static_cast<std::uint32_t>(record.positions->size());
    if (count && record.visible) {
      PrepareConstants constants;
      constants.local_to_camera = Multiply(view, record.transform);
      constants.projection = projection;
      constants.viewport_size = {static_cast<float>(width), static_cast<float>(height)};
      constants.resource_id_low = static_cast<std::uint32_t>(record.gaussian);
      constants.resource_id_high = static_cast<std::uint32_t>(record.gaussian >> 32U);
      constants.particle_count = count;
      constants.spherical_harmonics_degree = record.spherical_harmonics_degree;
      constants.coefficients_per_particle = (record.spherical_harmonics_degree + 1U) *
          (record.spherical_harmonics_degree + 1U);
      constants.projection_mode = static_cast<std::uint32_t>(record.projection_mode);
      constants.sorting_mode = static_cast<std::uint32_t>(frame->sorting_policy.mode);
      auto encoder = [command computeCommandEncoder];
      if (!encoder) Fail(render::RendererErrorCode::BackendFailure, "Metal preparation encoder allocation failed");
      [encoder setComputePipelineState:prepare_];
      [encoder setBuffer:resident.positions->metal offset:0 atIndex:0];
      [encoder setBuffer:resident.covariances->metal offset:0 atIndex:1];
      [encoder setBuffer:resident.opacities->metal offset:0 atIndex:2];
      [encoder setBuffer:resident.radiance->metal offset:0 atIndex:3];
      [encoder setBuffer:scratch->classifications offset:base * sizeof(std::uint32_t) atIndex:4];
      [encoder setBuffer:scratch->prepared offset:base * sizeof(PreparedRecord) atIndex:5];
      [encoder setBuffer:scratch->counters offset:i * sizeof(PrepareCounters) atIndex:6];
      [encoder setBytes:&constants length:sizeof(constants) atIndex:7];
      [encoder dispatchThreadgroups:MTLSizeMake(Groups(count, 64), 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
      [encoder endEncoding];
      ++frame->dispatch_count;
    }
    auto blit = [command blitCommandEncoder];
    if (!blit) Fail(render::RendererErrorCode::BackendFailure, "Metal count copy encoder allocation failed");
    [blit copyFromBuffer:scratch->counters sourceOffset:i * sizeof(PrepareCounters) + offsetof(PrepareCounters, visible_count)
        toBuffer:scratch->control destinationOffset:(4ULL + i) * sizeof(std::uint32_t) size:sizeof(std::uint32_t)];
    [blit endEncoding];
    SortConstants keys;
    keys.element_count = i + 1 == frame->resource_count ? frame->padded_count - base : count;
    keys.candidate_base = keys.prepared_base = base;
    keys.visible_count_offset = 4 + i;
    if (keys.element_count) sort_dispatch(keys_, keys, Groups(keys.element_count, 256),
        MTLCounterDontSample, i + 1 == frame->resource_count ? kGaussianPrepareEnd : MTLCounterDontSample);
    base += count;
  }
  if (!frame->resource_count) {
    SortConstants keys;
    keys.element_count = frame->padded_count;
    sort_dispatch(keys_, keys, Groups(keys.element_count, 256), MTLCounterDontSample, kGaussianPrepareEnd);
  }
  std::swap(source, destination);
  for (std::uint32_t digit = 0; digit < 8; ++digit) {
    SortConstants constants;
    constants.element_count = frame->padded_count;
    constants.block_count = Groups(frame->padded_count, 256);
    constants.digit_word = digit / 4;
    constants.digit_shift = (digit % 4) * 8;
    constants.scan_offset = 4 + frame->resource_count;
    constants.flags = device_count ? 1U : 0U;
    constants.count_word = 4;
    sort_dispatch(histogram_, constants, constants.block_count,
        digit == 0 ? kGaussianSortBegin : MTLCounterDontSample);
    std::vector<SortConstants> levels;
    auto level = constants;
    level.scan_count = frame->padded_count;
    for (;;) {
      level.scan_sums_offset = level.scan_offset + level.scan_count;
      const auto groups = Groups(level.scan_count, 1024);
      sort_dispatch(scan_, level, groups);
      levels.push_back(level);
      if (groups == 1) break;
      level.scan_offset = level.scan_sums_offset;
      level.scan_count = groups;
    }
    for (std::size_t i = levels.size() - 1; i > 0; --i)
      sort_dispatch(add_, levels[i - 1], Groups(levels[i - 1].scan_count, 1024));
    sort_dispatch(scatter_, constants, constants.block_count);
    std::swap(source, destination);
  }
  if (validate) {
    // The final stream retains its sentinel tail even when only its GPU count
    // participated in sorting. Verification therefore needs no CPU count.
    SortConstants verify;
    verify.element_count = frame->padded_count;
    sort_dispatch(verify_, verify, Groups(verify.element_count, 256));
  }
  GatherConstants gather;
  gather.element_count = frame->particle_count ? frame->padded_count : 0;
  gather.flags = device_count ? 1U : 0U;
  if (tiled) gather.flags |= 2U;
  gather.count_word = 4;
  id<MTLComputeCommandEncoder> encoder = nil;
  if (timestamps) {
    auto pass = [MTLComputePassDescriptor computePassDescriptor];
    pass.sampleBufferAttachments[0].sampleBuffer = timestamps;
    pass.sampleBufferAttachments[0].endOfEncoderSampleIndex = kGaussianSortEnd;
    encoder = [command computeCommandEncoderWithDescriptor:pass];
  } else {
    encoder = [command computeCommandEncoder];
  }
  if (!encoder) Fail(render::RendererErrorCode::BackendFailure, "Metal gather encoder allocation failed");
  [encoder setComputePipelineState:gather_];
  [encoder setBuffer:source offset:0 atIndex:0];
  [encoder setBuffer:scratch->prepared offset:0 atIndex:1];
  [encoder setBuffer:scratch->instances offset:0 atIndex:2];
  [encoder setBuffer:scratch->draw offset:0 atIndex:3];
  [encoder setBytes:&gather length:sizeof(gather) atIndex:4];
  [encoder setBuffer:scratch->control offset:0 atIndex:5];
  // Metal validation requires every reflected argument to be bound even when
  // the shader's tiled branch is disabled for this dispatch.
  [encoder setBuffer:tiled ? scratch->tile_sorted : scratch->prepared offset:0 atIndex:6];
  [encoder dispatchThreadgroups:MTLSizeMake(Groups(frame->padded_count, 256), 1, 1)
          threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
  [encoder endEncoding];
  ++frame->dispatch_count;
  if (tiled) {
    auto blit = [command blitCommandEncoder];
    if (!blit) Fail(render::RendererErrorCode::BackendFailure, "Metal tile count copy encoder allocation failed");
    [blit copyFromBuffer:scratch->draw sourceOffset:sizeof(std::uint32_t)
        toBuffer:scratch->tile_control destinationOffset:0 size:sizeof(std::uint32_t)];
    [blit endEncoding];
    source = scratch->tile_pairs[0];
    destination = scratch->tile_pairs[1];
    scan_buffer = scratch->tile_control;
    record_buffer = scratch->tile_sorted;
    const auto tile_dispatch = [&](id<MTLComputePipelineState> pipeline, std::uint32_t groups) {
      id<MTLComputeCommandEncoder> pass = nil;
      if (timestamps && pipeline == tile_count_) {
        auto descriptor = [MTLComputePassDescriptor computePassDescriptor];
        descriptor.sampleBufferAttachments[0].sampleBuffer = timestamps;
        descriptor.sampleBufferAttachments[0].startOfEncoderSampleIndex = kGaussianTileBegin;
        pass = [command computeCommandEncoderWithDescriptor:descriptor];
      } else {
        pass = [command computeCommandEncoder];
      }
      if (!pass) Fail(render::RendererErrorCode::BackendFailure, "Metal tile encoder allocation failed");
      [pass setComputePipelineState:pipeline];
      [pass setBuffer:source offset:0 atIndex:0];
      [pass setBuffer:destination offset:0 atIndex:1];
      [pass setBuffer:scan_buffer offset:0 atIndex:2];
      [pass setBuffer:record_buffer offset:0 atIndex:3];
      [pass setBytes:&tile length:sizeof(tile) atIndex:4];
      [pass dispatchThreadgroups:MTLSizeMake(groups, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
      [pass endEncoding];
      ++frame->dispatch_count;
    };
    const auto scan = [&](std::uint32_t offset, std::uint32_t count) {
      std::vector<SortConstants> levels;
      SortConstants level;
      level.scan_offset = offset;
      level.scan_count = count;
      for (;;) {
        level.scan_sums_offset = level.scan_offset + level.scan_count;
        const auto groups = Groups(level.scan_count, 1024);
        sort_dispatch(scan_, level, groups);
        levels.push_back(level);
        if (groups == 1) break;
        level.scan_offset = level.scan_sums_offset;
        level.scan_count = groups;
      }
      for (std::size_t i = levels.size() - 1; i > 0; --i)
        sort_dispatch(add_, levels[i - 1], Groups(levels[i - 1].scan_count, 1024));
      return levels.back().scan_sums_offset + 1;
    };
    tile_dispatch(tile_count_, Groups(tile.record_bound, 256));
    const auto histogram = scan(tile.offsets_offset, tile.record_bound);
    tile_dispatch(tile_emit_, Groups(tile.record_bound, 256));
    std::swap(source, destination);
    const auto passes = tile.tile_count_x * std::uint64_t{tile.tile_count_y} <= 1
        ? 0U : (std::bit_width(tile.tile_count_x * std::uint64_t{tile.tile_count_y} - 1) + 7) / 8;
    for (std::uint32_t digit = 0; digit < passes; ++digit) {
      SortConstants constants;
      constants.element_count = tile.pair_capacity;
      constants.block_count = Groups(tile.pair_capacity, 256);
      constants.digit_shift = digit * 8;
      constants.scan_offset = histogram;
      constants.count_word = 1;
      constants.flags = 1;
      sort_dispatch(histogram_, constants, constants.block_count);
      scan(histogram, tile.pair_capacity);
      sort_dispatch(scatter_, constants, constants.block_count);
      std::swap(source, destination);
    }
    tile_dispatch(tile_ranges_, Groups(tile.pair_capacity, 256));
    if (validate) tile_dispatch(tile_verify_, Groups(tile.pair_capacity, 256));
    frame->grouped_pairs = source;
    id<MTLComputeCommandEncoder> select = nil;
    if (timestamps) {
      auto descriptor = [MTLComputePassDescriptor computePassDescriptor];
      descriptor.sampleBufferAttachments[0].sampleBuffer = timestamps;
      descriptor.sampleBufferAttachments[0].endOfEncoderSampleIndex = kGaussianTileEnd;
      select = [command computeCommandEncoderWithDescriptor:descriptor];
    } else {
      select = [command computeCommandEncoder];
    }
    if (!select) Fail(render::RendererErrorCode::BackendFailure, "Metal tile selection encoder allocation failed");
    [select setComputePipelineState:tile_select_];
    [select setBuffer:source offset:0 atIndex:0];
    [select setBuffer:scratch->tile_control offset:0 atIndex:1];
    [select setBuffer:scratch->tile_sorted offset:0 atIndex:2];
    [select setBuffer:scratch->draw offset:0 atIndex:3];
    [select setBytes:&frame->tile_raster length:sizeof(frame->tile_raster) atIndex:8];
    [select dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [select endEncoding];
    ++frame->dispatch_count;
  }
  return frame;
}

void GaussianExecution::EncodeTileRaster(const Frame& frame, id<MTLCommandBuffer> command,
    id<MTLTexture> color, id<MTLTexture> depth, id<MTLTexture> prim_id,
    id<MTLTexture> instance_id, id<MTLCounterSampleBuffer> timestamps) const {
  if (!frame.tiled) return;
  id<MTLComputeCommandEncoder> encoder = nil;
  if (timestamps) {
    auto descriptor = [MTLComputePassDescriptor computePassDescriptor];
    descriptor.sampleBufferAttachments[0].sampleBuffer = timestamps;
    descriptor.sampleBufferAttachments[0].startOfEncoderSampleIndex = kGaussianTileRasterBegin;
    descriptor.sampleBufferAttachments[0].endOfEncoderSampleIndex = kGaussianTileRasterEnd;
    encoder = [command computeCommandEncoderWithDescriptor:descriptor];
  } else {
    encoder = [command computeCommandEncoder];
  }
  if (!encoder) Fail(render::RendererErrorCode::BackendFailure, "Metal tile raster encoder allocation failed");
  [encoder setComputePipelineState:tile_raster_];
  [encoder setBuffer:frame.grouped_pairs offset:0 atIndex:0];
  [encoder setBuffer:frame.scratch->tile_control offset:0 atIndex:1];
  [encoder setBuffer:frame.scratch->tile_sorted offset:0 atIndex:2];
  [encoder setBuffer:frame.scratch->draw offset:0 atIndex:3];
  [encoder setTexture:color atIndex:0];
  [encoder setTexture:prim_id atIndex:1];
  [encoder setTexture:instance_id atIndex:2];
  [encoder setTexture:depth atIndex:7];
  [encoder setBytes:&frame.tile_raster length:sizeof(frame.tile_raster) atIndex:8];
  [encoder dispatchThreadgroups:MTLSizeMake(frame.tile_raster.tile_count_x,
      frame.tile_raster.tile_count_y, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
  [encoder endEncoding];
}

} // namespace merlin::metal
