#pragma once

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <merlin/metal/backend.hpp>
#include <merlin/render/forward_lighting.hpp>
#include <merlin/render/diffuse_environment.hpp>
#include "gaussian_raster_abi.hpp"
#include "gaussian_execution.hpp"
#include <merlin/extraction/gaussian_preparation.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace merlin::metal {
namespace detail {

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kShaderTextureCapacity = 128;
constexpr std::uint32_t kShaderSamplerCapacity = 32;
constexpr std::uint32_t kMaskedAlphaFlag = 0x10000000U;

inline std::atomic<std::uint64_t> g_owner{1000};

inline std::string String(NSString* value) {
  if (value == nil) {
    return {};
  }
  const char* utf8 = value.UTF8String;
  return utf8 == nullptr ? std::string{} : std::string(utf8);
}

template <typename Handle>
inline Handle DecodeHandle(std::uintptr_t value) noexcept {
  return (__bridge Handle)(reinterpret_cast<void*>(value));
}

inline std::uint64_t DurationNs(Clock::time_point begin, Clock::time_point end) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin)
          .count());
}

inline std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment) {
  if (alignment == 0) {
    return value;
  }
  return (value + alignment - 1U) / alignment * alignment;
}

inline std::uint32_t AlignedRowPitch(std::uint32_t width) {
  return static_cast<std::uint32_t>(
      AlignUp(static_cast<std::uint64_t>(width) * 4U, 256U));
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
  std::array<Vec4, 9> diffuse_environment;
  Vec4 output_transform;
  Vec4 surface_policy;
};

struct alignas(16) GpuSceneDrawConstants {
  Mat4 view_projection;
  std::uint32_t draw_slot{};
  std::uint32_t padding[3]{};
};

static_assert(offsetof(extraction::DrawVertex, position) == 0);
static_assert(offsetof(extraction::DrawVertex, normal) == 12);
static_assert(offsetof(extraction::DrawVertex, color) == 24);
static_assert(offsetof(extraction::DrawVertex, texcoord) == 40);
static_assert(sizeof(extraction::DrawVertex) == 48);
static_assert(sizeof(DrawConstants) == 144);
static_assert(sizeof(MaterialConstants) == 224);
static_assert(offsetof(MaterialConstants, diffuse_environment) == 48);
static_assert(offsetof(MaterialConstants, output_transform) == 192);
static_assert(offsetof(MaterialConstants, surface_policy) == 208);
static_assert(sizeof(GpuSceneDrawConstants) == 80);
static_assert(alignof(GpuSceneDrawConstants) == 16);
static_assert(offsetof(GpuSceneDrawConstants, view_projection) == 0);
static_assert(offsetof(GpuSceneDrawConstants, draw_slot) == 64);

inline std::uint64_t AovBit(Aov aov) {
  return 1ULL << static_cast<std::uint32_t>(aov);
}

inline bool HasAov(const std::vector<Aov>& values, Aov aov) {
  return std::find(values.begin(), values.end(), aov) != values.end();
}

inline std::vector<Aov>
ValidateProducts(const std::vector<render::RenderProductRequest>& products,
    std::vector<Aov>* readbacks) {
  if (products.empty()) {
    throw render::RendererError(render::RendererErrorCode::InvalidRequest,
        "submit Metal frame",
        "at least one render product is required");
  }
  std::vector<Aov> rendered;
  for (const auto& product : products) {
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

inline MTLSamplerMinMagFilter Filter(FilterMode value) {
  return value == FilterMode::Nearest ? MTLSamplerMinMagFilterNearest
                                      : MTLSamplerMinMagFilterLinear;
}

inline MTLSamplerAddressMode Address(AddressMode value) {
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

inline MTLHeapDescriptor* SceneHeapDescriptor(std::uint64_t capacity_bytes) {
  MTLHeapDescriptor* descriptor = [MTLHeapDescriptor new];
  descriptor.size = capacity_bytes;
  descriptor.storageMode = MTLStorageModeShared;
  descriptor.cpuCacheMode = MTLCPUCacheModeWriteCombined;
  descriptor.hazardTrackingMode = MTLHazardTrackingModeTracked;
  return descriptor;
}

template <typename Record>
inline std::uint64_t GpuSceneTableReservation(id<MTLDevice> device,
    std::uint32_t capacity) {
  const auto length = static_cast<std::uint64_t>(capacity) * sizeof(Record);
  const auto size_and_align = [device
      heapBufferSizeAndAlignWithLength:static_cast<NSUInteger>(length)
                               options:MTLResourceStorageModePrivate];
  return AlignUp(size_and_align.size, size_and_align.align);
}

inline std::uint64_t GpuSceneReservation(
    id<MTLDevice> device,
    const render::GpuScenePackingCapacities& capacities) {
  return GpuSceneTableReservation<render::GpuGeometry>(device,
             capacities.geometries) +
         GpuSceneTableReservation<render::GpuInstance>(device,
             capacities.instances) +
         GpuSceneTableReservation<render::GpuMaterial>(device,
             capacities.materials) +
         GpuSceneTableReservation<render::GpuDraw>(device, capacities.draws);
}

} // namespace detail
} // namespace merlin::metal
