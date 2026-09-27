#include "../backend/merlin-metal/src/gaussian_compute_abi.hpp"
#include "../backend/merlin-metal/src/gaussian_raster_abi.hpp"
#include <merlin/extraction/gaussian_preparation.hpp>
#include <merlin/metal/backend.hpp>

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
using merlin::metal::GaussianInstance;

static_assert(sizeof(MTLDrawPrimitivesIndirectArguments) == 16);
static_assert(offsetof(MTLDrawPrimitivesIndirectArguments, instanceCount) == 4);
static_assert(offsetof(MTLDrawPrimitivesIndirectArguments, vertexStart) == 8);
static_assert(offsetof(MTLDrawPrimitivesIndirectArguments, baseInstance) == 12);

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
  id<MTLComputePipelineState> gather;
  id<MTLRenderPipelineState> raster;
  id<MTLDepthStencilState> depth;
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

id<MTLRenderPipelineState> RasterPipeline(id<MTLDevice> device, id<MTLLibrary> library) {
  auto descriptor = [MTLRenderPipelineDescriptor new];
  descriptor.vertexFunction = [library newFunctionWithName:@"gaussian_metal_vertex"];
  descriptor.fragmentFunction = [library newFunctionWithName:@"gaussian_metal_fragment"];
  descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
  descriptor.colorAttachments[0].blendingEnabled = YES;
  descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
  descriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
  descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
  descriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
  descriptor.colorAttachments[1].pixelFormat = MTLPixelFormatR32Uint;
  descriptor.colorAttachments[2].pixelFormat = MTLPixelFormatR32Uint;
  descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
  NSError* error = nil;
  auto pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
  if (!pipeline) throw std::runtime_error(error.localizedDescription.UTF8String);
  return pipeline;
}

id<MTLDepthStencilState> DepthState(id<MTLDevice> device) {
  auto descriptor = [MTLDepthStencilDescriptor new];
  descriptor.depthCompareFunction = MTLCompareFunctionLessEqual;
  descriptor.depthWriteEnabled = NO;
  auto state = [device newDepthStencilStateWithDescriptor:descriptor];
  Require(state != nil, "Metal depth state creation failed");
  return state;
}

// Readback happens after both draws and all compute work in the submission.
// A constant opaque depth also tests rejection without permitting depth writes.
std::array<id<MTLBuffer>, 4> Raster(id<MTLDevice> device,
    id<MTLCommandBuffer> command, const Kernels& kernels, id<MTLBuffer> instances,
    id<MTLBuffer> indirect, std::uint32_t count, float depth) {
  constexpr NSUInteger width = 320, height = 192, row_bytes = width * 4;
  const std::array formats{MTLPixelFormatRGBA8Unorm, MTLPixelFormatR32Uint,
      MTLPixelFormatR32Uint, MTLPixelFormatDepth32Float};
  std::array<id<MTLTexture>, 4> textures;
  std::array<id<MTLBuffer>, 4> readbacks;
  auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
  for (std::size_t i = 0; i < textures.size(); ++i) {
    auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:formats[i]
        width:width height:height mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageRenderTarget;
    textures[i] = [device newTextureWithDescriptor:descriptor];
    Require(textures[i] != nil, "Metal raster texture allocation failed");
    readbacks[i] = Buffer(device, row_bytes * height);
    if (i < 3) {
      pass.colorAttachments[i].texture = textures[i];
      pass.colorAttachments[i].loadAction = MTLLoadActionClear;
      pass.colorAttachments[i].storeAction = MTLStoreActionStore;
      pass.colorAttachments[i].clearColor = i == 0 ? MTLClearColorMake(0, 0, 0, 0)
          : MTLClearColorMake(UINT32_MAX, 0, 0, 0);
    } else {
      pass.depthAttachment.texture = textures[i];
      pass.depthAttachment.loadAction = MTLLoadActionClear;
      pass.depthAttachment.storeAction = MTLStoreActionStore;
      pass.depthAttachment.clearDepth = depth;
    }
  }
  auto encoder = [command renderCommandEncoderWithDescriptor:pass];
  Require(encoder != nil, "Metal render encoder allocation failed");
  [encoder setRenderPipelineState:kernels.raster];
  [encoder setDepthStencilState:kernels.depth];
  [encoder setVertexBuffer:instances offset:0 atIndex:MERLIN_GAUSSIAN_INSTANCES_BINDING];
  const merlin::Vec2 inverse_extent{1.0F / width, 1.0F / height};
  [encoder setVertexBytes:&inverse_extent length:sizeof(inverse_extent)
                 atIndex:MERLIN_GAUSSIAN_CONSTANTS_BINDING];
  if (indirect) {
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle indirectBuffer:indirect indirectBufferOffset:0];
  } else if (count) {
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:count];
  }
  [encoder endEncoding];
  auto blit = [command blitCommandEncoder];
  for (std::size_t i = 0; i < textures.size(); ++i)
    [blit copyFromTexture:textures[i] sourceSlice:0 sourceLevel:0
        sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(width, height, 1)
        toBuffer:readbacks[i] destinationOffset:0 destinationBytesPerRow:row_bytes
        destinationBytesPerImage:row_bytes * height];
  [blit endEncoding];
  return readbacks;
}

