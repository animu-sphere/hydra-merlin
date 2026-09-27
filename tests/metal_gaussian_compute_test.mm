#include "../backend/merlin-metal/src/gaussian_compute_abi.hpp"
#include <merlin/extraction/gaussian_preparation.hpp>

#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using namespace merlin::metal::gaussian_compute;
using merlin::extraction::FrameSnapshot;

void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void Near(float actual, float expected, const char* field) {
  // Field tolerance is documented in the Metal Gaussian execution design.
  if (!std::isfinite(actual) ||
      std::abs(actual - expected) > 2.0e-4F * std::max(1.0F, std::abs(expected))) {
    std::cerr << field << " actual=" << actual << " expected=" << expected << '\n';
    throw std::runtime_error("Metal preparation differs from the CPU reference");
  }
}

std::uint32_t Groups(std::uint32_t count, std::uint32_t size) {
  return (count + size - 1) / size;
}

id<MTLBuffer> Buffer(id<MTLDevice> device, std::size_t bytes, const void* data = nullptr) {
  auto buffer = [device newBufferWithLength:std::max(bytes, std::size_t{16})
                                   options:MTLResourceStorageModeShared];
  Require(buffer != nil, "Metal buffer allocation failed");
  std::memset(buffer.contents, 0, buffer.length);
  if (data && bytes) std::memcpy(buffer.contents, data, bytes);
  return buffer;
}

template <typename T>
id<MTLBuffer> Upload(id<MTLDevice> device, const std::vector<T>& values) {
  return Buffer(device, values.size() * sizeof(T), values.data());
}

struct Kernels {
  id<MTLComputePipelineState> prepare;
  id<MTLComputePipelineState> keys;
  id<MTLComputePipelineState> histogram;
  id<MTLComputePipelineState> scan;
  id<MTLComputePipelineState> add;
  id<MTLComputePipelineState> scatter;
  id<MTLComputePipelineState> verify;
};

id<MTLComputePipelineState> Pipeline(id<MTLDevice> device, id<MTLLibrary> library,
    NSString* name) {
  auto function = [library newFunctionWithName:name];
  Require(function != nil, "Missing Metal compute entry point");
  NSError* error = nil;
  auto pipeline = [device newComputePipelineStateWithFunction:function error:&error];
  if (!pipeline) throw std::runtime_error(error.localizedDescription.UTF8String);
  Require(pipeline.maxTotalThreadsPerThreadgroup >= 256,
      "Metal device cannot run the portable radix workgroup");
  return pipeline;
}

