#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <merlin/metal/backend.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace merlin::metal {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kShaderTextureCapacity = 128;
constexpr std::uint32_t kShaderSamplerCapacity = 32;
constexpr std::uint32_t kMaskedAlphaFlag = 0x10000000U;

std::atomic<std::uint64_t> g_owner{1000};

std::string String(NSString *value) {
  if (value == nil) {
    return {};
  }
  const char *utf8 = value.UTF8String;
  return utf8 == nullptr ? std::string{} : std::string(utf8);
}

template <typename Handle>
Handle DecodeHandle(std::uintptr_t value) noexcept {
  return (__bridge Handle)(reinterpret_cast<void *>(value));
}

std::uint64_t DurationNs(Clock::time_point begin, Clock::time_point end) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin)
          .count());
}

std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment) {
  if (alignment == 0) {
    return value;
  }
  return (value + alignment - 1U) / alignment * alignment;
}

std::uint32_t AlignedRowPitch(std::uint32_t width) {
  return static_cast<std::uint32_t>(
      AlignUp(static_cast<std::uint64_t>(width) * 4U, 256U));
}

Mat4 Multiply(const Mat4 &lhs, const Mat4 &rhs) {
  Mat4 result;
  result.values.fill(0.0F);
  for (std::size_t column = 0; column < 4; ++column) {
    for (std::size_t row = 0; row < 4; ++row) {
      for (std::size_t inner = 0; inner < 4; ++inner) {
        result.values[column * 4U + row] +=
            lhs.values[inner * 4U + row] * rhs.values[column * 4U + inner];
      }
    }
  }
  return result;
}

std::array<Vec4, 3> NormalMatrix(const Mat4 &transform) {
  const Vec3 column0{transform.values[0], transform.values[1],
                     transform.values[2]};
  const Vec3 column1{transform.values[4], transform.values[5],
                     transform.values[6]};
  const Vec3 column2{transform.values[8], transform.values[9],
                     transform.values[10]};
  const auto cross = [](const Vec3 &lhs, const Vec3 &rhs) {
    return Vec3{lhs.y * rhs.z - lhs.z * rhs.y, lhs.z * rhs.x - lhs.x * rhs.z,
                lhs.x * rhs.y - lhs.y * rhs.x};
  };
  const auto cofactor0 = cross(column1, column2);
  const auto cofactor1 = cross(column2, column0);
  const auto cofactor2 = cross(column0, column1);
  const auto determinant = column0.x * cofactor0.x + column0.y * cofactor0.y +
                           column0.z * cofactor0.z;
  if (std::abs(determinant) <= 1.0e-20F) {
    return {Vec4{1.0F, 0.0F, 0.0F, 0.0F}, Vec4{0.0F, 1.0F, 0.0F, 0.0F},
            Vec4{0.0F, 0.0F, 1.0F, 0.0F}};
  }
  const auto scale = 1.0F / determinant;
  return {
      Vec4{cofactor0.x * scale, cofactor0.y * scale, cofactor0.z * scale, 0.0F},
      Vec4{cofactor1.x * scale, cofactor1.y * scale, cofactor1.z * scale, 0.0F},
      Vec4{cofactor2.x * scale, cofactor2.y * scale, cofactor2.z * scale,
           0.0F}};
}

struct alignas(16) DrawConstants {
  Mat4 model_view_projection;
  Vec4 normal_matrix_column0;
  Vec4 normal_matrix_column1;
  Vec4 normal_matrix_column2;
  std::uint32_t feature_mask{};
  std::uint32_t prim_id{};
  std::uint32_t instance_id{};
  std::uint32_t texture_index{};
  std::uint32_t sampler_index{};
  std::uint32_t padding[3]{};
};

struct alignas(16) MaterialConstants {
  Vec4 base_color;
  Vec4 light_direction_intensity;
  Vec4 light_color_alpha_cutoff;
};

static_assert(offsetof(extraction::DrawVertex, position) == 0);
static_assert(offsetof(extraction::DrawVertex, normal) == 12);
static_assert(offsetof(extraction::DrawVertex, color) == 24);
static_assert(offsetof(extraction::DrawVertex, texcoord) == 40);
static_assert(sizeof(extraction::DrawVertex) == 48);
static_assert(sizeof(DrawConstants) == 144);
static_assert(sizeof(MaterialConstants) == 48);

const char *kShaderSource = R"METAL(
#include <metal_stdlib>
using namespace metal;

constant uint kDisplayColorFlag = 1u;
constant uint kBaseColorTextureFlag = 2u;
constant uint kDirectionalLightFlag = 4u;
constant uint kMaskedAlphaFlag = 0x10000000u;

struct VertexInput {
  float3 position [[attribute(0)]];
  float3 normal [[attribute(1)]];
  float4 color [[attribute(2)]];
  float2 texcoord [[attribute(3)]];
};

struct DrawConstants {
  float4x4 model_view_projection;
  float4 normal_matrix_column0;
  float4 normal_matrix_column1;
  float4 normal_matrix_column2;
  uint feature_mask;
  uint prim_id;
  uint instance_id;
  uint texture_index;
  uint sampler_index;
  uint3 padding;
};

struct MaterialConstants {
  float4 base_color;
  float4 light_direction_intensity;
  float4 light_color_alpha_cutoff;
};

struct VertexOutput {
  float4 position [[position]];
  float4 color;
  float3 shading_normal;
  float2 texcoord;
};

struct FragmentOutput {
  float4 color [[color(0)]];
  uint prim_id [[color(1)]];
  uint instance_id [[color(2)]];
};

struct ResourceTable {
  array<texture2d<float>, 128> textures [[id(0)]];
  array<sampler, 32> samplers [[id(128)]];
};

vertex VertexOutput merlin_vertex(
    VertexInput input [[stage_in]],
    constant DrawConstants& draw [[buffer(1)]]) {
  VertexOutput output;
  output.position = draw.model_view_projection * float4(input.position, 1.0);
  output.color = ((draw.feature_mask & kDisplayColorFlag) != 0u)
      ? input.color : float4(1.0);
  output.shading_normal =
      draw.normal_matrix_column0.xyz * input.normal.x +
      draw.normal_matrix_column1.xyz * input.normal.y +
      draw.normal_matrix_column2.xyz * input.normal.z;
  output.texcoord = input.texcoord;
  return output;
}

FragmentOutput shade(
    VertexOutput input,
    constant DrawConstants& draw,
    constant MaterialConstants& material,
    float4 texture_sample) {
  float4 color = material.base_color * input.color;
  if ((draw.feature_mask & kBaseColorTextureFlag) != 0u) {
    color *= texture_sample;
  }
  if ((draw.feature_mask & kDirectionalLightFlag) != 0u) {
    float n_dot_l = max(dot(normalize(input.shading_normal),
                            normalize(material.light_direction_intensity.xyz)),
                        0.0);
    color.rgb *= 0.15 + material.light_color_alpha_cutoff.rgb *
        (0.85 * n_dot_l * material.light_direction_intensity.w);
  }
  if ((draw.feature_mask & kMaskedAlphaFlag) != 0u &&
      color.a < material.light_color_alpha_cutoff.a) {
    discard_fragment();
  }
  FragmentOutput output;
  output.color = color;
  output.prim_id = draw.prim_id;
  output.instance_id = draw.instance_id;
  return output;
}

fragment FragmentOutput merlin_fragment_argument_buffer(
    VertexOutput input [[stage_in]],
    constant DrawConstants& draw [[buffer(1)]],
    constant MaterialConstants& material [[buffer(2)]],
    constant ResourceTable& resources [[buffer(3)]]) {
  float4 sample_value = float4(1.0);
  if ((draw.feature_mask & kBaseColorTextureFlag) != 0u) {
    sample_value = resources.textures[draw.texture_index].sample(
        resources.samplers[draw.sampler_index], input.texcoord);
  }
  return shade(input, draw, material, sample_value);
}

fragment FragmentOutput merlin_fragment_conventional(
    VertexOutput input [[stage_in]],
    constant DrawConstants& draw [[buffer(1)]],
    constant MaterialConstants& material [[buffer(2)]],
    texture2d<float> base_color_texture [[texture(0)]],
    sampler base_color_sampler [[sampler(0)]]) {
  float4 sample_value = float4(1.0);
  if ((draw.feature_mask & kBaseColorTextureFlag) != 0u) {
    sample_value = base_color_texture.sample(base_color_sampler, input.texcoord);
  }
  return shade(input, draw, material, sample_value);
}

