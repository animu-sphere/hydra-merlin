#include "gaussian_residency.hpp"

#include <merlin/render/backend.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>
#include <unordered_map>

namespace merlin::metal {
namespace {
[[noreturn]] void Fail(render::RendererErrorCode code, const char* message) {
  throw render::RendererError(code, "synchronize Metal Gaussian attributes", message);
}

void Validate(const extraction::GaussianRecord& record) {
  if (!record.positions || !record.covariances || !record.opacities ||
      !record.spherical_harmonics_coefficients || record.spherical_harmonics_degree > 3 ||
      record.positions->size() > std::numeric_limits<std::uint32_t>::max() ||
      record.positions->size() != record.covariances->size() ||
      record.positions->size() != record.opacities->size()) {
    Fail(render::RendererErrorCode::InvalidRequest, "Gaussian attribute payload is malformed");
  }
  const auto coefficients = (record.spherical_harmonics_degree + 1U) *
                            (record.spherical_harmonics_degree + 1U);
  if (record.spherical_harmonics_coefficients->size() != record.positions->size() * coefficients) {
    Fail(render::RendererErrorCode::InvalidRequest, "Gaussian SH payload size is inconsistent");
  }
  for (const auto& range : record.particle_ranges) {
    if (range.first > record.positions->size() ||
        range.count > record.positions->size() - range.first) {
      Fail(render::RendererErrorCode::InvalidRequest, "Gaussian changed range is out of bounds");
    }
  }
}
} // namespace

GaussianResidency::Buffer::~Buffer() {
  if (metal != nil) budget->live.fetch_sub(metal.length, std::memory_order_relaxed);
}

GaussianResidency::GaussianResidency(id<MTLDevice> device, std::uint64_t byte_budget)
    : device_(device), budget_(std::make_shared<Budget>()) {
  if (!device) Fail(render::RendererErrorCode::InvalidRequest, "Metal device is null");
  budget_->limit = byte_budget;
}

std::uint64_t GaussianResidency::live_bytes() const noexcept {
  return budget_->live.load(std::memory_order_relaxed);
}

GaussianResidency::BufferPtr GaussianResidency::Allocate(
    std::uint64_t bytes, MTLResourceOptions options, Update& update) {
  // Even empty attributes have a legal binding; byte-addressed shaders use
  // 32-bit offsets. Reject before allocation or narrowing to NSUInteger.
  bytes = std::max(bytes, std::uint64_t{16});
  const auto live = live_bytes();
  if (bytes > std::numeric_limits<std::uint32_t>::max() || bytes > device_.maxBufferLength)
    Fail(render::RendererErrorCode::Unsupported, "Gaussian attribute exceeds the Metal buffer or shader address limit");
  if (live > budget_->limit || bytes > budget_->limit - live)
    Fail(render::RendererErrorCode::ResourceExhausted, "Gaussian residency exceeds the live-byte budget");
  auto result = std::make_shared<Buffer>();
  result->budget = budget_;
  result->metal = [device_ newBufferWithLength:bytes options:options];
  if (!result->metal) Fail(render::RendererErrorCode::BackendFailure, "Metal attribute buffer allocation failed");
  budget_->live.fetch_add(result->metal.length, std::memory_order_relaxed);
  ++update.allocation_count;
  return result;
}

std::shared_ptr<GaussianResidency::Update> GaussianResidency::Prepare(
    const extraction::FrameSnapshot& snapshot) {
  auto update = std::make_shared<Update>();
  update->owner = budget_;
  update->epoch = epoch_;
  update->source_id = snapshot.source_id;
  std::unordered_map<std::uint64_t, const Resource*> previous;
  if (snapshot.source_id == source_id_) {
    for (const auto& resource : resident_) previous.emplace(resource.record.gaussian, &resource);
  }
  update->resources.reserve(snapshot.gaussians.size());
  for (const auto& record : snapshot.gaussians) {
    Validate(record);
    const auto found = previous.find(record.gaussian);
    const auto* old = found == previous.end() ? nullptr : found->second;
    const auto coefficients = (record.spherical_harmonics_degree + 1U) *
                              (record.spherical_harmonics_degree + 1U);
    const bool partial = old && snapshot.source_id != 0 &&
        old->record.revision == record.particle_base_revision &&
        old->record.positions->size() == record.positions->size() &&
        old->record.spherical_harmonics_degree == record.spherical_harmonics_degree &&
        std::any_of(record.particle_ranges.begin(), record.particle_ranges.end(),
            [](const auto& range) { return range.count != 0; });
    const auto attribute = [&](const auto& payload, const auto& previous_payload,
                               std::uint64_t revision, std::uint64_t previous_revision,
                               BufferPtr previous_buffer, std::uint32_t multiplier) -> BufferPtr {
      if (previous_buffer && revision == previous_revision && payload == previous_payload)
        return previous_buffer;
      using Element = typename std::decay_t<decltype(*payload)>::value_type;
      const auto bytes = std::uint64_t{payload->size()} * sizeof(Element);
      auto destination = Allocate(bytes, MTLResourceStorageModePrivate, *update);
      const auto stage = [&](std::uint64_t first, std::uint64_t count) {
        const auto size = count * sizeof(Element);
        if (!size) return;
        auto staging = Allocate(size, MTLResourceStorageModeShared, *update);
        std::memcpy(staging->metal.contents, payload->data() + first, size);
        update->copies.push_back({staging, destination, first * sizeof(Element), size});
        update->upload_bytes += size;
        ++update->upload_range_count;
      };
      if (partial && previous_buffer && bytes) {
        // Version on the GPU before patching: never overwrite a buffer that an
        // earlier command may still read, and never copy unchanged data on CPU.
        update->copies.push_back({previous_buffer, destination, 0, bytes});
        update->device_copy_bytes += bytes;
        for (const auto& range : record.particle_ranges)
          stage(std::uint64_t{range.first} * multiplier, std::uint64_t{range.count} * multiplier);
      } else {
        stage(0, payload->size());
      }
      return destination;
    };
    Resource next;
    next.record = record;
    next.positions = attribute(record.positions, old ? old->record.positions : nullptr,
        record.positions_revision, old ? old->record.positions_revision : 0,
        old ? old->positions : nullptr, 1);
    next.covariances = attribute(record.covariances, old ? old->record.covariances : nullptr,
        record.covariance_revision, old ? old->record.covariance_revision : 0,
        old ? old->covariances : nullptr, 1);
    next.opacities = attribute(record.opacities, old ? old->record.opacities : nullptr,
        record.opacity_revision, old ? old->record.opacity_revision : 0,
        old ? old->opacities : nullptr, 1);
    next.radiance = attribute(record.spherical_harmonics_coefficients,
        old ? old->record.spherical_harmonics_coefficients : nullptr,
        record.radiance_revision, old ? old->record.radiance_revision : 0,
        old ? old->radiance : nullptr, coefficients);
    if (!old || next.positions != old->positions || next.covariances != old->covariances ||
        next.opacities != old->opacities || next.radiance != old->radiance)
      ++update->generation_count;
    update->resources.push_back(std::move(next));
  }
  std::sort(update->resources.begin(), update->resources.end(),
      [](const auto& a, const auto& b) { return a.record.gaussian < b.record.gaussian; });
  for (std::size_t i = 1; i < update->resources.size(); ++i) {
    if (update->resources[i - 1].record.gaussian == update->resources[i].record.gaussian)
      Fail(render::RendererErrorCode::InvalidRequest, "Duplicate Gaussian resource handle");
  }
  return update;
}

void GaussianResidency::ValidateUpdate(const std::shared_ptr<Update>& update) const {
  if (!update || update->owner != budget_ || update->epoch != epoch_ || update->committed)
    Fail(render::RendererErrorCode::InvalidRequest, "Stale or foreign Gaussian residency update");
}

void GaussianResidency::Encode(const std::shared_ptr<Update>& update, id<MTLCommandBuffer> command) {
  ValidateUpdate(update);
  if (!command || command.device != device_ || update->encoded ||
      command.status != MTLCommandBufferStatusNotEnqueued || !command.retainedReferences)
    Fail(render::RendererErrorCode::InvalidRequest, "Expected an unsubmitted retaining Metal command buffer");
  if (!update->copies.empty()) {
    auto encoder = [command blitCommandEncoder];
    if (!encoder) Fail(render::RendererErrorCode::BackendFailure, "Metal attribute blit encoder allocation failed");
    for (const auto& copy : update->copies) {
      [encoder copyFromBuffer:copy.source->metal sourceOffset:0
          toBuffer:copy.destination->metal destinationOffset:copy.destination_offset size:copy.bytes];
    }
    [encoder endEncoding];
  }
  // Capturing the plan keeps allocation accounting and old versions alive as
  // long as the GPU uses them; the plan itself does not retain the command.
  __block auto retained = update;
  [command addCompletedHandler:^(id<MTLCommandBuffer>) { retained.reset(); }];
  update->encoded = true;
}

void GaussianResidency::Commit(const std::shared_ptr<Update>& update) {
  ValidateUpdate(update);
  if (!update->encoded) Fail(render::RendererErrorCode::InvalidRequest, "Gaussian update has not been encoded");
  auto resources = update->resources;
  resident_.swap(resources);
  source_id_ = update->source_id;
  update->committed = true;
  ++epoch_;
}

void GaussianResidency::Reset() {
  resident_.clear();
  source_id_ = 0;
  ++epoch_;
}

} // namespace merlin::metal