// Preparation, global sort, gather and optional indirect raster in one
// submission. No intermediate readback schedules subsequent GPU stages.
std::array<id<MTLBuffer>, 4> Compare(id<MTLDevice> device, id<MTLCommandQueue> queue, const Kernels& kernels,
    FrameSnapshot snapshot, bool dynamic_count = false, bool compare_image = false,
    float opaque_depth = 1.0F) {
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
  auto instances = Buffer(device, (padded + 1U) * sizeof(GaussianInstance));
  auto draw = Buffer(device, sizeof(MTLDrawPrimitivesIndirectArguments));
  // Poison every argument and the output tail to catch missing initialization,
  // stale empty-frame counts, sentinel reads and out-of-bounds stores.
  std::memset(instances.contents, 0xA5, instances.length);
  std::memset(draw.contents, 0xA5, draw.length);
  GatherConstants gather;
  gather.element_count = total == 0 ? 0 : padded;
  gather.count_word = 4;
  gather.flags = dynamic_count ? 1U : 0U;
  auto encoder = [command computeCommandEncoder];
  [encoder setComputePipelineState:kernels.gather];
  [encoder setBuffer:source offset:0 atIndex:0];
  [encoder setBuffer:prepared offset:0 atIndex:1];
  [encoder setBuffer:instances offset:0 atIndex:2];
  [encoder setBuffer:draw offset:0 atIndex:3];
  [encoder setBytes:&gather length:sizeof(gather) atIndex:4];
  [encoder setBuffer:control offset:0 atIndex:5];
  [encoder dispatchThreadgroups:MTLSizeMake(Groups(padded, 256), 1, 1)
          threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
  [encoder endEncoding];
  std::array<id<MTLBuffer>, 4> gpu_image{}, cpu_image{};
  if (compare_image) {
    gpu_image = Raster(device, command, kernels, instances, draw, 0, opaque_depth);
    std::vector<GaussianInstance> cpu_instances;
    for (const auto& g : reference.gaussians)
      cpu_instances.push_back({g.center_pixels, g.inverse_conic, g.radiance,
          g.opacity, g.radius_pixels, g.depth, static_cast<std::uint32_t>(g.resource), g.particle});
    cpu_image = Raster(device, command, kernels, Upload(device, cpu_instances), nil,
        static_cast<std::uint32_t>(cpu_instances.size()), opaque_depth);
  }
  [command commit];
  [command waitUntilCompleted];
  if (command.status != MTLCommandBufferStatusCompleted)
    throw std::runtime_error(command.error.localizedDescription.UTF8String);
  const auto& arguments = *static_cast<const MTLDrawPrimitivesIndirectArguments*>(draw.contents);
  Require(arguments.vertexCount == 6 && arguments.instanceCount == reference.gaussians.size() &&
      arguments.vertexStart == 0 && arguments.baseInstance == 0, "Invalid GPU draw arguments");
  const auto* tail = static_cast<const std::uint8_t*>(instances.contents);
  for (std::size_t i = reference.gaussians.size() * sizeof(GaussianInstance); i < instances.length; ++i)
    Require(tail[i] == 0xA5, "Gather overwrote the sentinel tail or output guard");
  if (compare_image) {
    const auto* actual = static_cast<const std::uint8_t*>(gpu_image[0].contents);
    const auto* expected = static_cast<const std::uint8_t*>(cpu_image[0].contents);
    for (NSUInteger i = 0; i < gpu_image[0].length; ++i)
      Require(std::abs(int(actual[i]) - int(expected[i])) <= 2, "Indirect color differs from CPU raster");
    for (std::size_t i = 1; i < gpu_image.size(); ++i)
      Require(std::memcmp(gpu_image[i].contents, cpu_image[i].contents, gpu_image[i].length) == 0,
          "Indirect ID/depth differs from CPU raster");
  }
  const auto* verification = static_cast<const std::uint32_t*>(control.contents);
  Require(verification[0] == reference.gaussians.size(), "Wrong sorted count");
  Require(verification[1] == 0 && verification[2] == 0, "GPU sort verification failed");
  const auto* sorted = static_cast<const SortElement*>(source.contents);
  const auto* records = static_cast<const PreparedRecord*>(prepared.contents);
  for (std::size_t i = 0; i < reference.gaussians.size(); ++i) {
    Require(sorted[i].value < padded, "Sorted record index exceeds capacity");
    const auto& actual = records[sorted[i].value];
    const auto& expected = reference.gaussians[i];
    const GaussianInstance packed{actual.center_pixels, actual.inverse_conic, actual.radiance,
        actual.opacity, actual.radius_pixels, actual.depth, actual.resource_id_low, actual.particle_id};
    Require(std::memcmp(static_cast<const GaussianInstance*>(instances.contents) + i,
        &packed, sizeof(packed)) == 0, "Gather changed prepared fields or raster order");
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
  return gpu_image;
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

FrameSnapshot RasterFixture(std::uint32_t count) {
  auto snapshot = Fixture(count, 0, false);
  auto record = snapshot.gaussians[0];
  std::vector<merlin::Vec3> positions(count), radiance(count);
  constexpr float sh = 0.2820947918F;
  for (std::uint32_t i = 0; i < count; ++i) {
    switch (i % 3) {
    case 0:
      positions[i] = {0, 0, 0.3F};
      radiance[i] = {0.5F / sh, -0.5F / sh, -0.5F / sh};
      break;
    case 1:
      positions[i] = {0, 0, 0.6F};
      radiance[i] = {-0.5F / sh, 0.5F / sh, -0.5F / sh};
      break;
    default:
      positions[i] = {0.5F, 0.5F, 0.4F};
      radiance[i] = {-0.5F / sh, -0.5F / sh, 0.5F / sh};
      break;
    }
  }
  record.positions = std::make_shared<const std::vector<merlin::Vec3>>(positions);
  record.covariances = std::make_shared<const std::vector<merlin::Covariance3>>(
      count, merlin::Covariance3{0.01F, 0, 0, 0.0025F, 0, 0.0001F});
  record.opacities = std::make_shared<const std::vector<float>>(count, 0.5F);
  record.spherical_harmonics_coefficients = std::make_shared<const std::vector<merlin::Vec3>>(radiance);
  snapshot.gaussians.assign({record});
  return snapshot;
}
} // namespace

int main(int argc, char** argv) {
  @autoreleasepool {
    try {
      Require(argc == 2, "Expected compiled Metal library path");
      // Use the same renderer capability gate as the other Metal image tests.
      // A non-null device alone does not establish native backend support.
      const auto availability = merlin::metal::BackendFactory{}.availability();
      if (!availability.available) {
        std::cerr << "skip: " << availability.detail << '\n';
        return 77;
      }
      auto device = MTLCreateSystemDefaultDevice();
      if (!device) { std::cerr << "skip: no Metal device\n"; return 77; }
      std::cout << "Metal compute device: " << device.name.UTF8String << std::endl;
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
          Pipeline(device, library, @"gaussian_sort_verify"),
          Pipeline(device, library, @"gaussian_metal_gather"),
          RasterPipeline(device, library), DepthState(device)};
      Compare(device, queue, kernels, {}, false, true);
      const auto image = Compare(device, queue, kernels, RasterFixture(3), false, true);
      const auto* color = static_cast<const std::uint8_t*>(image[0].contents);
      const auto* ids = static_cast<const std::uint32_t*>(image[2].contents);
      const auto center = 96 * 320 + 160;
      Require(color[center * 4] > 120 && color[center * 4] < 130 &&
          color[center * 4 + 1] > 60 && color[center * 4 + 1] < 70 && ids[center] == 0,
          "Indirect draw lost back-to-front composition or nearest picking");
      Require(ids[48 * 320 + 240] == 2 && ids[144 * 320 + 240] == UINT32_MAX,
          "Indirect draw has incorrect Y orientation");
      const auto occluded = Compare(device, queue, kernels, RasterFixture(3), true, true, 0.45F);
      const auto* occluded_color = static_cast<const std::uint8_t*>(occluded[0].contents);
      Require(occluded_color[center * 4] > 120 && occluded_color[center * 4 + 1] == 0,
          "Indirect draw ignored opaque depth");
      Require(static_cast<const float*>(occluded[3].contents)[center] == 0.45F,
          "Indirect draw overwrote opaque depth");
      // Fully occupied groups exercise the boundary without a sentinel, and
      // asymmetric overlapping colors expose incorrect order/orientation.
      for (auto count : {1U, 3U, 255U, 256U, 257U}) {
        Compare(device, queue, kernels, RasterFixture(count), false, true);
        Compare(device, queue, kernels, RasterFixture(count), true, true, 0.45F);
      }
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
              Compare(device, queue, kernels, snapshot, true, degree == 3);
              auto second = first;
              second.gaussian = 0x100000004ULL;
              snapshot.gaussians.push_back(second);
              Compare(device, queue, kernels, snapshot, false, degree == 3);
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
      Compare(device, queue, kernels, transformed, false, true);
      auto mixed = Fixture(257, 1, true);
      auto second = mixed.gaussians[0];
      second.gaussian = 1;
      second.sorting_mode = merlin::GaussianSortingMode::CameraDistance;
      mixed.gaussians.push_back(second);
      Compare(device, queue, kernels, mixed, false, true);
      auto hidden = mixed.gaussians[0];
      hidden.visible = false;
      mixed.gaussians.assign({hidden, second});
      Compare(device, queue, kernels, mixed, false, true);
      auto culled = Fixture(257, 0, false);
      auto culled_record = culled.gaussians[0];
      culled_record.opacities = std::make_shared<const std::vector<float>>(257, 0);
      culled.gaussians.assign({culled_record});
      Compare(device, queue, kernels, culled);
      Compare(device, queue, kernels, culled, true, true);
      std::cout << "Metal preparation, radix sort, gather and indirect raster match CPU: " << device.name.UTF8String << '\n';
      return 0;
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
    }
  }
}