struct PresentationVertexOutput {
  float4 position [[position]];
  float2 texcoord;
};

vertex PresentationVertexOutput merlin_presentation_vertex(
    uint vertex_id [[vertex_id]]) {
  constexpr float2 positions[] = {
      float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
  constexpr float2 texcoords[] = {
      float2(0.0, 1.0), float2(2.0, 1.0), float2(0.0, -1.0)};
  PresentationVertexOutput output;
  output.position = float4(positions[vertex_id], 0.0, 1.0);
  output.texcoord = texcoords[vertex_id];
  return output;
}

fragment float4 merlin_presentation_fragment(
    PresentationVertexOutput input [[stage_in]],
    texture2d<float> color [[texture(0)]],
    constant uint& color_space [[buffer(0)]]) {
  constexpr sampler linear_sampler(
      coord::normalized, address::clamp_to_edge, filter::linear);
  float4 result = color.sample(linear_sampler, input.texcoord);
  if (color_space == 1u) {
    result.rgb = float3(
        dot(float3(0.82259287, 0.17753395, 0.0), result.rgb),
        dot(float3(0.03319951, 0.96678350, 0.0), result.rgb),
        dot(float3(0.01708535, 0.07239572, 0.91030148), result.rgb));
  }
  return result;
}
)METAL";

std::uint64_t AovBit(Aov aov) {
  return 1ULL << static_cast<std::uint32_t>(aov);
}

bool HasAov(const std::vector<Aov> &values, Aov aov) {
  return std::find(values.begin(), values.end(), aov) != values.end();
}

std::vector<Aov>
ValidateProducts(const std::vector<render::RenderProductRequest> &products,
                 std::vector<Aov> *readbacks) {
  if (products.empty()) {
    throw render::RendererError(render::RendererErrorCode::InvalidRequest,
                                "submit Metal frame",
                                "at least one render product is required");
  }
  std::vector<Aov> rendered;
  for (const auto &product : products) {
    if (product.aov != Aov::Color && product.aov != Aov::Depth &&
        product.aov != Aov::PrimId && product.aov != Aov::InstanceId) {
      throw render::RendererError(
          render::RendererErrorCode::Unsupported, "submit Metal frame",
          "unsupported AOV: " + std::string(AovName(product.aov)));
    }
    if (HasAov(rendered, product.aov)) {
      throw render::RendererError(render::RendererErrorCode::InvalidRequest,
                                  "submit Metal frame",
                                  "duplicate render product");
    }
    rendered.push_back(product.aov);
    if (product.cpu_readback) {
      readbacks->push_back(product.aov);
    }
  }
  return rendered;
}

MTLSamplerMinMagFilter Filter(FilterMode value) {
  return value == FilterMode::Nearest ? MTLSamplerMinMagFilterNearest
                                      : MTLSamplerMinMagFilterLinear;
}

MTLSamplerAddressMode Address(AddressMode value) {
  switch (value) {
  case AddressMode::Repeat:
    return MTLSamplerAddressModeRepeat;
  case AddressMode::MirroredRepeat:
    return MTLSamplerAddressModeMirrorRepeat;
  case AddressMode::ClampToEdge:
    return MTLSamplerAddressModeClampToEdge;
  }
  return MTLSamplerAddressModeClampToEdge;
}

MTLHeapDescriptor *SceneHeapDescriptor(std::uint64_t capacity_bytes) {
  MTLHeapDescriptor *descriptor = [MTLHeapDescriptor new];
  descriptor.size = capacity_bytes;
  descriptor.storageMode = MTLStorageModeShared;
  descriptor.cpuCacheMode = MTLCPUCacheModeWriteCombined;
  descriptor.hazardTrackingMode = MTLHazardTrackingModeTracked;
  return descriptor;
}

} // namespace

AovImageLease::AovImageLease(AovImageLease&& other) noexcept
    : owner_(other.owner_), completion_(other.completion_), aov_(other.aov_) {
  other.owner_ = 0;
  other.completion_ = 0;
}

class Backend::Impl {
public:
  Impl(const render::BackendCreateInfo &info, BackendOptions options)
      : owner_(++g_owner), options_(options),
        texture_slots_(options.texture_capacity),
        sampler_slots_(options.sampler_capacity) {
    @autoreleasepool {
      if (info.frames_in_flight == 0 || info.frames_in_flight > 8) {
        throw render::RendererError(render::RendererErrorCode::InvalidRequest,
                                    "create Metal backend",
                                    "frames_in_flight must be between 1 and 8");
      }
      if (options_.texture_capacity > kShaderTextureCapacity ||
          options_.sampler_capacity > kShaderSamplerCapacity) {
        throw render::RendererError(
            render::RendererErrorCode::InvalidRequest, "create Metal backend",
            "resource table capacity exceeds the compiled argument-buffer ABI");
      }
      if (options_.heap_capacity_bytes < 1024U * 1024U) {
        throw render::RendererError(render::RendererErrorCode::InvalidRequest,
                                    "create Metal backend",
                                    "heap capacity must be at least 1 MiB");
      }
      if (options_.presentation &&
          (options_.presentation->layer == 0 ||
           options_.presentation->drawable_count < 2 ||
           options_.presentation->drawable_count > 3)) {
        throw render::RendererError(
            render::RendererErrorCode::InvalidRequest,
            "create Metal presentation",
            "presentation requires a CAMetalLayer and 2 or 3 drawables");
      }
      if (options_.presentation &&
          options_.presentation->dynamic_range !=
              PresentationDynamicRange::Standard) {
        throw render::RendererError(
            render::RendererErrorCode::Unsupported,
            "create Metal presentation",
            "extended-range presentation is reserved for the future HDR "
            "contract");
      }

      device_ = MTLCreateSystemDefaultDevice();
      if (device_ == nil) {
        throw render::RendererError(
            render::RendererErrorCode::BackendUnavailable,
            "create Metal device", "no Metal device is available");
      }
      queue_ = [device_ newCommandQueue];
      if (queue_ == nil) {
        throw render::RendererError(render::RendererErrorCode::BackendFailure,
                                    "create Metal command queue",
                                    "newCommandQueue returned nil");
      }

      bindless_ = device_.argumentBuffersSupport == MTLArgumentBuffersTier2;
      CreateLibraryAndPipeline();
      CreateDepthState();
      CreateHeap();
      if (options_.presentation) {
        ConfigurePresentation(*options_.presentation);
      }
      completion_event_ = [device_ newSharedEvent];
      if (completion_event_ == nil) {
        throw render::RendererError(
            render::RendererErrorCode::BackendFailure,
            "create Metal completion event", "newSharedEvent returned nil");
      }

      frames_.resize(info.frames_in_flight);
      for (auto &frame : frames_) {
        frame.encoded_textures.resize(options_.texture_capacity);
        frame.encoded_texture_revisions.resize(options_.texture_capacity);
        frame.encoded_samplers.resize(options_.sampler_capacity);
        frame.encoded_sampler_revisions.resize(options_.sampler_capacity);
        if (bindless_) {
          frame.argument_buffer =
              [device_ newBufferWithLength:argument_encoder_.encodedLength
                                   options:MTLResourceStorageModeShared];
          if (frame.argument_buffer == nil) {
            throw render::RendererError(
                render::RendererErrorCode::ResourceExhausted,
                "allocate Metal argument buffer", "newBuffer returned nil");
          }
        }
      }

      capabilities_.backend = render::BackendKind::Metal;
      capabilities_.backend_name = "metal";
      capabilities_.device_name = String(device_.name);
      capabilities_.bindless_textures = bindless_;
      capabilities_.asynchronous_upload = false;
      capabilities_.timestamp_queries = true;
      capabilities_.external_presentation = layer_ != nil;
      capabilities_.cpu_readback = true;
      capabilities_.validation_enabled = info.enable_validation;
      capabilities_.generated_materials = false;
      capabilities_.limits.max_image_dimension_2d = 16384;
      capabilities_.limits.max_frames_in_flight = info.frames_in_flight;
      capabilities_.limits.sampled_image_slots = options_.texture_capacity;
      capabilities_.limits.sampler_slots = options_.sampler_capacity;

      metal_statistics_.frame_context_count = info.frames_in_flight;
      metal_statistics_.heap_capacity_bytes = options_.heap_capacity_bytes;
    }
  }

