#include "../backend/merlin-metal/src/gaussian_residency.hpp"
#include <merlin/metal/backend.hpp>

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {
using merlin::metal::GaussianResidency;
using merlin::extraction::FrameSnapshot;
using merlin::extraction::GaussianRecord;

void Require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

template <typename Function>
void Reject(Function function, merlin::render::RendererErrorCode code) {
  try {
    function();
  } catch (const merlin::render::RendererError& error) {
    Require(error.code() == code, "Wrong residency error code");
    return;
  }
  throw std::runtime_error("Expected residency rejection");
}

FrameSnapshot Fixture() {
  FrameSnapshot snapshot;
  snapshot.source_id = 7;
  snapshot.revision = 1;
  GaussianRecord record;
  record.gaussian = 0x100000002ULL;
  record.revision = record.positions_revision = record.covariance_revision =
      record.opacity_revision = record.radiance_revision = 1;
  record.positions = std::make_shared<const std::vector<merlin::Vec3>>(8, merlin::Vec3{1, 2, 3});
  record.covariances = std::make_shared<const std::vector<merlin::Covariance3>>(
      8, merlin::Covariance3{1, 0, 0, 1, 0, 1});
  record.opacities = std::make_shared<const std::vector<float>>(8, 0.5F);
  record.spherical_harmonics_degree = 3;
  record.spherical_harmonics_coefficients = std::make_shared<const std::vector<merlin::Vec3>>(
      8 * 16, merlin::Vec3{0.1F, 0.2F, 0.3F});
  snapshot.gaussians.assign({record});
  return snapshot;
}

struct Readback {
  id<MTLBuffer> buffer;
  const void* expected;
  std::size_t bytes;
};

std::vector<Readback> Read(id<MTLDevice> device, id<MTLCommandBuffer> command,
    const GaussianResidency::Resource& resource) {
  std::vector<Readback> reads;
  auto encoder = [command blitCommandEncoder];
  const auto read = [&](const auto& payload, const auto& source) {
    using Element = typename std::decay_t<decltype(*payload)>::value_type;
    const auto bytes = payload->size() * sizeof(Element);
    if (!bytes) return;
    auto buffer = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    Require(buffer != nil, "Readback allocation failed");
    [encoder copyFromBuffer:source->metal sourceOffset:0 toBuffer:buffer destinationOffset:0 size:bytes];
    reads.push_back({buffer, payload->data(), bytes});
  };
  read(resource.record.positions, resource.positions);
  read(resource.record.covariances, resource.covariances);
  read(resource.record.opacities, resource.opacities);
  read(resource.record.spherical_harmonics_coefficients, resource.radiance);
  [encoder endEncoding];
  return reads;
}

void Complete(id<MTLCommandBuffer> command, const std::vector<Readback>& reads) {
  [command waitUntilCompleted];
  Require(command.status == MTLCommandBufferStatusCompleted, "Attribute command failed");
  for (const auto& read : reads)
    Require(std::memcmp(read.buffer.contents, read.expected, read.bytes) == 0,
        "Resident attribute bytes differ from snapshot");
}

void TestResidency(id<MTLDevice> device, id<MTLCommandQueue> queue) {
  auto initial = Fixture();
  const auto full_bytes = 8 * (sizeof(merlin::Vec3) + sizeof(merlin::Covariance3) +
      sizeof(float) + 16 * sizeof(merlin::Vec3));
  GaussianResidency store(device, full_bytes * 4);
  auto first = store.Prepare(initial);
  Require(first->upload_bytes == full_bytes && first->upload_range_count == 4 &&
      first->device_copy_bytes == 0 && first->allocation_count == 8, "Wrong initial upload accounting");
  Reject([&] { store.Commit(first); }, merlin::render::RendererErrorCode::InvalidRequest);
  auto gate = [device newSharedEvent];
  Require(gate != nil, "Shared event allocation failed");
  struct ReleaseGate {
    id<MTLSharedEvent> event;
    ~ReleaseGate() { event.signaledValue = 1; }
  } release_gate{gate};
  auto first_command = [queue commandBuffer];
  [first_command encodeWaitForEvent:gate value:1];
  store.Encode(first, first_command);
  const auto first_reads = Read(device, first_command, first->resources[0]);
  [first_command commit];
  store.Commit(first);

  auto camera = initial;
  camera.revision = 2;
  camera.view.values[12] = 0.5F;
  auto unchanged = store.Prepare(camera);
  Require(unchanged->upload_bytes == 0 && unchanged->allocation_count == 0 &&
      unchanged->resources[0].positions == first->resources[0].positions,
      "Camera change reuploaded resident attributes");
  auto metadata = initial.gaussians[0];
  metadata.revision = 2;
  metadata.transform.values[12] = 1;
  metadata.visible = false;
  metadata.sorting_mode = merlin::GaussianSortingMode::CameraDistance;
  camera.gaussians.assign({metadata});
  auto metadata_update = store.Prepare(camera);
  Require(metadata_update->upload_bytes == 0 && metadata_update->allocation_count == 0 &&
      !metadata_update->resources[0].record.visible &&
      metadata_update->resources[0].record.transform.values[12] == 1,
      "Metadata edit changed attributes or lost metadata");

  auto edited = initial;
  auto record = initial.gaussians[0];
  record.revision = 2;
  record.positions_revision = record.radiance_revision = 2;
  record.particle_base_revision = 1;
  record.particle_ranges = {{1, 2}, {6, 1}};
  auto positions = std::make_shared<std::vector<merlin::Vec3>>(*record.positions);
  auto radiance = std::make_shared<std::vector<merlin::Vec3>>(*record.spherical_harmonics_coefficients);
  for (const auto& range : record.particle_ranges) {
    for (std::size_t i = range.first; i < range.first + range.count; ++i) {
      (*positions)[i] = {4, 5, 6};
      for (std::size_t j = 0; j < 16; ++j) (*radiance)[i * 16 + j] = {0.4F, 0.5F, 0.6F};
    }
  }
  record.positions = positions;
  record.spherical_harmonics_coefficients = radiance;
  edited.gaussians.assign({record});
  auto second = store.Prepare(edited);
  Require(second->upload_bytes == 3 * 17 * sizeof(merlin::Vec3) && second->upload_range_count == 4 &&
      second->device_copy_bytes == 8 * 17 * sizeof(merlin::Vec3), "Wrong partial upload accounting");
  Require(second->resources[0].positions != first->resources[0].positions &&
      second->resources[0].radiance != first->resources[0].radiance &&
      second->resources[0].covariances == first->resources[0].covariances &&
      second->resources[0].opacities == first->resources[0].opacities,
      "Partial update did not version only changed attributes");
  auto second_command = [queue commandBuffer];
  store.Encode(second, second_command);
  const auto second_reads = Read(device, second_command, second->resources[0]);
  // Read the old version AFTER the new upload on the GPU as well.
  const auto old_reads = Read(device, second_command, first->resources[0]);
  [second_command commit];
  store.Commit(second);
  Reject([&] { store.Encode(unchanged, [queue commandBuffer]); },
      merlin::render::RendererErrorCode::InvalidRequest);
  Reject([&] { store.Commit(second); }, merlin::render::RendererErrorCode::InvalidRequest);

  const auto retained_bytes = store.live_bytes();
  auto replacement = edited;
  replacement.source_id = 8;
  Reject([&] { store.Prepare(replacement); }, merlin::render::RendererErrorCode::ResourceExhausted);
  Require(store.live_bytes() == retained_bytes && store.Prepare(edited)->upload_bytes == 0,
      "In-flight budget rejection corrupted residency or leaked allocations");

  // Release every CPU owner while both submissions are blocked. Command
  // completion owns staging, old versions and their live budget accounting.
  first.reset();
  second.reset();
  unchanged.reset();
  metadata_update.reset();
  store.Reset();
  Require(store.live_bytes() > full_bytes, "In-flight versions escaped live allocation accounting");
  gate.signaledValue = 1;
  Complete(first_command, first_reads);
  Complete(second_command, second_reads);
  Complete(second_command, old_reads);
}

void TestReconciliation(id<MTLDevice> device, id<MTLCommandQueue> queue) {
  auto snapshot = Fixture();
  GaussianResidency store(device, 1024 * 1024);
  auto initial = store.Prepare(snapshot);
  auto command = [queue commandBuffer];
  store.Encode(initial, command);
  [command commit];
  store.Commit(initial);
  Complete(command, {});
  const auto full_bytes = initial->upload_bytes;

  auto gap = snapshot;
  auto record = snapshot.gaussians[0];
  record.revision = record.opacity_revision = 4;
  record.particle_base_revision = 3;
  record.particle_ranges = {{1, 1}};
  record.opacities = std::make_shared<const std::vector<float>>(8, 0.75F);
  gap.gaussians.assign({record});
  auto update = store.Prepare(gap);
  Require(update->upload_bytes == 8 * sizeof(float) && update->device_copy_bytes == 0,
      "Revision gap incorrectly applied only dirty ranges");
  command = [queue commandBuffer];
  store.Encode(update, command);
  const auto reads = Read(device, command, update->resources[0]);
  [command commit];
  store.Commit(update);
  Complete(command, reads);

  auto foreign = gap;
  foreign.source_id = 8;
  Require(store.Prepare(foreign)->upload_bytes == full_bytes, "Foreign source reused resource versions");
  auto reused = gap;
  record.gaussian += 1ULL << 32;
  reused.gaussians.assign({record});
  Require(store.Prepare(reused)->upload_bytes == full_bytes, "Handle generation reused removed attributes");

  // Aborted preparation cannot replace committed residency.
  auto aborted = store.Prepare(snapshot);
  aborted.reset();
  Require(store.Prepare(gap)->upload_bytes == 0, "Aborted plan changed residency");
  GaussianResidency other(device, 1024 * 1024);
  Reject([&] { other.Encode(store.Prepare(gap), [queue commandBuffer]); },
      merlin::render::RendererErrorCode::InvalidRequest);

  auto invalid = gap;
  record = gap.gaussians[0];
  record.particle_ranges = {{8, 1}};
  invalid.gaussians.assign({record});
  Reject([&] { store.Prepare(invalid); }, merlin::render::RendererErrorCode::InvalidRequest);
  record = gap.gaussians[0];
  record.spherical_harmonics_degree = 4;
  invalid.gaussians.assign({record});
  Reject([&] { store.Prepare(invalid); }, merlin::render::RendererErrorCode::InvalidRequest);
  record = gap.gaussians[0];
  record.opacities.reset();
  invalid.gaussians.assign({record});
  Reject([&] { store.Prepare(invalid); }, merlin::render::RendererErrorCode::InvalidRequest);
  invalid.gaussians.assign({gap.gaussians[0], gap.gaussians[0]});
  Reject([&] { store.Prepare(invalid); }, merlin::render::RendererErrorCode::InvalidRequest);
  Require(store.Prepare(gap)->upload_bytes == 0, "Invalid plan changed residency");

  // Count and SH layout changes must upload complete replacement payloads,
  // even when a caller supplies a matching base revision and a small range.
  auto resized = gap;
  record = gap.gaussians[0];
  record.particle_base_revision = record.revision;
  record.particle_ranges = {{0, 1}};
  record.positions = std::make_shared<const std::vector<merlin::Vec3>>(2);
  record.covariances = std::make_shared<const std::vector<merlin::Covariance3>>(2);
  record.opacities = std::make_shared<const std::vector<float>>(2, 0.5F);
  record.spherical_harmonics_degree = 1;
  record.spherical_harmonics_coefficients = std::make_shared<const std::vector<merlin::Vec3>>(8);
  resized.gaussians.assign({record});
  auto resize = store.Prepare(resized);
  Require(resize->upload_bytes == 2 * (sizeof(merlin::Vec3) + sizeof(merlin::Covariance3) +
      sizeof(float) + 4 * sizeof(merlin::Vec3)) && resize->device_copy_bytes == 0,
      "Count/SH layout change incorrectly applied partial upload");
  command = [queue commandBuffer];
  store.Encode(resize, command);
  const auto resize_reads = Read(device, command, resize->resources[0]);
  [command commit];
  store.Commit(resize);
  Complete(command, resize_reads);

  auto empty = gap;
  empty.gaussians.assign({});
  auto removal = store.Prepare(empty);
  command = [queue commandBuffer];
  store.Encode(removal, command);
  [command commit];
  store.Commit(removal);
  Complete(command, {});
  Require(removal->resources.empty() && removal->upload_bytes == 0, "Removal uploaded attributes");
  Require(store.Prepare(gap)->upload_bytes == full_bytes, "Removed resource reused stale residency");

  // Failure half way through allocations rolls back both state and accounting.
  GaussianResidency bounded(device, 256);
  Reject([&] { bounded.Prepare(snapshot); }, merlin::render::RendererErrorCode::ResourceExhausted);
  Require(bounded.live_bytes() == 0, "Failed transaction leaked its allocation budget");
}
} // namespace

int main() {
  @autoreleasepool {
    try {
      const auto availability = merlin::metal::BackendFactory{}.availability();
      if (!availability.available) {
        std::cerr << "skip: " << availability.detail << '\n';
        return 77;
      }
      auto device = MTLCreateSystemDefaultDevice();
      auto queue = [device newCommandQueue];
      Require(device && queue, "Metal device/queue unavailable");
      TestResidency(device, queue);
      TestReconciliation(device, queue);
      std::cout << "Metal Gaussian attribute residency, partial updates and lifetime passed\n";
      return 0;
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
    }
  }
}
