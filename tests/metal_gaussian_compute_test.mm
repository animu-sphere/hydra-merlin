#include "../backend/merlin-metal/src/gaussian_execution.hpp"
#include "../backend/merlin-metal/src/gaussian_raster_abi.hpp"
#include "../backend/merlin-metal/src/gaussian_residency.hpp"
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
#include <source_location>
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
  merlin::metal::GaussianExecution& execution;
  id<MTLRenderPipelineState> raster;
  id<MTLDepthStencilState> depth;
};

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
    float opaque_depth = 1.0F, merlin::metal::GaussianResidency* persistent = nullptr,
    std::uint64_t* attribute_upload_bytes = nullptr, std::uint64_t* scratch_allocations = nullptr) {
  std::vector<merlin::extraction::GaussianRecord> ordered(snapshot.gaussians.begin(), snapshot.gaussians.end());
  std::sort(ordered.begin(), ordered.end(),
      [](const auto& a, const auto& b) { return a.gaussian < b.gaussian; });
  snapshot.gaussians.assign(std::move(ordered));
  const auto reference = merlin::extraction::PrepareGaussianFrame(snapshot, {320, 192});
  auto command = [queue commandBuffer];
  Require(command != nil, "Metal command allocation failed");
  merlin::metal::GaussianResidency local_residency(device, 64 * 1024 * 1024);
  auto& residency = persistent ? *persistent : local_residency;
  auto attributes = residency.Prepare(snapshot);
  residency.Encode(attributes, command);
  if (attribute_upload_bytes) *attribute_upload_bytes = attributes->upload_bytes;
  const auto frame = kernels.execution.Encode(attributes, snapshot.view, snapshot.projection,
      320, 192, command, dynamic_count, true);
  if (scratch_allocations) *scratch_allocations = frame->allocation_count;
  const auto& scratch = *frame->scratch;
  const auto total = frame->particle_count;
  const auto padded = frame->padded_count;
  const auto policy = frame->sorting_policy;
  auto instances = scratch.instances;
  auto draw = scratch.draw;
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
  // Diagnostic copies happen after all compute and raster work. Production
  // scheduling consumes only device buffers, including the indirect count.
  auto readback = [command blitCommandEncoder];
  const auto read = [&](id<MTLBuffer> source, std::size_t offset, std::size_t bytes) {
    auto target = Buffer(device, bytes);
    if (bytes) [readback copyFromBuffer:source sourceOffset:offset toBuffer:target destinationOffset:0 size:bytes];
    return target;
  };
  auto prepared = read(scratch.prepared, 0, scratch.prepared.length);
  auto source = read(scratch.sort[1], 0, scratch.sort[1].length);
  auto control = read(scratch.control, 0, scratch.control.length);
  instances = read(scratch.instances, 0, scratch.instances.length);
  draw = read(scratch.draw, 0, scratch.draw.length);
  std::vector<id<MTLBuffer>> counters, classifications;
  std::size_t base = 0;
  for (std::size_t i = 0; i < snapshot.gaussians.size(); ++i) {
    const auto count = snapshot.gaussians[i].positions->size();
    counters.push_back(read(scratch.counters, i * sizeof(PrepareCounters), sizeof(PrepareCounters)));
    classifications.push_back(read(scratch.classifications, base * sizeof(std::uint32_t), count * sizeof(std::uint32_t)));
    base += count;
  }
  [readback endEncoding];
  [command commit];
  residency.Commit(attributes);
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
template <typename Function>
void Reject(Function function, merlin::render::RendererErrorCode code,
    std::source_location location = std::source_location::current()) {
  try {
    function();
  } catch (const merlin::render::RendererError& error) {
    Require(error.code() == code, "Wrong execution error code");
    return;
  }
  throw std::runtime_error("Expected execution rejection at line " + std::to_string(location.line()));
}

void TestExecutionLifetime(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLLibrary> library) {
  using merlin::metal::GaussianExecution;
  using merlin::metal::GaussianResidency;
  using merlin::render::RendererErrorCode;
  GaussianResidency residency(device, 1024 * 1024);
  auto snapshot = RasterFixture(3);
  auto attributes = residency.Prepare(snapshot);
  auto upload = [queue commandBuffer];
  residency.Encode(attributes, upload);
  [upload commit];
  residency.Commit(attributes);
  [upload waitUntilCompleted];
  Require(upload.status == MTLCommandBufferStatusCompleted, "Lifetime fixture upload failed");
  const auto encode = [&](GaussianExecution& execution, id<MTLCommandBuffer> command) {
    return execution.Encode(attributes, snapshot.view, snapshot.projection, 320, 192, command);
  };
  std::uint64_t bytes = 0;
  GaussianExecution probe(device, library, 1024 * 1024);
  @autoreleasepool {
    // An encoded but abandoned command must release its lease when destroyed.
    auto command = [queue commandBuffer];
    auto frame = encode(probe, command);
    bytes = frame->scratch->bytes;
    Require(frame->allocation_count == 8 && frame->dispatch_count == 27,
        "Unexpected small-frame allocation or dispatch count");
    frame.reset();
    probe.Reset();
    Require(probe.live_bytes() == bytes, "Unsubmitted command lost its scratch lease");
  }
  Require(probe.live_bytes() == 0, "Abandoned command leaked scratch");

  GaussianExecution too_small(device, library, bytes - 1);
  Reject([&] { encode(too_small, [queue commandBuffer]); }, RendererErrorCode::ResourceExhausted);
  Require(too_small.live_bytes() == 0, "Failed allocation leaked scratch budget");
  Reject([&] { encode(probe, nil); }, RendererErrorCode::InvalidRequest);
  auto unretained = [queue commandBufferWithUnretainedReferences];
  // Metal validation may force resource retention even for this API.
  if (!unretained.retainedReferences)
    Reject([&] { encode(probe, unretained); }, RendererErrorCode::InvalidRequest);
  Reject([&] {
    probe.Encode(attributes, snapshot.view, snapshot.projection, 0, 192, [queue commandBuffer]);
  }, RendererErrorCode::InvalidRequest);
  auto no_resources = residency.Prepare({});
  Reject([&] {
    probe.Encode(no_resources, snapshot.view, snapshot.projection, 320, 192, [queue commandBuffer], true);
  }, RendererErrorCode::InvalidRequest);
  Require(probe.live_bytes() == 0, "Invalid frame allocated scratch");

  GaussianExecution execution(device, library, bytes * 2);
  auto gate = [device newSharedEvent];
  Require(gate != nil, "Shared event allocation failed");
  struct ReleaseGate {
    id<MTLSharedEvent> event;
    ~ReleaseGate() { event.signaledValue = 1; }
  } release_gate{gate};
  auto first_command = [queue commandBuffer];
  [first_command encodeWaitForEvent:gate value:1];
  auto first = encode(execution, first_command);
  const auto first_scratch = std::weak_ptr<const GaussianExecution::Scratch>(first->scratch);
  [first_command commit];
  auto second_command = [queue commandBuffer];
  auto second = execution.Encode(no_resources, snapshot.view, snapshot.projection,
      640, 384, second_command);
  Require(first->scratch != second->scratch && execution.live_bytes() == bytes + second->scratch->bytes,
      "Unfinished frames shared mutable scratch or escaped the budget");
  // Re-read the first draw after the second frame's compute work. A scratch
  // alias would replace its three instances with the empty frame's zero count.
  auto first_draw = Buffer(device, 16), second_draw = Buffer(device, 16);
  auto read = [second_command blitCommandEncoder];
  [read copyFromBuffer:first->scratch->draw sourceOffset:0 toBuffer:first_draw destinationOffset:0 size:16];
  [read copyFromBuffer:second->scratch->draw sourceOffset:0 toBuffer:second_draw destinationOffset:0 size:16];
  [read endEncoding];
  [second_command commit];
  Reject([&] { encode(execution, [queue commandBuffer]); }, RendererErrorCode::ResourceExhausted);
  const auto live = execution.live_bytes();
  const auto old_attributes = std::weak_ptr<const GaussianResidency::Buffer>(attributes->resources[0].positions);
  attributes.reset();
  residency.Reset();
  Require(!old_attributes.expired(), "Execution lost resident attribute accounting before completion");
  first.reset();
  second.reset();
  execution.Reset();
  Require(execution.live_bytes() == live && !first_scratch.expired(),
      "Reset retired unfinished frame scratch");
  gate.signaledValue = 1;
  [first_command waitUntilCompleted];
  [second_command waitUntilCompleted];
  Require(first_command.status == MTLCommandBufferStatusCompleted &&
      second_command.status == MTLCommandBufferStatusCompleted, "Blocked execution failed");
  const auto& old_draw = *static_cast<const MTLDrawPrimitivesIndirectArguments*>(first_draw.contents);
  const auto& empty_draw = *static_cast<const MTLDrawPrimitivesIndirectArguments*>(second_draw.contents);
  Require(old_draw.vertexCount == 6 && old_draw.instanceCount == 3 && old_draw.vertexStart == 0 &&
      old_draw.baseInstance == 0 && empty_draw.vertexCount == 6 && empty_draw.instanceCount == 0 &&
      empty_draw.vertexStart == 0 && empty_draw.baseInstance == 0,
      "Concurrent or empty frames corrupted indirect arguments");
  Require(execution.live_bytes() == 0 && first_scratch.expired(), "Completed commands leaked scratch");
  Require(old_attributes.expired() && residency.live_bytes() == 0, "Completed execution leaked attributes");
  attributes = residency.Prepare(snapshot);
  auto reload = [queue commandBuffer];
  residency.Encode(attributes, reload);
  [reload commit];
  residency.Commit(attributes);
  [reload waitUntilCompleted];

  // A completed output retained by the caller still holds its lease. Once
  // released, the next frame reuses buffers even across extent/count changes.
  auto completed_command = [queue commandBuffer];
  auto completed = encode(execution, completed_command);
  [completed_command commit];
  [completed_command waitUntilCompleted];
  const auto saved = std::weak_ptr<const GaussianExecution::Scratch>(completed->scratch);
  auto retained_command = [queue commandBuffer];
  auto retained = encode(execution, retained_command);
  Require(retained->scratch != completed->scratch, "Retained completed output was overwritten");
  [retained_command commit];
  [retained_command waitUntilCompleted];
  retained.reset();
  completed.reset();
  auto reuse_command = [queue commandBuffer];
  auto reuse = execution.Encode(attributes, snapshot.view, snapshot.projection,
      640, 384, reuse_command, true);
  Require(reuse->allocation_count == 0 && reuse->scratch == saved.lock(), "Completed scratch was not reused");
  [reuse_command commit];
  [reuse_command waitUntilCompleted];
  Reject([&] { encode(execution, reuse_command); }, RendererErrorCode::InvalidRequest);
  reuse.reset();
  execution.Reset();
  Require(execution.live_bytes() == 0, "Idle reset leaked scratch");
  std::cout << "Scratch lifetime: frame=" << bytes
            << " bytes, static/resize reuse=0 allocations, blocked/reset/budget checks passed\n";
}

void CompareResidentFrames(id<MTLDevice> device, id<MTLCommandQueue> queue, const Kernels& kernels) {
  merlin::metal::GaussianResidency residency(device, 64 * 1024 * 1024);
  auto snapshot = RasterFixture(3);
  snapshot.source_id = 17;
  auto record = snapshot.gaussians[0];
  record.revision = 1;
  snapshot.gaussians.assign({record});
  std::uint64_t bytes = 0, allocations = 0;
  const auto compare = [&] {
    Compare(device, queue, kernels, snapshot, false, true, 1.0F, &residency, &bytes, &allocations);
  };
  compare();
  const auto initial_bytes = bytes;
  Require(initial_bytes == 3 * (2 * sizeof(merlin::Vec3) + sizeof(merlin::Covariance3) + sizeof(float)),
      "Initial resident upload differs from source attributes");
  compare();
  Require(bytes == 0, "Static frame reuploaded source attributes");
  Require(allocations == 0, "Static frame reallocated scratch");
  snapshot.view.values[12] = 0.125F;
  compare();
  Require(bytes == 0, "Camera motion reuploaded source attributes");
  Require(allocations == 0, "Camera motion reallocated scratch");
  record.revision = 2;
  record.transform.values[13] = -0.125F;
  snapshot.gaussians.assign({record});
  compare();
  Require(bytes == 0, "Transform edit reuploaded source attributes");

  record.revision = record.opacity_revision = record.covariance_revision = 3;
  record.particle_base_revision = 2;
  record.particle_ranges = {{1, 1}};
  auto opacity = std::make_shared<std::vector<float>>(*record.opacities);
  (*opacity)[1] = 0.25F;
  record.opacities = opacity;
  auto covariance = std::make_shared<std::vector<merlin::Covariance3>>(*record.covariances);
  (*covariance)[1].xx *= 2;
  record.covariances = covariance;
  snapshot.gaussians.assign({record});
  compare();
  const auto partial_bytes = bytes;
  Require(partial_bytes == sizeof(float) + sizeof(merlin::Covariance3),
      "Localized edit did not upload only changed attribute ranges");
  compare();
  Require(bytes == 0, "Repeated partial-update snapshot uploaded again");
  record.visible = false;
  record.revision = 4;
  snapshot.gaussians.assign({record});
  compare();
  Require(bytes == 0, "Visibility edit reuploaded source attributes");
  snapshot.gaussians.assign({});
  compare();
  Require(bytes == 0, "Empty frame uploaded source attributes");
  record.visible = true;
  record.gaussian += 1ULL << 32;
  snapshot.gaussians.assign({record});
  compare();
  Require(bytes == initial_bytes, "New resource generation reused removed attributes");
  std::cout << "Resident image sequence: initial=" << initial_bytes
            << " static/camera/transform/visibility=0 partial=" << partial_bytes << " upload bytes\n";
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
      merlin::metal::GaussianExecution execution(device, library, 64 * 1024 * 1024);
      const Kernels kernels{execution,
          RasterPipeline(device, library), DepthState(device)};
      TestExecutionLifetime(device, queue, library);
      CompareResidentFrames(device, queue, kernels);
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