  ~Impl() {
    @autoreleasepool {
      for (auto &[value, pending] : pending_) {
        (void)value;
        [pending.command waitUntilCompleted];
      }
      NotifyOverlay(PresentationOverlayPhase::Shutdown);
    }
  }

  const render::RendererCapabilities &capabilities() const noexcept {
    return capabilities_;
  }

  render::RendererStatistics statistics() const noexcept {
    auto result = statistics_;
    const auto metal = metal_statistics();
    result.uploaded_bytes = uploaded_bytes_;
    result.readback_bytes = readback_bytes_;
    result.presentation_copy_bytes = presentation_copy_bytes_;
    result.aov_image_export_count = aov_image_export_count_;
    result.active_aov_image_leases = active_aov_image_leases_;
    result.residency.bindless_tables = bindless_;
    result.residency.vram_heap_capacity_bytes = metal.heap_capacity_bytes;
    result.residency.vram_heap_budget_bytes = metal.heap_capacity_bytes;
    result.residency.vram_heap_usage_bytes = metal.heap_resident_bytes;
    result.residency.vram_heap_available_bytes =
        metal.heap_capacity_bytes > metal.heap_resident_bytes
            ? metal.heap_capacity_bytes - metal.heap_resident_bytes
            : 0;
    result.residency.configured_vram_limit_bytes = options_.heap_capacity_bytes;
    result.residency.effective_vram_limit_bytes = options_.heap_capacity_bytes;
    result.residency.renderer_allocated_bytes = metal.heap_resident_bytes;
    result.residency.renderer_peak_allocated_bytes =
        metal.heap_peak_resident_bytes;
    result.residency.geometry_resident_bytes = GeometryResidentBytes();
    result.residency.geometry_peak_resident_bytes =
        geometry_peak_resident_bytes_;
    result.residency.geometry_retiring_bytes = RetiringBytes();
    result.residency.texture_slots_capacity = metal.texture_slots.capacity;
    result.residency.texture_slots_in_use = metal.texture_slots.in_use;
    result.residency.texture_slots_retiring = metal.texture_slots.retiring;
    result.residency.sampler_slots_capacity = metal.sampler_slots.capacity;
    result.residency.sampler_slots_in_use = metal.sampler_slots.in_use;
    result.residency.sampler_slots_retiring = metal.sampler_slots.retiring;
    result.residency.unique_sampler_count = metal.sampler_slots.in_use;
    return result;
  }

  MetalStatistics metal_statistics() const noexcept {
    auto result = metal_statistics_;
    result.texture_slots = texture_slots_.telemetry();
    result.sampler_slots = sampler_slots_.telemetry();
    return result;
  }

  std::optional<render::PresentationTarget>
  default_presentation_target() const noexcept {
    return presentation_;
  }

  void ResizePresentationTarget(render::PresentationTarget target,
                                std::uint32_t width, std::uint32_t height) {
    ValidatePresentation(target, "resize Metal presentation");
    ValidatePresentationExtent(width, height, "resize Metal presentation");
    UpdatePresentationExtent(width, height);
  }

  render::CompletionToken Submit(const render::RenderRequest &request) {
    @autoreleasepool {
      const auto submit_begin = Clock::now();
      if (!request.snapshot) {
        throw render::RendererError(render::RendererErrorCode::InvalidRequest,
                                    "submit Metal frame", "snapshot is null");
      }
      if (request.width == 0 || request.height == 0 ||
          request.width > capabilities_.limits.max_image_dimension_2d ||
          request.height > capabilities_.limits.max_image_dimension_2d) {
        throw render::RendererError(render::RendererErrorCode::InvalidRequest,
                                    "submit Metal frame",
                                    "render extent is invalid");
      }
      if (request.presentation) {
        ValidatePresentation(request.presentation, "submit Metal frame");
        UpdatePresentationExtent(request.width, request.height);
      }

      std::vector<Aov> readbacks;
      auto rendered = ValidateProducts(request.products, &readbacks);
      CollectRetirements();

      auto context_index = next_context_;
      std::size_t searched{};
      while (searched < frames_.size() && frames_[context_index].busy) {
        context_index = (context_index + 1U) % frames_.size();
        ++searched;
      }
      if (searched == frames_.size()) {
        throw render::RendererError(render::RendererErrorCode::ResourceBusy,
                                    "submit Metal frame",
                                    "all frames-in-flight are unresolved");
      }
      auto &frame = frames_[context_index];
      next_context_ = (context_index + 1U) % frames_.size();

      FrameBuild build;
      Reconcile(*request.snapshot, build);
      EnsureTargets(frame, request.width, request.height, build);
      if (bindless_) {
        EncodeArgumentBuffer(frame, build);
      }

      id<MTLCommandBuffer> command = [queue_ commandBuffer];
      if (command == nil) {
        throw render::RendererError(render::RendererErrorCode::BackendFailure,
                                    "create Metal command buffer",
                                    "commandBuffer returned nil");
      }
      command.label = @"hdMerlin offscreen frame";

      const auto record_begin = Clock::now();
      EncodeRender(command, frame, request, build);
      id<CAMetalDrawable> drawable;
      if (request.presentation) {
        const auto presentation_begin = Clock::now();
        drawable = [layer_ nextDrawable];
        if (drawable == nil) {
          throw render::RendererError(
              render::RendererErrorCode::ResourceBusy,
              "acquire Metal drawable",
              "CAMetalLayer returned no drawable before its timeout");
        }
        EncodePresentation(command, frame, drawable, build);
        [command presentDrawable:drawable];
        build.presentation_ns =
            DurationNs(presentation_begin, Clock::now());
      }
      EncodeReadback(command, frame, readbacks);
      const auto record_end = Clock::now();

      const auto value = ++submitted_value_;
      dispatch_semaphore_t completion = dispatch_semaphore_create(0);
      auto *completed = &completed_value_;
      [command addCompletedHandler:^(id<MTLCommandBuffer>) {
        completed->store(value, std::memory_order_release);
        dispatch_semaphore_signal(completion);
      }];
      if (completion_event_ != nil) {
        [command encodeSignalEvent:completion_event_ value:value];
      }

      const auto queue_begin = Clock::now();
      [command commit];
      const auto queue_end = Clock::now();

      frame.busy = true;
      frame.completion_value = value;

      Pending pending;
      pending.command = command;
      pending.completion = completion;
      pending.context_index = context_index;
      pending.snapshot = request.snapshot;
      pending.width = request.width;
      pending.height = request.height;
      pending.rendered_aovs = std::move(rendered);
      pending.readback_aovs = std::move(readbacks);
      pending.drawable = drawable;
      pending.result.scene_revision = request.snapshot->revision;
      pending.result.completion_value = value;
      pending.result.telemetry = build.telemetry;
      pending.result.timings.upload_ns = build.upload_ns;
      pending.result.timings.command_recording_ns =
          DurationNs(record_begin, record_end);
      pending.result.timings.presentation_ns = build.presentation_ns;
      pending.result.timings.queue_submission_ns =
          DurationNs(queue_begin, queue_end);
      pending.result.timings.backend_total_ns =
          DurationNs(submit_begin, queue_end);
      pending.result.material_diagnostics =
          std::move(build.material_diagnostics);
      pending_.emplace(value, std::move(pending));

      ++statistics_.frames_submitted;
      uploaded_bytes_ += build.telemetry.upload_bytes;
      return render::CompletionToken(owner_, value);
    }
  }

  bool IsComplete(render::CompletionToken token) const {
    const auto &pending = ValidateToken(token, "query Metal completion");
    return pending.command.status == MTLCommandBufferStatusCompleted ||
           pending.command.status == MTLCommandBufferStatusError;
  }

