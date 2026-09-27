#include "gaussian_execution.hpp"
#include "gaussian_raster_abi.hpp"

#include <merlin/render/backend.hpp>

#include <algorithm>
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
}

std::uint64_t GaussianExecution::live_bytes() const noexcept {
  return budget_->live.load(std::memory_order_relaxed);
}

void GaussianExecution::Reset() { pool_.clear(); }

std::shared_ptr<GaussianExecution::Scratch> GaussianExecution::Acquire(
    std::uint32_t particles, std::uint32_t resources, std::uint64_t& allocations) {
  for (const auto& scratch : pool_) {
    // Both the caller's Frame and the completion handler hold a lease. Neither
    // an unresolved GPU consumer nor a retained output can be overwritten.
    if (scratch.use_count() == 1 && scratch->particle_capacity >= particles &&
        scratch->resource_capacity >= resources) return scratch;
  }
  // Idle undersized buffers have no consumers; retire them before growing.
  std::erase_if(pool_, [](const auto& scratch) { return scratch.use_count() == 1; });
  auto scratch = std::make_shared<Scratch>();
  scratch->budget = budget_;
  scratch->particle_capacity = particles;
  scratch->resource_capacity = resources;
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
  pool_.push_back(scratch);
  return scratch;
}

std::shared_ptr<const GaussianExecution::Frame> GaussianExecution::Encode(
    const std::shared_ptr<GaussianResidency::Update>& attributes,
    const Mat4& view, const Mat4& projection, std::uint32_t width, std::uint32_t height,
    id<MTLCommandBuffer> command, bool device_count, bool validate) {
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
  auto scratch = Acquire(frame->padded_count, frame->resource_count, frame->allocation_count);
  frame->scratch = scratch;
  // Install the lease before the first encoder: an exception must not make
  // already encoded scratch available to another frame. Discard failed commands.
  __block auto retained = frame;
  __block auto retained_attributes = attributes;
  [command addCompletedHandler:^(id<MTLCommandBuffer>) {
    retained.reset();
    retained_attributes.reset();
  }];
  auto clear = [command blitCommandEncoder];
  if (!clear) Fail(render::RendererErrorCode::BackendFailure, "Metal scratch clear encoder allocation failed");
  [clear fillBuffer:scratch->control range:NSMakeRange(0, scratch->control.length) value:0];
  [clear fillBuffer:scratch->counters range:NSMakeRange(0, scratch->counters.length) value:0];
  if (validate) {
    [clear fillBuffer:scratch->instances range:NSMakeRange(0, scratch->instances.length) value:0xA5];
    [clear fillBuffer:scratch->draw range:NSMakeRange(0, scratch->draw.length) value:0xA5];
  }
  [clear endEncoding];
  auto source = scratch->sort[0];
  auto destination = scratch->sort[1];
  const auto sort_dispatch = [&](id<MTLComputePipelineState> pipeline,
                                 const SortConstants& constants, std::uint32_t groups) {
    auto encoder = [command computeCommandEncoder];
    if (!encoder) Fail(render::RendererErrorCode::BackendFailure, "Metal sort encoder allocation failed");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:source offset:0 atIndex:0];
    [encoder setBuffer:destination offset:0 atIndex:1];
    [encoder setBuffer:scratch->control offset:0 atIndex:2];
    [encoder setBuffer:scratch->prepared offset:0 atIndex:3];
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
    if (keys.element_count) sort_dispatch(keys_, keys, Groups(keys.element_count, 256));
    base += count;
  }
  if (!frame->resource_count) {
    SortConstants keys;
    keys.element_count = frame->padded_count;
    sort_dispatch(keys_, keys, Groups(keys.element_count, 256));
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
    sort_dispatch(histogram_, constants, constants.block_count);
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
  gather.count_word = 4;
  auto encoder = [command computeCommandEncoder];
  if (!encoder) Fail(render::RendererErrorCode::BackendFailure, "Metal gather encoder allocation failed");
  [encoder setComputePipelineState:gather_];
  [encoder setBuffer:source offset:0 atIndex:0];
  [encoder setBuffer:scratch->prepared offset:0 atIndex:1];
  [encoder setBuffer:scratch->instances offset:0 atIndex:2];
  [encoder setBuffer:scratch->draw offset:0 atIndex:3];
  [encoder setBytes:&gather length:sizeof(gather) atIndex:4];
  [encoder setBuffer:scratch->control offset:0 atIndex:5];
  [encoder dispatchThreadgroups:MTLSizeMake(Groups(frame->padded_count, 256), 1, 1)
          threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
  [encoder endEncoding];
  ++frame->dispatch_count;
  return frame;
}

} // namespace merlin::metal