// A complete preparation + global sort in one submission. Counts are copied
// device-to-device; no intermediate readback schedules subsequent kernels.
void Compare(id<MTLDevice> device, id<MTLCommandQueue> queue, const Kernels& kernels,
    FrameSnapshot snapshot, bool dynamic_count = false) {
  std::vector<merlin::extraction::GaussianRecord> ordered(snapshot.gaussians.begin(), snapshot.gaussians.end());
  std::sort(ordered.begin(), ordered.end(),
      [](const auto& a, const auto& b) { return a.gaussian < b.gaussian; });
  snapshot.gaussians.assign(std::move(ordered));
  const auto reference = merlin::extraction::PrepareGaussianFrame(snapshot, {320, 192});
  std::uint32_t total = 0;
  for (const auto& record : snapshot.gaussians)
    total += static_cast<std::uint32_t>(record.positions->size());
  const std::uint32_t padded = std::max(256U, Groups(total, 256) * 256);
  auto prepared = Buffer(device, padded * sizeof(PreparedRecord));
  auto source = Buffer(device, padded * sizeof(SortElement));
  auto destination = Buffer(device, padded * sizeof(SortElement));
  // Four verification words, then per-resource counts, then hierarchical scan.
  const auto histogram_offset = 4U + static_cast<std::uint32_t>(snapshot.gaussians.size());
  auto control = Buffer(device, (histogram_offset + padded * 2U + 16U) * sizeof(std::uint32_t));
  auto command = [queue commandBuffer];
  Require(command != nil, "Metal command allocation failed");
  std::vector<id<MTLBuffer>> counters;
  std::vector<id<MTLBuffer>> classifications;
  const auto policy = merlin::extraction::SelectGaussianSortingPolicy(snapshot);
  auto sort_dispatch = [&](id<MTLComputePipelineState> pipeline,
                           const SortConstants& constants, std::uint32_t groups) {
    auto encoder = [command computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:source offset:0 atIndex:0];
    [encoder setBuffer:destination offset:0 atIndex:1];
    [encoder setBuffer:control offset:0 atIndex:2];
    [encoder setBuffer:prepared offset:0 atIndex:3];
    [encoder setBytes:&constants length:sizeof(constants) atIndex:4];
    [encoder dispatchThreadgroups:MTLSizeMake(groups, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    [encoder endEncoding];
  };
  std::uint32_t base = 0;
  for (std::size_t i = 0; i < snapshot.gaussians.size(); ++i) {
    const auto& record = snapshot.gaussians[i];
    const auto count = static_cast<std::uint32_t>(record.positions->size());
    PrepareConstants constants;
    constants.local_to_camera = record.transform; // Fixtures use identity view.
    constants.projection = snapshot.projection;
    constants.viewport_size = {320, 192};
    constants.resource_id_low = static_cast<std::uint32_t>(record.gaussian);
    constants.resource_id_high = static_cast<std::uint32_t>(record.gaussian >> 32U);
    constants.particle_count = count;
    constants.spherical_harmonics_degree = record.spherical_harmonics_degree;
    constants.coefficients_per_particle = (record.spherical_harmonics_degree + 1U) *
        (record.spherical_harmonics_degree + 1U);
    constants.projection_mode = static_cast<std::uint32_t>(record.projection_mode);
    constants.sorting_mode = static_cast<std::uint32_t>(policy.mode);
    counters.push_back(Buffer(device, sizeof(PrepareCounters)));
    classifications.push_back(Buffer(device, count * sizeof(std::uint32_t)));
    if (count && record.visible) {
      auto encoder = [command computeCommandEncoder];
      [encoder setComputePipelineState:kernels.prepare];
      [encoder setBuffer:Upload(device, *record.positions) offset:0 atIndex:0];
      [encoder setBuffer:Upload(device, *record.covariances) offset:0 atIndex:1];
      [encoder setBuffer:Upload(device, *record.opacities) offset:0 atIndex:2];
      [encoder setBuffer:Upload(device, *record.spherical_harmonics_coefficients) offset:0 atIndex:3];
      [encoder setBuffer:classifications.back() offset:0 atIndex:4];
      [encoder setBuffer:prepared offset:base * sizeof(PreparedRecord) atIndex:5];
      [encoder setBuffer:counters.back() offset:0 atIndex:6];
      [encoder setBytes:&constants length:sizeof(constants) atIndex:7];
      [encoder dispatchThreadgroups:MTLSizeMake(Groups(count, 64), 1, 1)
              threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
      [encoder endEncoding];
    }
    auto blit = [command blitCommandEncoder];
    [blit copyFromBuffer:counters.back() sourceOffset:offsetof(PrepareCounters, visible_count)
               toBuffer:control destinationOffset:(4 + i) * sizeof(std::uint32_t)
                   size:sizeof(std::uint32_t)];
    [blit endEncoding];
    SortConstants keys;
    keys.element_count = i + 1 == snapshot.gaussians.size() ? padded - base : count;
    keys.candidate_base = base;
    keys.prepared_base = base;
    keys.visible_count_offset = 4U + static_cast<std::uint32_t>(i);
    if (keys.element_count) sort_dispatch(kernels.keys, keys, Groups(keys.element_count, 256));
    base += count;
  }
  // A resource-free frame still initializes the sentinel stream on the GPU.
  if (snapshot.gaussians.empty()) {
    SortConstants keys;
    keys.element_count = padded;
    sort_dispatch(kernels.keys, keys, Groups(padded, 256));
  }
  std::swap(source, destination);
  for (std::uint32_t digit = 0; digit < 8; ++digit) {
    SortConstants constants;
    constants.element_count = padded;
    constants.block_count = Groups(padded, 256);
    constants.digit_word = digit / 4;
    constants.digit_shift = (digit % 4) * 8;
    constants.scan_offset = histogram_offset;
    if (dynamic_count) {
      // Single-resource fixtures use the GPU-written visible count. Padding
      // remains in the buffers but does not participate in this sort.
      Require(snapshot.gaussians.size() == 1, "Dynamic-count fixture must use one resource");
      constants.flags = 1;
      constants.count_word = 4;
    }
    sort_dispatch(kernels.histogram, constants, constants.block_count);
    std::vector<SortConstants> levels;
    auto level = constants;
    level.scan_count = padded;
    for (;;) {
      level.scan_sums_offset = level.scan_offset + level.scan_count;
      const auto groups = Groups(level.scan_count, 1024);
      sort_dispatch(kernels.scan, level, groups);
      levels.push_back(level);
      if (groups == 1) break;
      level.scan_offset = level.scan_sums_offset;
      level.scan_count = groups;
    }
    for (std::size_t i = levels.size() - 1; i > 0; --i)
      sort_dispatch(kernels.add, levels[i - 1], Groups(levels[i - 1].scan_count, 1024));
    sort_dispatch(kernels.scatter, constants, constants.block_count);
    std::swap(source, destination);
  }
  SortConstants verify;
  verify.element_count = dynamic_count ? static_cast<std::uint32_t>(reference.gaussians.size()) : padded;
  if (verify.element_count) sort_dispatch(kernels.verify, verify, Groups(verify.element_count, 256));
  [command commit];
  [command waitUntilCompleted];
  if (command.status != MTLCommandBufferStatusCompleted)
    throw std::runtime_error(command.error.localizedDescription.UTF8String);
  const auto* verification = static_cast<const std::uint32_t*>(control.contents);
  Require(verification[0] == reference.gaussians.size(), "Wrong sorted count");
  Require(verification[1] == 0 && verification[2] == 0, "GPU sort verification failed");
  const auto* sorted = static_cast<const SortElement*>(source.contents);
  const auto* records = static_cast<const PreparedRecord*>(prepared.contents);
  for (std::size_t i = 0; i < reference.gaussians.size(); ++i) {
    Require(sorted[i].value < padded, "Sorted record index exceeds capacity");
    const auto& actual = records[sorted[i].value];
    const auto& expected = reference.gaussians[i];
    if (actual.particle_id != expected.particle) {
      std::cerr << "identity at " << i << " particles=" << total << " degree="
                << snapshot.gaussians[0].spherical_harmonics_degree
                << " projection=" << int(snapshot.gaussians[0].projection_mode)
                << " sorting=" << int(policy.mode) << " dynamic=" << dynamic_count
                << " actual=" << actual.particle_id << " key=" << actual.sort_key
                << " expected=" << expected.particle << " key=" << expected.sort_key << '\n';
    }
    Require((std::uint64_t{actual.resource_id_high} << 32U | actual.resource_id_low) == expected.resource &&
        actual.particle_id == expected.particle, "Non-deterministic sorted identity");
    Near(actual.center_pixels.x, expected.center_pixels.x, "center_pixels.x");
    Near(actual.center_pixels.y, expected.center_pixels.y, "center_pixels.y");
    Near(actual.radius_pixels, expected.radius_pixels, "radius_pixels");
    Near(actual.depth, expected.depth, "depth");
    Near(actual.inverse_conic.x, expected.inverse_conic.x, "inverse_conic.x");
    Near(actual.inverse_conic.y, expected.inverse_conic.y, "inverse_conic.y");
    Near(actual.inverse_conic.z, expected.inverse_conic.z, "inverse_conic.z");
    Near(actual.opacity, expected.opacity, "opacity");
    Near(actual.radiance.x, expected.radiance.x, "radiance.x");
    Near(actual.radiance.y, expected.radiance.y, "radiance.y");
    Near(actual.radiance.z, expected.radiance.z, "radiance.z");
    Near(actual.sort_key, expected.sort_key, "sort_key");
  }
  if (!dynamic_count)
    for (std::size_t i = reference.gaussians.size(); i < padded; ++i)
      Require(sorted[i].value == UINT32_MAX && sorted[i].key_low == UINT32_MAX &&
          sorted[i].key_high == UINT32_MAX, "Invalid sentinel tail");
  std::array<std::uint64_t, 4> sums{};
  for (std::size_t i = 0; i < counters.size(); ++i) {
    const auto& record = snapshot.gaussians[i];
    const auto& actual = *static_cast<const PrepareCounters*>(counters[i].contents);
    Require(actual.candidate_count == (record.visible ? record.positions->size() : 0),
        "Wrong candidate count");
    const std::array counts{actual.visible_count, actual.opacity_culled_count,
        actual.frustum_culled_count, actual.invalid_culled_count};
    std::array<std::uint32_t, 4> classified{};
    if (record.visible) {
      const auto* values = static_cast<const std::uint32_t*>(classifications[i].contents);
      for (std::size_t j = 0; j < record.positions->size(); ++j) {
        Require(values[j] < 4, "Invalid candidate classification");
        ++classified[values[j]];
      }
    }
    Require(counts == classified, "Candidate classifications disagree with counters");
    for (std::size_t j = 0; j < counts.size(); ++j) sums[j] += counts[j];
  }
  Require(sums[0] == reference.counters.visible_count &&
      sums[1] == reference.counters.opacity_culled_count &&
      sums[2] == reference.counters.frustum_culled_count &&
      sums[3] == reference.counters.invalid_culled_count, "GPU culling differs from CPU");
}

FrameSnapshot Fixture(std::uint32_t count, std::uint32_t degree, bool perspective) {
  FrameSnapshot snapshot;
  if (perspective) snapshot.projection.values = {
      1.2F, 0, 0, 0, 0, 1.5F, 0, 0, 0, 0, -1.001001F, -1,
      0, 0, -0.1001001F, 0};
  merlin::extraction::GaussianRecord record;
  record.gaussian = 0x200000003ULL;
  record.spherical_harmonics_degree = degree;
  std::vector<merlin::Vec3> positions(count);
  std::vector<merlin::Covariance3> covariances(count, {0.0004F, 0.0001F, 0, 0.0002F, 0, 0.0001F});
  std::vector<float> opacities(count, 0.65F);
  const auto coefficients = (degree + 1) * (degree + 1);
  std::vector<merlin::Vec3> radiance(count * coefficients);
  for (std::uint32_t i = 0; i < count; ++i) {
    // Binary-exact coordinates keep intentional distance ties exact on CPU
    // and GPU; unrelated floating-point near-ties are not sort failures.
    positions[i] = {float(int(i % 17) - 8) * 0.0625F,
        float(int(i % 13) - 6) * 0.0625F,
        perspective ? -1.0F - float(i % 7) * 0.25F : 0.25F + float(i % 7) * 0.0625F};
    for (std::uint32_t j = 0; j < coefficients; ++j)
      radiance[i * coefficients + j] = {float(j + 1) * 0.02F, -0.03F, 0.04F};
    // Duplicate depths exercise deterministic tie-breaking across workgroups.
    if (i % 31 == 0) opacities[i] = 0;
    if (i % 37 == 0) positions[i].x = 1000;
  }
  if (count >= 8) {
    opacities[1] = std::numeric_limits<float>::quiet_NaN();
    positions[2].z = perspective ? -0.05F : -0.01F; // Near plane.
    positions[3].z = perspective ? -101.0F : 1.01F; // Conservative far bound.
    positions[4].z = perspective ? -200.0F : 2.0F;
    positions[5].x = std::numeric_limits<float>::infinity();
    covariances[6].xx = std::numeric_limits<float>::quiet_NaN();
    covariances[7].zz = -1.0F;
  }
  record.positions = std::make_shared<const std::vector<merlin::Vec3>>(positions);
  record.covariances = std::make_shared<const std::vector<merlin::Covariance3>>(covariances);
  record.opacities = std::make_shared<const std::vector<float>>(opacities);
  record.spherical_harmonics_coefficients = std::make_shared<const std::vector<merlin::Vec3>>(radiance);
  snapshot.gaussians.push_back(record);
  return snapshot;
}
} // namespace

int main(int argc, char** argv) {
  @autoreleasepool {
    try {
      Require(argc == 2, "Expected compiled Metal library path");
      auto device = MTLCreateSystemDefaultDevice();
      if (!device) { std::cerr << "skip: no Metal device\n"; return 77; }
      NSError* error = nil;
      auto library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
      if (!library) throw std::runtime_error(error.localizedDescription.UTF8String);
      auto queue = [device newCommandQueue];
      Require(queue != nil, "Metal queue creation failed");
      const Kernels kernels{
          Pipeline(device, library, @"gaussian_prepare_compact"),
          Pipeline(device, library, @"gaussian_sort_keys"),
          Pipeline(device, library, @"gaussian_sort_histogram"),
          Pipeline(device, library, @"gaussian_sort_scan_blocks"),
          Pipeline(device, library, @"gaussian_sort_scan_add"),
          Pipeline(device, library, @"gaussian_sort_scatter"),
          Pipeline(device, library, @"gaussian_sort_verify")};
      Compare(device, queue, kernels, {});
      for (auto count : {0U, 1U, 63U, 64U, 65U, 255U, 256U, 257U, 1301U})
        Compare(device, queue, kernels, Fixture(count, 0, false));
      for (std::uint32_t degree = 0; degree <= 3; ++degree) {
        for (bool perspective : {false, true}) {
          for (auto projection : {merlin::GaussianProjectionMode::Perspective,
                   merlin::GaussianProjectionMode::Tangential}) {
            for (auto sorting : {merlin::GaussianSortingMode::ZDepth,
                     merlin::GaussianSortingMode::CameraDistance}) {
              auto snapshot = Fixture(1301, degree, perspective);
              auto first = snapshot.gaussians[0];
              first.projection_mode = projection;
              first.sorting_mode = sorting;
              snapshot.gaussians.assign({first});
              Compare(device, queue, kernels, snapshot, true);
              auto second = first;
              second.gaussian = 0x100000004ULL;
              snapshot.gaussians.push_back(second);
              Compare(device, queue, kernels, snapshot);
            }
          }
        }
      }
      auto transformed = Fixture(257, 3, true);
      auto transformed_record = transformed.gaussians[0];
      transformed_record.transform.values = {
          0.8F, 0.2F, 0, 0, -0.3F, 1.1F, 0, 0, 0, 0, 1.2F, 0,
          0.1F, -0.1F, -0.2F, 1};
      transformed.gaussians.assign({transformed_record});
      Compare(device, queue, kernels, transformed);
      auto mixed = Fixture(257, 1, true);
      auto second = mixed.gaussians[0];
      second.gaussian = 1;
      second.sorting_mode = merlin::GaussianSortingMode::CameraDistance;
      mixed.gaussians.push_back(second);
      Compare(device, queue, kernels, mixed);
      auto hidden = mixed.gaussians[0];
      hidden.visible = false;
      mixed.gaussians.assign({hidden, second});
      Compare(device, queue, kernels, mixed);
      auto culled = Fixture(257, 0, false);
      auto culled_record = culled.gaussians[0];
      culled_record.opacities = std::make_shared<const std::vector<float>>(257, 0);
      culled.gaussians.assign({culled_record});
      Compare(device, queue, kernels, culled);
      Compare(device, queue, kernels, culled, true);
      std::cout << "Metal preparation and radix sort match CPU: " << device.name.UTF8String << '\n';
      return 0;
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
    }
  }
}