  AovImageExport AcquireAovImage(render::CompletionToken token, Aov aov) {
    @autoreleasepool {
      auto &pending = ValidateToken(token, "acquire Metal AOV image");
      const auto mask = std::uint64_t{1} << static_cast<std::uint32_t>(aov);
      auto &frame = frames_[pending.context_index];
      if (frame.exported_aov_mask & mask) {
        throw render::RendererError(
            render::RendererErrorCode::ResourceBusy,
            "acquire Metal AOV image", "AOV image already has an active export lease");
      }
      if (!HasAov(pending.rendered_aovs, aov)) {
        throw render::RendererError(
            render::RendererErrorCode::InvalidRequest,
            "acquire Metal AOV image", "AOV was not selected for this submission");
      }
      id<MTLTexture> texture = nil;
      MTLPixelFormat format = MTLPixelFormatInvalid;
      MTLTextureUsage usage = MTLTextureUsageUnknown;
      switch (aov) {
      case Aov::Color:
        texture = frame.color;
        format = MTLPixelFormatRGBA8Unorm;
        usage = MTLTextureUsageRenderTarget;
        break;
      case Aov::Depth:
        texture = frame.depth;
        format = MTLPixelFormatDepth32Float;
        usage = MTLTextureUsageRenderTarget;
        break;
      case Aov::PrimId:
        texture = frame.prim_id;
        format = MTLPixelFormatR32Uint;
        usage = MTLTextureUsageRenderTarget;
        break;
      case Aov::InstanceId:
        texture = frame.instance_id;
        format = MTLPixelFormatR32Uint;
        usage = MTLTextureUsageRenderTarget;
        break;
      default:
        throw render::RendererError(
            render::RendererErrorCode::Unsupported,
            "acquire Metal AOV image", "AOV has no Metal export image");
      }
      if (texture == nil) {
        throw render::RendererError(
            render::RendererErrorCode::BackendFailure,
            "acquire Metal AOV image", "selected AOV image is unavailable");
      }
      frame.exported_aov_mask |= mask;
      ++aov_image_export_count_;
      ++active_aov_image_leases_;
      ++pending.result.telemetry.aov_image_export_count;
      return AovImageExport{
          AovImageLease(owner_, pending.result.completion_value, aov),
          MakeRenderProduct(pending.width, pending.height, aov),
          reinterpret_cast<std::uintptr_t>((__bridge void*)device_),
          reinterpret_cast<std::uintptr_t>((__bridge void*)queue_),
          reinterpret_cast<std::uintptr_t>((__bridge void*)texture),
          reinterpret_cast<std::uintptr_t>((__bridge void*)completion_event_),
          static_cast<std::uint32_t>(format),
          static_cast<std::uint32_t>(usage),
          static_cast<std::uint32_t>(texture.storageMode),
          pending.result.completion_value};
    }
  }

  void ReleaseAovImage(AovImageLease&& lease) {
    @autoreleasepool {
      if (!lease) {
        return;
      }
      for (auto &frame : frames_) {
        if (frame.completion_value != lease.completion_value()) {
          continue;
        }
        const auto mask = std::uint64_t{1} <<
                          static_cast<std::uint32_t>(lease.aov());
        if ((frame.exported_aov_mask & mask) == 0) {
          return;
        }
        frame.exported_aov_mask &= ~mask;
        if (active_aov_image_leases_ != 0) {
          --active_aov_image_leases_;
        }
        if (frame.exported_aov_mask == 0 &&
            pending_.find(frame.completion_value) == pending_.end()) {
          frame.busy = false;
          frame.completion_value = 0;
        }
        lease.Reset();
        return;
      }
    }
  }

  render::RenderResult Resolve(render::CompletionToken token,
                               std::chrono::nanoseconds timeout) {
    @autoreleasepool {
      auto &pending = ValidateToken(token, "resolve Metal frame");
      const auto wait_begin = Clock::now();
      if (pending.command.status != MTLCommandBufferStatusCompleted &&
          pending.command.status != MTLCommandBufferStatusError) {
        if (timeout == std::chrono::nanoseconds::max()) {
          [pending.command waitUntilCompleted];
        } else {
          const auto count = std::max<std::int64_t>(timeout.count(), 0);
          const auto deadline = dispatch_time(DISPATCH_TIME_NOW, count);
          if (dispatch_semaphore_wait(pending.completion, deadline) != 0) {
            throw render::RendererError(render::RendererErrorCode::Timeout,
                                        "resolve Metal frame",
                                        "completion wait timed out");
          }
        }
      }
      pending.result.timings.completion_wait_ns =
          DurationNs(wait_begin, Clock::now());
      if (pending.command.status == MTLCommandBufferStatusError) {
        ++statistics_.validation_messages;
        const auto detail =
            pending.command.error == nil
                ? std::string("Metal command buffer failed")
                : String(pending.command.error.localizedDescription);
        auto &failed_frame = frames_[pending.context_index];
        // A bridge blit may still be waiting on this frame's completion event.
        // Keep the frame identified until that lease callback releases it.
        failed_frame.busy = failed_frame.exported_aov_mask != 0;
        if (!failed_frame.busy) {
          failed_frame.completion_value = 0;
        }
        const auto native_code =
            static_cast<std::int32_t>(pending.command.error.code);
        pending_.erase(token.value());
        CollectRetirements();
        throw render::RendererError(render::RendererErrorCode::BackendFailure,
                                    "execute Metal frame", detail, native_code);
      }

      const auto readback_begin = Clock::now();
      auto &frame = frames_[pending.context_index];
      pending.result.rendered_aovs = pending.rendered_aovs;
      pending.result.cpu_readback_aovs = pending.readback_aovs;
      CopyReadbacks(frame, pending);
      pending.result.timings.readback_ns =
          DurationNs(readback_begin, Clock::now());
      if (pending.command.GPUEndTime >= pending.command.GPUStartTime) {
        pending.result.timings.gpu_execution_ns = static_cast<std::uint64_t>(
            (pending.command.GPUEndTime - pending.command.GPUStartTime) *
            1.0e9);
      }
      pending.result.timings.backend_total_ns +=
          pending.result.timings.completion_wait_ns +
          pending.result.timings.readback_ns;

      auto result = std::move(pending.result);
      // An exported texture is retained until the bridge command buffer
      // completes. Resolve may return before that callback, so the frame
      // context remains unavailable for reuse while any lease is active.
      frame.busy = frame.exported_aov_mask != 0;
      if (!frame.busy) {
        frame.completion_value = 0;
      }
      readback_bytes_ += result.telemetry.readback_bytes;
      if (pending.drawable != nil) {
        ++statistics_.frames_presented;
        presentation_copy_bytes_ +=
            result.telemetry.presentation_copy_bytes;
      }
      pending_.erase(token.value());
      CollectRetirements();
      return result;
    }
  }

private:
  struct Geometry {
    std::uint64_t vertex_revision{};
    std::uint64_t index_revision{};
    id<MTLBuffer> vertices;
    id<MTLBuffer> indices;
    std::uint32_t index_count{};
    std::uint64_t bytes{};
  };

  struct Texture {
    std::uint64_t revision{};
    id<MTLTexture> texture;
    std::uint64_t bytes{};
  };

  struct Sampler {
    std::uint64_t revision{};
    id<MTLSamplerState> sampler;
  };

  struct Retirement {
    std::uint64_t completion_value{};
    std::uint64_t bytes{};
    id<MTLResource> first;
    id<MTLResource> second;
  };

  struct FrameContext {
    bool busy{};
    std::uint64_t completion_value{};
    std::uint64_t exported_aov_mask{};
    std::uint32_t width{};
    std::uint32_t height{};
    id<MTLTexture> color;
    id<MTLTexture> depth;
    id<MTLTexture> prim_id;
    id<MTLTexture> instance_id;
    id<MTLBuffer> color_readback;
    id<MTLBuffer> depth_readback;
    id<MTLBuffer> prim_readback;
    id<MTLBuffer> instance_readback;
    id<MTLBuffer> argument_buffer;
    std::vector<std::uint64_t> encoded_textures;
    std::vector<std::uint64_t> encoded_texture_revisions;
    std::vector<std::uint64_t> encoded_samplers;
    std::vector<std::uint64_t> encoded_sampler_revisions;
  };

  struct Pending {
    id<MTLCommandBuffer> command;
    id<CAMetalDrawable> drawable;
    dispatch_semaphore_t completion;
    std::size_t context_index{};
    std::shared_ptr<const extraction::FrameSnapshot> snapshot;
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<Aov> rendered_aovs;
    std::vector<Aov> readback_aovs;
    render::RenderResult result;
  };

  struct FrameBuild {
    render::FrameTelemetry telemetry;
    std::uint64_t upload_ns{};
    std::uint64_t presentation_ns{};
    std::vector<MaterialDiagnostic> material_diagnostics;
  };

  void CreateLibraryAndPipeline() {
    MTLCompileOptions *options = [MTLCompileOptions new];
    options.fastMathEnabled = YES;
    NSError *error = nil;
    library_ = [device_
        newLibraryWithSource:[NSString stringWithUTF8String:kShaderSource]
                     options:options
                       error:&error];
    if (library_ == nil) {
      throw render::RendererError(
          render::RendererErrorCode::BackendFailure, "compile Metal shader",
          error == nil ? "newLibraryWithSource returned nil"
                       : String(error.localizedDescription),
          static_cast<std::int32_t>(error.code));
    }
    id<MTLFunction> vertex = [library_ newFunctionWithName:@"merlin_vertex"];
    NSString *fragment_name = bindless_ ? @"merlin_fragment_argument_buffer"
                                        : @"merlin_fragment_conventional";
    id<MTLFunction> fragment = [library_ newFunctionWithName:fragment_name];
    if (vertex == nil || fragment == nil) {
      throw render::RendererError(render::RendererErrorCode::BackendFailure,
                                  "load Metal shader entry point",
                                  "compiled entry point is missing");
    }

    MTLVertexDescriptor *vertices = [MTLVertexDescriptor vertexDescriptor];
    vertices.attributes[0].format = MTLVertexFormatFloat3;
    vertices.attributes[0].offset = offsetof(extraction::DrawVertex, position);
    vertices.attributes[0].bufferIndex = 0;
    vertices.attributes[1].format = MTLVertexFormatFloat3;
    vertices.attributes[1].offset = offsetof(extraction::DrawVertex, normal);
    vertices.attributes[1].bufferIndex = 0;
    vertices.attributes[2].format = MTLVertexFormatFloat4;
    vertices.attributes[2].offset = offsetof(extraction::DrawVertex, color);
    vertices.attributes[2].bufferIndex = 0;
    vertices.attributes[3].format = MTLVertexFormatFloat2;
    vertices.attributes[3].offset = offsetof(extraction::DrawVertex, texcoord);
    vertices.attributes[3].bufferIndex = 0;
    vertices.layouts[0].stride = sizeof(extraction::DrawVertex);
    vertices.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.label = @"hdMerlin Forward";
    descriptor.vertexFunction = vertex;
    descriptor.fragmentFunction = fragment;
    descriptor.vertexDescriptor = vertices;
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    descriptor.colorAttachments[1].pixelFormat = MTLPixelFormatR32Uint;
    descriptor.colorAttachments[2].pixelFormat = MTLPixelFormatR32Uint;
    descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    pipeline_ = [device_ newRenderPipelineStateWithDescriptor:descriptor
                                                        error:&error];
    if (pipeline_ == nil) {
      throw render::RendererError(render::RendererErrorCode::BackendFailure,
                                  "create Metal render pipeline",
                                  error == nil
                                      ? "newRenderPipelineState returned nil"
                                      : String(error.localizedDescription),
                                  static_cast<std::int32_t>(error.code));
    }
    if (bindless_) {
      argument_encoder_ = [fragment newArgumentEncoderWithBufferIndex:3];
      if (argument_encoder_ == nil) {
        throw render::RendererError(render::RendererErrorCode::BackendFailure,
                                    "create Metal argument encoder",
                                    "fragment argument encoder is unavailable");
      }
    }
    if (options_.presentation) {
      id<MTLFunction> presentation_vertex =
          [library_ newFunctionWithName:@"merlin_presentation_vertex"];
      id<MTLFunction> presentation_fragment =
          [library_ newFunctionWithName:@"merlin_presentation_fragment"];
      MTLRenderPipelineDescriptor *presentation =
          [MTLRenderPipelineDescriptor new];
      presentation.label = @"hdMerlin native presentation";
      presentation.vertexFunction = presentation_vertex;
      presentation.fragmentFunction = presentation_fragment;
      presentation.colorAttachments[0].pixelFormat =
          MTLPixelFormatBGRA8Unorm_sRGB;
      presentation_pipeline_ =
          [device_ newRenderPipelineStateWithDescriptor:presentation
                                                  error:&error];
      if (presentation_pipeline_ == nil) {
        throw render::RendererError(
            render::RendererErrorCode::BackendFailure,
            "create Metal presentation pipeline",
            error == nil ? "newRenderPipelineState returned nil"
                         : String(error.localizedDescription),
            static_cast<std::int32_t>(error.code));
      }
    }
  }

  void ConfigurePresentation(const PresentationOptions &options) {
    layer_ = DecodeHandle<CAMetalLayer *>(options.layer);
    layer_.device = device_;
    layer_.pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
    layer_.framebufferOnly = YES;
    layer_.displaySyncEnabled = options.vsync ? YES : NO;
    layer_.maximumDrawableCount = options.drawable_count;
    layer_.allowsNextDrawableTimeout = YES;
    layer_.presentsWithTransaction = NO;
    presentation_color_space_ = options.color_space;
    CGColorSpaceRef color_space = CGColorSpaceCreateWithName(
        options.color_space == PresentationColorSpace::DisplayP3
            ? kCGColorSpaceDisplayP3
            : kCGColorSpaceSRGB);
    layer_.colorspace = color_space;
    CGColorSpaceRelease(color_space);
    presentation_ = render::PresentationTarget(owner_, 1);
    NotifyOverlay(PresentationOverlayPhase::Initialize);
  }

  void NotifyOverlay(PresentationOverlayPhase phase,
                     id<MTLCommandBuffer> command = nil,
                     id<MTLRenderCommandEncoder> encoder = nil,
                     MTLRenderPassDescriptor *pass = nil) {
    if (!options_.presentation ||
        options_.presentation->render_overlay == nullptr) {
      return;
    }
    PresentationOverlayContext context;
    context.phase = phase;
    context.device =
        reinterpret_cast<std::uintptr_t>((__bridge void *)device_);
    context.command_buffer =
        reinterpret_cast<std::uintptr_t>((__bridge void *)command);
    context.render_encoder =
        reinterpret_cast<std::uintptr_t>((__bridge void *)encoder);
    context.render_pass_descriptor =
        reinterpret_cast<std::uintptr_t>((__bridge void *)pass);
    options_.presentation->render_overlay(
        options_.presentation->overlay_user_data, context);
  }

  void ValidatePresentation(render::PresentationTarget target,
                            const char *operation) const {
    if (!presentation_ || target != *presentation_) {
      throw render::RendererError(
          render::RendererErrorCode::InvalidRequest, operation,
          "presentation target belongs to another backend");
    }
  }

  void ValidatePresentationExtent(std::uint32_t width, std::uint32_t height,
                                  const char *operation) const {
    if (width == 0 || height == 0 ||
        width > capabilities_.limits.max_image_dimension_2d ||
        height > capabilities_.limits.max_image_dimension_2d) {
      throw render::RendererError(render::RendererErrorCode::InvalidRequest,
                                  operation,
                                  "presentation extent is invalid");
    }
  }

  void UpdatePresentationExtent(std::uint32_t width, std::uint32_t height) {
    ValidatePresentationExtent(width, height, "resize Metal presentation");
    if (presentation_width_ == width && presentation_height_ == height) {
      return;
    }
    if (presentation_width_ != 0 && presentation_height_ != 0) {
      ++statistics_.presentation_recreates;
    }
    presentation_width_ = width;
    presentation_height_ = height;
    layer_.drawableSize = CGSizeMake(width, height);
  }

  void CreateDepthState() {
    MTLDepthStencilDescriptor *descriptor = [MTLDepthStencilDescriptor new];
    descriptor.depthCompareFunction = MTLCompareFunctionLessEqual;
    descriptor.depthWriteEnabled = YES;
    depth_state_ = [device_ newDepthStencilStateWithDescriptor:descriptor];
    if (depth_state_ == nil) {
      throw render::RendererError(render::RendererErrorCode::BackendFailure,
                                  "create Metal depth state",
                                  "newDepthStencilState returned nil");
    }
  }

  void CreateHeap() {
    MTLHeapDescriptor *descriptor =
        SceneHeapDescriptor(options_.heap_capacity_bytes);
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

  id<MTLBuffer> HeapBuffer(std::uint64_t length, std::uint64_t *charged,
                           FrameBuild &build) {
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

  id<MTLTexture> HeapTexture(MTLTextureDescriptor *descriptor,
                             std::uint64_t *charged, FrameBuild &build) {
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

  void ChargeAllocation(std::uint64_t bytes, FrameBuild &build) {
    metal_statistics_.heap_resident_bytes += bytes;
    metal_statistics_.heap_peak_resident_bytes =
        std::max(metal_statistics_.heap_peak_resident_bytes,
                 metal_statistics_.heap_resident_bytes);
    ++metal_statistics_.heap_allocation_count;
    ++build.telemetry.allocation_count;
    build.telemetry.buffer_allocation_bytes += bytes;
  }

  void Retire(id<MTLResource> first, id<MTLResource> second,
              std::uint64_t bytes) {
    if (first == nil && second == nil) {
      return;
    }
    retirements_.push_back({submitted_value_, bytes, first, second});
    ++metal_statistics_.scene_resource_retirements;
  }

  void CollectRetirements() {
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

  void Reconcile(const extraction::FrameSnapshot &snapshot, FrameBuild &build) {
    const auto begin = Clock::now();
    std::unordered_set<std::uint64_t> active;
    for (const auto &record : snapshot.geometries) {
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
    for (const auto &record : snapshot.geometries) {
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
    for (const auto &record : snapshot.textures) {
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
    for (const auto &record : snapshot.textures) {
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
      MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
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
    for (const auto &record : snapshot.samplers) {
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
    for (const auto &record : snapshot.samplers) {
      const auto found = samplers_.find(record.sampler);
      if (found != samplers_.end() &&
          found->second.revision == record.revision) {
        continue;
      }
      MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
      descriptor.minFilter = Filter(record.min_filter);
      descriptor.magFilter = Filter(record.mag_filter);
      descriptor.sAddressMode = Address(record.address_u);
      descriptor.tAddressMode = Address(record.address_v);
      descriptor.normalizedCoordinates = YES;
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

  ResourceSlot AcquireSlot(StableResourceTable &table, std::uint64_t resource,
                           const char *operation) {
    try {
      return table.Acquire(resource, CompletedValue());
    } catch (const std::length_error &) {
      const auto telemetry = table.telemetry();
      throw render::RendererError(
          render::RendererErrorCode::ResourceExhausted, operation,
          "stable resource table exhausted; capacity=" +
              std::to_string(telemetry.capacity) +
              ", in_use=" + std::to_string(telemetry.in_use) +
              ", retiring=" + std::to_string(telemetry.retiring));
    }
  }

  std::uint64_t CompletedValue() const noexcept {
    return completed_value_.load(std::memory_order_acquire);
  }

  void EnsureTargets(FrameContext &frame, std::uint32_t width,
                     std::uint32_t height, FrameBuild &build) {
    if (frame.width == width && frame.height == height && frame.color != nil) {
      return;
    }
    auto texture = [&](MTLPixelFormat format, MTLTextureUsage usage) {
      MTLTextureDescriptor *descriptor =
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
          static_cast<std::uint64_t>(width) * height * 4U;
      return value;
    };
    id<MTLTexture> color =
        texture(MTLPixelFormatRGBA8Unorm, MTLTextureUsageRenderTarget);
    id<MTLTexture> depth =
        texture(MTLPixelFormatDepth32Float, MTLTextureUsageRenderTarget);
    id<MTLTexture> prim_id =
        texture(MTLPixelFormatR32Uint, MTLTextureUsageRenderTarget);
    id<MTLTexture> instance_id =
        texture(MTLPixelFormatR32Uint, MTLTextureUsageRenderTarget);
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
    frame.depth = depth;
    frame.prim_id = prim_id;
    frame.instance_id = instance_id;
    frame.color_readback = color_readback;
    frame.depth_readback = depth_readback;
    frame.prim_readback = prim_readback;
    frame.instance_readback = instance_readback;
  }

  void EncodeArgumentBuffer(FrameContext &frame, FrameBuild &build) {
    [argument_encoder_ setArgumentBuffer:frame.argument_buffer offset:0];
    std::vector<const Texture *> by_texture(options_.texture_capacity);
    std::vector<std::uint64_t> texture_handles(options_.texture_capacity);
    for (const auto &[handle, texture] : textures_) {
      const auto slot = texture_slots_.Find(handle);
      if (slot) {
        by_texture[slot->index] = &texture;
        texture_handles[slot->index] = handle;
      }
    }
    std::vector<const Sampler *> by_sampler(options_.sampler_capacity);
    std::vector<std::uint64_t> sampler_handles(options_.sampler_capacity);
    for (const auto &[handle, sampler] : samplers_) {
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

  static MaterialConstants
  MakeMaterial(const extraction::MaterialRecord &material,
               const extraction::FrameSnapshot &snapshot) {
    MaterialConstants result{};
    result.base_color = material.parameters.base_color;
    result.light_direction_intensity = {0.0F, 0.0F, 1.0F, 1.0F};
    result.light_color_alpha_cutoff = {1.0F, 1.0F, 1.0F,
                                       material.parameters.alpha_cutoff};
    const auto light =
        std::find_if(snapshot.lights.begin(), snapshot.lights.end(),
                     [](const auto &candidate) {
                       return candidate.type == LightType::Directional;
                     });
    if (light != snapshot.lights.end()) {
      auto x = light->transform.values[8];
      auto y = light->transform.values[9];
      auto z = light->transform.values[10];
      const auto length = std::sqrt(x * x + y * y + z * z);
      if (length > 0.0F) {
        x /= length;
        y /= length;
        z /= length;
      }
      result.light_direction_intensity = {x, y, z, light->intensity};
      result.light_color_alpha_cutoff.x = light->color.x;
      result.light_color_alpha_cutoff.y = light->color.y;
      result.light_color_alpha_cutoff.z = light->color.z;
    }
    return result;
  }

  void EncodeRender(id<MTLCommandBuffer> command, FrameContext &frame,
                    const render::RenderRequest &request, FrameBuild &build) {
    MTLRenderPassDescriptor *pass =
        [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = frame.color;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor =
        MTLClearColorMake(request.clear_color.x, request.clear_color.y,
                          request.clear_color.z, request.clear_color.w);
    pass.colorAttachments[1].texture = frame.prim_id;
    pass.colorAttachments[1].loadAction = MTLLoadActionClear;
    pass.colorAttachments[1].storeAction = MTLStoreActionStore;
    pass.colorAttachments[1].clearColor =
        MTLClearColorMake(std::numeric_limits<std::uint32_t>::max(), 0, 0, 0);
    pass.colorAttachments[2].texture = frame.instance_id;
    pass.colorAttachments[2].loadAction = MTLLoadActionClear;
    pass.colorAttachments[2].storeAction = MTLStoreActionStore;
    pass.colorAttachments[2].clearColor =
        MTLClearColorMake(std::numeric_limits<std::uint32_t>::max(), 0, 0, 0);
    pass.depthAttachment.texture = frame.depth;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.depthAttachment.clearDepth = 1.0;

    id<MTLRenderCommandEncoder> encoder =
        [command renderCommandEncoderWithDescriptor:pass];
    if (encoder == nil) {
      throw render::RendererError(render::RendererErrorCode::BackendFailure,
                                  "encode Metal render pass",
                                  "renderCommandEncoder returned nil");
    }
    [encoder setRenderPipelineState:pipeline_];
    [encoder setDepthStencilState:depth_state_];
    const MTLViewport viewport{0.0,
                               0.0,
                               static_cast<double>(request.width),
                               static_cast<double>(request.height),
                               0.0,
                               1.0};
    const MTLScissorRect scissor{0, 0, request.width, request.height};
    [encoder setViewport:viewport];
    [encoder setScissorRect:scissor];
    [encoder setFrontFacingWinding:request.snapshot->front_face ==
                                           FrontFaceWinding::Clockwise
                                       ? MTLWindingCounterClockwise
                                       : MTLWindingClockwise];
    if (bindless_) {
      [encoder setFragmentBuffer:frame.argument_buffer offset:0 atIndex:3];
      for (const auto &[handle, texture] : textures_) {
        (void)handle;
        [encoder useResource:texture.texture
                       usage:MTLResourceUsageRead
                      stages:MTLRenderStageFragment];
      }
    }

    const auto view_projection =
        Multiply(request.snapshot->projection, request.snapshot->view);
    for (const auto &draw : request.snapshot->draws) {
      if (draw.geometry_index >= request.snapshot->geometries.size() ||
          draw.material_index >= request.snapshot->materials.size() ||
          draw.instance_index >= request.snapshot->instances.size()) {
        [encoder endEncoding];
        throw render::RendererError(render::RendererErrorCode::InvalidRequest,
                                    "encode Metal draw",
                                    "draw record index is invalid");
      }
      const auto &geometry_record =
          request.snapshot->geometries[draw.geometry_index];
      const auto geometry = geometries_.find(geometry_record.mesh);
      if (geometry == geometries_.end()) {
        continue;
      }
      const auto &instance = request.snapshot->instances[draw.instance_index];
      if (!instance.visible) {
        continue;
      }
      const auto &material = request.snapshot->materials[draw.material_index];
      [encoder setCullMode:material.double_sided ? MTLCullModeNone
                                                 : MTLCullModeBack];

      DrawConstants constants;
      constants.model_view_projection =
          Multiply(view_projection, instance.transform);
      const auto normal = NormalMatrix(instance.transform);
      constants.normal_matrix_column0 = normal[0];
      constants.normal_matrix_column1 = normal[1];
      constants.normal_matrix_column2 = normal[2];
      constants.feature_mask = static_cast<std::uint32_t>(material.features);
      if (material.alpha_mode == AlphaMode::Masked) {
        constants.feature_mask |= kMaskedAlphaFlag;
      } else if (material.alpha_mode == AlphaMode::Blended) {
        constants.feature_mask &=
            ~static_cast<std::uint32_t>(MaterialFeature::BaseColorTexture);
        build.telemetry.material_fallbacks.Record(
            MaterialFallback::BasicMaterial);
      }
      constants.prim_id = static_cast<std::uint32_t>(geometry_record.mesh);
      constants.instance_id = static_cast<std::uint32_t>(instance.instance);

      id<MTLTexture> texture = nil;
      id<MTLSamplerState> sampler = nil;
      if (material.base_color_texture) {
        if (material.base_color_texture->texture_index >=
                request.snapshot->textures.size() ||
            material.base_color_texture->sampler_index >=
                request.snapshot->samplers.size()) {
          [encoder endEncoding];
          throw render::RendererError(
              render::RendererErrorCode::InvalidRequest,
              "encode Metal material",
              "material texture binding index is invalid");
        }
        const auto texture_handle =
            request.snapshot
                ->textures[material.base_color_texture->texture_index]
                .texture;
        const auto sampler_handle =
            request.snapshot
                ->samplers[material.base_color_texture->sampler_index]
                .sampler;
        const auto texture_found = textures_.find(texture_handle);
        const auto sampler_found = samplers_.find(sampler_handle);
        if (texture_found != textures_.end() &&
            sampler_found != samplers_.end()) {
          texture = texture_found->second.texture;
          sampler = sampler_found->second.sampler;
          const auto texture_slot = texture_slots_.Find(texture_handle);
          const auto sampler_slot = sampler_slots_.Find(sampler_handle);
          if (texture_slot && sampler_slot) {
            constants.texture_index = texture_slot->index;
            constants.sampler_index = sampler_slot->index;
          }
        } else {
          constants.feature_mask &=
              ~static_cast<std::uint32_t>(MaterialFeature::BaseColorTexture);
          build.telemetry.material_fallbacks.Record(
              MaterialFallback::BasicMaterial);
        }
      }

      if (material.module) {
        ++build.telemetry.generated_material_fallback_count;
        build.telemetry.material_fallbacks.Record(
            MaterialFallback::BasicMaterial);
        MaterialDiagnostic diagnostic;
        diagnostic.category = MaterialDiagnosticCategory::CacheIncompatible;
        diagnostic.severity = DiagnosticSeverity::Warning;
        diagnostic.fallback = MaterialFallback::BasicMaterial;
        diagnostic.message =
            "Metal generated-material execution is not registered; using "
            "the basic material contract";
        diagnostic.context.material_identity = material.module->key;
        diagnostic.context.backend_target = "metal";
        build.material_diagnostics.push_back(std::move(diagnostic));
      }

      const auto material_constants = MakeMaterial(material, *request.snapshot);
      [encoder setVertexBuffer:geometry->second.vertices offset:0 atIndex:0];
      [encoder setVertexBytes:&constants length:sizeof(constants) atIndex:1];
      [encoder setFragmentBytes:&constants length:sizeof(constants) atIndex:1];
      [encoder setFragmentBytes:&material_constants
                         length:sizeof(material_constants)
                        atIndex:2];
      if (!bindless_) {
        [encoder setFragmentTexture:texture atIndex:0];
        [encoder setFragmentSamplerState:sampler atIndex:0];
      }
      [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                          indexCount:geometry->second.index_count
                           indexType:MTLIndexTypeUInt32
                         indexBuffer:geometry->second.indices
                   indexBufferOffset:0];
      ++build.telemetry.draw_count;
      build.telemetry.triangle_count += geometry->second.index_count / 3U;
      ++build.telemetry.visible_primitive_count;
    }
    [encoder endEncoding];
  }

  void EncodePresentation(id<MTLCommandBuffer> command, FrameContext &frame,
                          id<CAMetalDrawable> drawable, FrameBuild &build) {
    MTLRenderPassDescriptor *pass =
        [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder =
        [command renderCommandEncoderWithDescriptor:pass];
    if (encoder == nil) {
      throw render::RendererError(
          render::RendererErrorCode::BackendFailure,
          "encode Metal presentation pass",
          "renderCommandEncoder returned nil");
    }
    [encoder setRenderPipelineState:presentation_pipeline_];
    const auto color_space =
        presentation_color_space_ == PresentationColorSpace::DisplayP3 ? 1U
                                                                       : 0U;
    [encoder setFragmentBytes:&color_space
                       length:sizeof(color_space)
                      atIndex:0];
    [encoder setFragmentTexture:frame.color atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:3];
    try {
      NotifyOverlay(PresentationOverlayPhase::Render, command, encoder, pass);
    } catch (...) {
      [encoder endEncoding];
      throw;
    }
    [encoder endEncoding];
    build.telemetry.present_count = 1;
    build.telemetry.presentation_copy_bytes =
        static_cast<std::uint64_t>(frame.width) * frame.height * 4U;
  }

  void EncodeReadback(id<MTLCommandBuffer> command, FrameContext &frame,
                      const std::vector<Aov> &readbacks) {
    if (readbacks.empty()) {
      return;
    }
    id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
    const auto pitch = AlignedRowPitch(frame.width);
    const auto size = static_cast<NSUInteger>(pitch) * frame.height;
    const MTLOrigin origin{0, 0, 0};
    const MTLSize extent{frame.width, frame.height, 1};
    const auto copy = [&](Aov aov, id<MTLTexture> texture,
                          id<MTLBuffer> buffer) {
      if (!HasAov(readbacks, aov)) {
        return;
      }
      [encoder copyFromTexture:texture
                       sourceSlice:0
                       sourceLevel:0
                      sourceOrigin:origin
                        sourceSize:extent
                          toBuffer:buffer
                 destinationOffset:0
            destinationBytesPerRow:pitch
          destinationBytesPerImage:size];
    };
    copy(Aov::Color, frame.color, frame.color_readback);
    copy(Aov::Depth, frame.depth, frame.depth_readback);
    copy(Aov::PrimId, frame.prim_id, frame.prim_readback);
    copy(Aov::InstanceId, frame.instance_id, frame.instance_readback);
    [encoder endEncoding];
  }

  template <typename T>
  static std::vector<T> TightCopy(id<MTLBuffer> buffer, std::uint32_t width,
                                  std::uint32_t height) {
    std::vector<T> result(static_cast<std::size_t>(width) * height);
    const auto source_pitch = AlignedRowPitch(width);
    const auto row_bytes = static_cast<std::size_t>(width) * sizeof(T);
    const auto *source = static_cast<const std::byte *>(buffer.contents);
    auto *destination = reinterpret_cast<std::byte *>(result.data());
    for (std::uint32_t row = 0; row < height; ++row) {
      std::memcpy(destination + row * row_bytes,
                  source + static_cast<std::size_t>(row) * source_pitch,
                  row_bytes);
    }
    return result;
  }

  static std::vector<std::uint8_t> TightColorCopy(id<MTLBuffer> buffer,
                                                  std::uint32_t width,
                                                  std::uint32_t height) {
    std::vector<std::uint8_t> result(static_cast<std::size_t>(width) * height *
                                     4U);
    const auto source_pitch = AlignedRowPitch(width);
    const auto row_bytes = static_cast<std::size_t>(width) * 4U;
    const auto *source = static_cast<const std::byte *>(buffer.contents);
    auto *destination = reinterpret_cast<std::byte *>(result.data());
    for (std::uint32_t row = 0; row < height; ++row) {
      std::memcpy(destination + row * row_bytes,
                  source + static_cast<std::size_t>(row) * source_pitch,
                  row_bytes);
    }
    return result;
  }

  void CopyReadbacks(FrameContext &frame, Pending &pending) {
    const auto width = pending.width;
    const auto height = pending.height;
    const auto tight_pitch = width * 4U;
    const auto bytes = static_cast<std::uint64_t>(tight_pitch) * height;
    if (HasAov(pending.readback_aovs, Aov::Color)) {
      pending.result.color.product =
          MakeRenderProduct(width, height, Aov::Color);
      pending.result.color.row_pitch_bytes = tight_pitch;
      pending.result.color.pixels =
          TightColorCopy(frame.color_readback, width, height);
      pending.result.telemetry.readback_bytes += bytes;
    }
    if (HasAov(pending.readback_aovs, Aov::Depth)) {
      pending.result.depth.product =
          MakeRenderProduct(width, height, Aov::Depth);
      pending.result.depth.row_pitch_bytes = tight_pitch;
      pending.result.depth.pixels =
          TightCopy<float>(frame.depth_readback, width, height);
      pending.result.telemetry.readback_bytes += bytes;
    }
    if (HasAov(pending.readback_aovs, Aov::PrimId)) {
      pending.result.prim_id.product =
          MakeRenderProduct(width, height, Aov::PrimId);
      pending.result.prim_id.row_pitch_bytes = tight_pitch;
      pending.result.prim_id.pixels =
          TightCopy<std::uint32_t>(frame.prim_readback, width, height);
      pending.result.telemetry.readback_bytes += bytes;
    }
    if (HasAov(pending.readback_aovs, Aov::InstanceId)) {
      pending.result.instance_id.product =
          MakeRenderProduct(width, height, Aov::InstanceId);
      pending.result.instance_id.row_pitch_bytes = tight_pitch;
      pending.result.instance_id.pixels =
          TightCopy<std::uint32_t>(frame.instance_readback, width, height);
      pending.result.telemetry.readback_bytes += bytes;
    }
    for (const auto aov : pending.rendered_aovs) {
      pending.result.telemetry.requested_aov_mask |= AovBit(aov);
      pending.result.telemetry.rendered_aov_mask |= AovBit(aov);
    }
    for (const auto aov : pending.readback_aovs) {
      pending.result.telemetry.cpu_readback_aov_mask |= AovBit(aov);
    }
    pending.result.telemetry.requested_aov_count = pending.rendered_aovs.size();
    pending.result.telemetry.rendered_aov_count = pending.rendered_aovs.size();
    pending.result.telemetry.cpu_readback_aov_count =
        pending.readback_aovs.size();
    pending.result.telemetry.resolve_count = 1;
    pending.result.telemetry.map_count = pending.readback_aovs.empty() ? 0 : 1;
    pending.result.telemetry.wait_count = 1;
  }

  Pending &ValidateToken(render::CompletionToken token, const char *operation) {
    if (token.owner() != owner_ || token.value() == 0) {
      throw render::RendererError(render::RendererErrorCode::InvalidToken,
                                  operation,
                                  "token belongs to another backend");
    }
    const auto found = pending_.find(token.value());
    if (found == pending_.end()) {
      throw render::RendererError(render::RendererErrorCode::InvalidToken,
                                  operation,
                                  "token is unknown or already resolved");
    }
    return found->second;
  }

  const Pending &ValidateToken(render::CompletionToken token,
                               const char *operation) const {
    if (token.owner() != owner_ || token.value() == 0) {
      throw render::RendererError(render::RendererErrorCode::InvalidToken,
                                  operation,
                                  "token belongs to another backend");
    }
    const auto found = pending_.find(token.value());
    if (found == pending_.end()) {
      throw render::RendererError(render::RendererErrorCode::InvalidToken,
                                  operation,
                                  "token is unknown or already resolved");
    }
    return found->second;
  }

  std::uint64_t GeometryResidentBytes() const noexcept {
    std::uint64_t result{};
    for (const auto &[handle, geometry] : geometries_) {
      (void)handle;
      result += geometry.bytes;
    }
    return result;
  }

  std::uint64_t RetiringBytes() const noexcept {
    std::uint64_t result{};
    for (const auto &retirement : retirements_) {
      result += retirement.bytes;
    }
    return result;
  }

  std::uint64_t owner_{};
  BackendOptions options_;
  id<MTLDevice> device_;
  id<MTLCommandQueue> queue_;
  id<MTLSharedEvent> completion_event_;
  id<MTLLibrary> library_;
  id<MTLRenderPipelineState> pipeline_;
  id<MTLRenderPipelineState> presentation_pipeline_;
  id<MTLDepthStencilState> depth_state_;
  id<MTLArgumentEncoder> argument_encoder_;
  id<MTLHeap> heap_;
  CAMetalLayer *layer_;
  bool bindless_{};
  std::optional<render::PresentationTarget> presentation_;
  PresentationColorSpace presentation_color_space_{
      PresentationColorSpace::Srgb};
  std::uint32_t presentation_width_{};
  std::uint32_t presentation_height_{};

  render::RendererCapabilities capabilities_;
  render::RendererStatistics statistics_;
  MetalStatistics metal_statistics_;
  StableResourceTable texture_slots_;
  StableResourceTable sampler_slots_;
  std::unordered_map<std::uint64_t, Geometry> geometries_;
  std::unordered_map<std::uint64_t, Texture> textures_;
  std::unordered_map<std::uint64_t, Sampler> samplers_;
  std::vector<Retirement> retirements_;
  std::vector<FrameContext> frames_;
  std::unordered_map<std::uint64_t, Pending> pending_;
  std::size_t next_context_{};
  std::uint64_t submitted_value_{};
  std::atomic<std::uint64_t> completed_value_{};
  std::uint64_t uploaded_bytes_{};
  std::uint64_t readback_bytes_{};
  std::uint64_t presentation_copy_bytes_{};
  std::uint64_t aov_image_export_count_{};
  std::uint64_t active_aov_image_leases_{};
  std::uint64_t geometry_peak_resident_bytes_{};
};

Backend::Backend(const render::BackendCreateInfo &info, BackendOptions options)
    : impl_(std::make_unique<Impl>(info, options)) {}

Backend::~Backend() = default;
Backend::Backend(Backend &&) noexcept = default;
Backend &Backend::operator=(Backend &&) noexcept = default;

const render::RendererCapabilities &Backend::capabilities() const noexcept {
  return impl_->capabilities();
}

render::RendererStatistics Backend::statistics() const noexcept {
  return impl_->statistics();
}

MetalStatistics Backend::metal_statistics() const noexcept {
  return impl_->metal_statistics();
}

std::optional<render::PresentationTarget>
Backend::default_presentation_target() const noexcept {
  return impl_->default_presentation_target();
}

void Backend::ResizePresentationTarget(render::PresentationTarget target,
                                       std::uint32_t width,
                                       std::uint32_t height) {
  impl_->ResizePresentationTarget(target, width, height);
}

render::CompletionToken Backend::Submit(const render::RenderRequest &request) {
  return impl_->Submit(request);
}

bool Backend::IsComplete(render::CompletionToken token) const {
  return impl_->IsComplete(token);
}

AovImageExport Backend::AcquireAovImage(render::CompletionToken token,
                                        Aov aov) {
  return impl_->AcquireAovImage(token, aov);
}

void Backend::ReleaseAovImage(AovImageLease&& lease) {
  impl_->ReleaseAovImage(std::move(lease));
}

render::RenderResult Backend::Resolve(render::CompletionToken token,
                                      std::chrono::nanoseconds timeout) {
  return impl_->Resolve(token, timeout);
}

BackendFactory::BackendFactory(BackendOptions options) : options_(options) {}

render::BackendKind BackendFactory::kind() const noexcept {
  return render::BackendKind::Metal;
}

render::BackendAvailability BackendFactory::availability() const {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
      return {false, "no Metal device is available"};
    }
    id<MTLHeap> heap = [device
        newHeapWithDescriptor:SceneHeapDescriptor(options_.heap_capacity_bytes)];
    if (heap == nil) {
      return {false,
              "Metal resource heaps are unavailable for the configured " +
                  std::to_string(options_.heap_capacity_bytes) +
                  "-byte scene residency budget"};
    }
    return {true, String(device.name)};
  }
}

std::unique_ptr<render::Backend>
BackendFactory::Create(const render::BackendCreateInfo &info) const {
  if (info.backend == render::BackendRequest::Vulkan) {
    throw render::RendererError(render::RendererErrorCode::InvalidRequest,
                                "create Metal backend",
                                "the Metal factory received a Vulkan request");
  }
  return std::make_unique<Backend>(info, options_);
}

} // namespace merlin::metal
