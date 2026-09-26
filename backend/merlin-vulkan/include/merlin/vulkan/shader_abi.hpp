#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include <merlin/core/shader_contract.hpp>
#include <merlin/core/types.hpp>
#include <merlin/extraction/frame_snapshot.hpp>
#include <merlin/render/gpu_driven.hpp>

namespace merlin::vulkan::shader_abi {

inline constexpr std::uint32_t kVersion = 9;
inline constexpr std::uint32_t kArtifactSchemaVersion = 2;

// Derived rather than spelled out so a schema bump cannot leave the runtime
// loading a directory the build system no longer writes. merlin-vulkan
// static_asserts this against MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION.
[[nodiscard]] inline std::filesystem::path ArtifactDirectory() {
  return std::filesystem::path("shaders") /
         ("v" + std::to_string(kArtifactSchemaVersion));
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
};

struct alignas(16) MaterialConstants {
  Vec4 base_color;
  Vec4 light_direction_intensity;
  Vec4 light_color_alpha_cutoff;
  std::array<Vec4, 9> diffuse_environment;
};

// Table-backed Forward keeps indexed submission on the CPU while selecting a
// persistent GpuDraw record by physical slot in the shader.
struct alignas(16) GpuSceneDrawConstants {
  Mat4 view_projection;
  std::uint32_t draw_slot{};
  std::uint32_t padding[3]{};
};

inline constexpr std::uint32_t kGpuDrivenVisibilityMaskCulling = 1U << 0U;
inline constexpr std::uint32_t kGpuDrivenFrustumCulling = 1U << 1U;
inline constexpr std::uint32_t kGpuDrivenIndexedVertexStride = 48U;
inline constexpr std::uint32_t kGpuDrivenIndexedWorkgroupSize = 64U;

[[nodiscard]] constexpr std::uint32_t GpuDrivenIndexedWorkgroupCount(
    std::uint32_t candidate_count) noexcept {
  return candidate_count == 0U
             ? 0U
             : 1U + (candidate_count - 1U) /
                        kGpuDrivenIndexedWorkgroupSize;
}

enum class GpuDrivenCandidateResult : std::uint32_t {
  Visible,
  VisibilityMaskCulled,
  FrustumCulled,
};

struct alignas(16) GpuDrivenIndexedConstants {
  Mat4 view_projection;
  std::uint32_t visibility_mask{~std::uint32_t{}};
  std::uint32_t candidate_count{};
  std::uint32_t flags{kGpuDrivenVisibilityMaskCulling |
                      kGpuDrivenFrustumCulling};
  std::uint32_t vertex_stride{kGpuDrivenIndexedVertexStride};
};

struct alignas(16) GpuDrivenIndexedDispatchCounters {
  std::uint32_t candidate_count{};
  std::uint32_t visible_count{};
  std::uint32_t visibility_mask_culled_count{};
  std::uint32_t frustum_culled_count{};
};

struct alignas(16) GpuDrivenForwardConstants {
  Mat4 view_projection;
};

inline constexpr std::uint32_t kGaussianPrepareWorkgroupSize = 64U;

[[nodiscard]] constexpr std::uint32_t GaussianPrepareWorkgroupCount(
    std::uint32_t candidate_count) noexcept {
  return candidate_count == 0U
             ? 0U
             : 1U + (candidate_count - 1U) /
                        kGaussianPrepareWorkgroupSize;
}

enum class GaussianCandidateResult : std::uint32_t {
  Visible,
  OpacityCulled,
  FrustumCulled,
  InvalidCulled,
};

// One dispatch processes one resident Gaussian resource. The constants use a
// uniform descriptor because this two-matrix block exceeds Vulkan's guaranteed
// 128-byte push-constant limit. The four source bindings are byte-addressed
// because their arena payloads are tightly packed.
struct alignas(16) GaussianPrepareConstants {
  Mat4 local_to_camera;
  Mat4 projection;
  Vec2 viewport_size;
  float sigma_extent{3.0F};
  float minimum_variance_pixels{0.25F};
  std::uint32_t resource_id_low{};
  std::uint32_t resource_id_high{};
  std::uint32_t particle_count{};
  std::uint32_t coefficients_per_particle{};
  std::uint32_t spherical_harmonics_degree{};
  std::uint32_t projection_mode{};
  std::uint32_t sorting_mode{};
  std::uint32_t padding{};
};

struct alignas(16) GaussianPreparedRecord {
  Vec2 center_pixels;
  float radius_pixels{};
  float depth{};
  Vec3 inverse_conic;
  float opacity{};
  Vec3 radiance;
  float sort_key{};
  std::uint32_t resource_id_low{};
  std::uint32_t resource_id_high{};
  std::uint32_t particle_id{};
  std::uint32_t padding{};
};

struct alignas(16) GaussianPrepareDispatchCounters {
  std::uint32_t candidate_count{};
  std::uint32_t visible_count{};
  std::uint32_t opacity_culled_count{};
  std::uint32_t frustum_culled_count{};
  std::uint32_t invalid_culled_count{};
  std::uint32_t padding[3]{};
};

// The global Gaussian sort runs an LSD radix sort over 8-bit digits of a
// 64-bit key. Histogram and scatter workgroups own 256 keys each; the key
// count is padded to that size with sentinels, so the digit-major histogram
// holds exactly one entry per padded key.
inline constexpr std::uint32_t kGaussianSortWorkgroupSize = 256U;
inline constexpr std::uint32_t kGaussianSortRadixBits = 8U;
inline constexpr std::uint32_t kGaussianSortRadixBins = 256U;
inline constexpr std::uint32_t kGaussianSortScanWorkgroupElements = 1024U;
inline constexpr std::uint32_t kGaussianSortInvalidValue = ~std::uint32_t{};
// Verification words precede the per-resource visible counts and histogram
// levels in the scan/control buffer.
inline constexpr std::uint32_t kGaussianSortControlWordCount = 4U;

[[nodiscard]] constexpr std::uint32_t GaussianSortWorkgroupCount(
    std::uint32_t key_count) noexcept {
  return key_count == 0U
             ? 0U
             : 1U + (key_count - 1U) / kGaussianSortWorkgroupSize;
}

[[nodiscard]] constexpr std::uint32_t GaussianSortScanWorkgroupCount(
    std::uint32_t element_count) noexcept {
  return element_count == 0U
             ? 0U
             : 1U + (element_count - 1U) /
                        kGaussianSortScanWorkgroupElements;
}

// Back to front: a larger sort key maps to a smaller unsigned high word, and
// -0 shares +0's key because the CPU reference compares the float values.
[[nodiscard]] constexpr std::uint32_t GaussianSortKeyHigh(
    float sort_key) noexcept {
  const auto bits =
      sort_key == 0.0F ? 0U : std::bit_cast<std::uint32_t>(sort_key);
  const auto ordered =
      (bits & 0x80000000U) != 0U ? ~bits : (bits | 0x80000000U);
  return ~ordered;
}

// The low word is a frame-global candidate index below candidate_count, so
// only the bytes that can differ need a radix pass. The four high-word passes
// always run.
[[nodiscard]] constexpr std::uint32_t GaussianSortLowWordPassCount(
    std::uint64_t candidate_count) noexcept {
  if (candidate_count <= 1U) {
    return 0U;
  }
  const auto bits = static_cast<std::uint32_t>(
      std::bit_width(candidate_count - 1U));
  return (bits + kGaussianSortRadixBits - 1U) / kGaussianSortRadixBits;
}

[[nodiscard]] constexpr std::uint32_t GaussianSortMix(
    std::uint32_t value) noexcept {
  value ^= value >> 16U;
  value *= 0x85EBCA6BU;
  value ^= value >> 13U;
  value *= 0xC2B2AE35U;
  value ^= value >> 16U;
  return value;
}

// Order-sensitive verification: the sort adds (position + 1) * hash for each
// sorted record, so the CPU reference order reproduces the same checksum.
[[nodiscard]] constexpr std::uint32_t GaussianSortIdentityHash(
    std::uint32_t resource_low, std::uint32_t resource_high,
    std::uint32_t particle) noexcept {
  const auto hash = GaussianSortMix((particle * 0x9E3779B9U) ^ resource_low);
  return GaussianSortMix(hash ^ (resource_high * 0x85EBCA6BU));
}

struct GaussianSortElement {
  std::uint32_t key_low{};
  std::uint32_t key_high{};
  // Index into the prepared-record buffer, or kGaussianSortInvalidValue.
  std::uint32_t value{};
};

// Histogram and scatter bound their input by
// min(element_count, scan[count_word]) instead of element_count alone, so a
// device-written count can sort a prefix of a fixed-capacity buffer.
inline constexpr std::uint32_t kGaussianSortDynamicCount = 1U << 0U;

// One push-constant block shared by every sort kernel; each reads only the
// fields its stage documents in gaussian-sort.slang.
struct alignas(16) GaussianSortConstants {
  std::uint32_t element_count{};
  std::uint32_t block_count{};
  std::uint32_t digit_shift{};
  std::uint32_t digit_word{};
  std::uint32_t scan_offset{};
  std::uint32_t scan_count{};
  std::uint32_t scan_sums_offset{};
  std::uint32_t candidate_base{};
  std::uint32_t prepared_base{};
  std::uint32_t visible_count_offset{};
  std::uint32_t count_word{};
  std::uint32_t flags{};
};

struct GaussianSortVerification {
  std::uint32_t sorted_count{};
  std::uint32_t order_violation_count{};
  std::uint32_t key_mismatch_count{};
  std::uint32_t identity_checksum{};
};

// The raster gather copies the verified sorted stream into raster order and
// publishes the instance count of one VkDrawIndirectCommand. It shares the
// sort's descriptor-set layout, and its push constants fit inside the sort's.
struct alignas(16) GaussianRasterGatherConstants {
  std::uint32_t element_count{};
  std::uint32_t padding[3]{};
};

// Six procedural corner vertices per sorted record.
inline constexpr std::uint32_t kGaussianRasterVertexCount = 6U;

// Tile binning pairs every sorted record with the fixed-size screen tiles its
// conservative square can cover, then groups the pairs by tile with a stable
// radix sort over the tile index alone, so each tile keeps the verified back-
// to-front order. Pairs live in a fixed-capacity buffer; pairs past it are
// dropped and reported, never written out of bounds.
inline constexpr std::uint32_t kGaussianTileSize = 16U;
inline constexpr std::uint32_t kGaussianTileWorkgroupSize = 256U;
inline constexpr std::uint32_t kGaussianTileDefaultPairsPerRecord = 8U;
inline constexpr std::uint32_t kGaussianTileMinimumPairCapacity = 65536U;
// Control words at the start of the tile control buffer; the per-record pair
// offsets, the pair histogram levels, and the tile ranges follow.
inline constexpr std::uint32_t kGaussianTileControlWordCount = 10U;
inline constexpr std::uint32_t kGaussianTileRecordCountWord = 0U;
inline constexpr std::uint32_t kGaussianTileRequestedPairCountWord = 1U;

[[nodiscard]] constexpr std::uint32_t GaussianTileGridSize(
    std::uint32_t pixels) noexcept {
  return pixels == 0U ? 0U : 1U + (pixels - 1U) / kGaussianTileSize;
}

// Inclusive tile bounds of the pixels whose centers the conservative square
// center +/- radius covers. gaussian-tile.slang evaluates the same float
// expressions, so CPU reference pairs match GPU pairs for identical records.
struct GaussianTileRect {
  std::uint32_t x0{};
  std::uint32_t y0{};
  std::uint32_t x1{};
  std::uint32_t y1{};
};

[[nodiscard]] inline bool GaussianTileBounds(Vec2 center, float radius,
    std::uint32_t width, std::uint32_t height,
    GaussianTileRect& rect) noexcept {
  if (width == 0U || height == 0U || !(radius > 0.0F) ||
      !std::isfinite(radius) || !std::isfinite(center.x) ||
      !std::isfinite(center.y)) {
    return false;
  }
  const auto max_x = static_cast<float>(width - 1U);
  const auto max_y = static_cast<float>(height - 1U);
  const auto low_x = std::ceil(center.x - radius - 0.5F);
  const auto low_y = std::ceil(center.y - radius - 0.5F);
  const auto high_x = std::floor(center.x + radius - 0.5F);
  const auto high_y = std::floor(center.y + radius - 0.5F);
  if (!(high_x >= 0.0F) || !(high_y >= 0.0F) || !(low_x <= max_x) ||
      !(low_y <= max_y) || low_x > high_x || low_y > high_y) {
    return false;
  }
  const auto first_x = static_cast<std::uint32_t>(std::max(low_x, 0.0F));
  const auto first_y = static_cast<std::uint32_t>(std::max(low_y, 0.0F));
  const auto last_x = static_cast<std::uint32_t>(std::min(high_x, max_x));
  const auto last_y = static_cast<std::uint32_t>(std::min(high_y, max_y));
  rect = {first_x / kGaussianTileSize, first_y / kGaussianTileSize,
      last_x / kGaussianTileSize, last_y / kGaussianTileSize};
  return true;
}

// Order-sensitive verification: the tile verify kernel adds
// (pair position + 1) * hash for every stored pair.
[[nodiscard]] constexpr std::uint32_t GaussianTilePairHash(
    std::uint32_t tile, std::uint32_t record) noexcept {
  return GaussianSortMix(GaussianSortMix(tile * 0x85EBCA6BU) ^
                         (record * 0x9E3779B9U));
}

// Tile kernels bind the sort's set layout and fit its push-constant block.
// Count and emit run over records, ranges and verify over stored pairs.
struct alignas(16) GaussianTileConstants {
  // Padded sorted record count.
  std::uint32_t record_bound{};
  std::uint32_t pair_capacity{};
  std::uint32_t tile_count_x{};
  std::uint32_t tile_count_y{};
  std::uint32_t viewport_width{};
  std::uint32_t viewport_height{};
  // Word offsets inside the control buffer.
  std::uint32_t offsets_offset{};
  std::uint32_t ranges_offset{};
  // Pairs one record may emit, in row-major tile order; keeps every prefix
  // sum of per-record counts within uint32.
  std::uint32_t record_pair_limit{};
  std::uint32_t padding[3]{};
};

struct GaussianTileVerification {
  // Real records, copied from the sorted-stream draw arguments.
  std::uint32_t record_count{};
  // Pairs the records request after record_pair_limit; stored pairs are
  // min(requested, capacity).
  std::uint32_t requested_pair_count{};
  std::uint32_t clamped_record_count{};
  std::uint32_t verified_pair_count{};
  std::uint32_t order_violation_count{};
  std::uint32_t pair_mismatch_count{};
  std::uint32_t range_violation_count{};
  std::uint32_t occupied_tile_count{};
  std::uint32_t max_tile_pair_count{};
  std::uint32_t identity_checksum{};
};

static_assert(sizeof(DrawConstants) == 128);
static_assert(alignof(DrawConstants) == 16);
static_assert(offsetof(DrawConstants, model_view_projection) == 0);
static_assert(offsetof(DrawConstants, normal_matrix_column0) == 64);
static_assert(offsetof(DrawConstants, normal_matrix_column1) == 80);
static_assert(offsetof(DrawConstants, normal_matrix_column2) == 96);
static_assert(offsetof(DrawConstants, feature_mask) == 112);
static_assert(offsetof(DrawConstants, prim_id) == 116);
static_assert(offsetof(DrawConstants, instance_id) == 120);
static_assert(offsetof(DrawConstants, texture_index) == 124);
static_assert(sizeof(MaterialConstants) == 192);
static_assert(alignof(MaterialConstants) == 16);
static_assert(offsetof(MaterialConstants, base_color) == 0);
static_assert(offsetof(MaterialConstants, light_direction_intensity) == 16);
static_assert(offsetof(MaterialConstants, light_color_alpha_cutoff) == 32);
static_assert(offsetof(MaterialConstants, diffuse_environment) == 48);
static_assert(sizeof(GpuSceneDrawConstants) == 80);
static_assert(alignof(GpuSceneDrawConstants) == 16);
static_assert(offsetof(GpuSceneDrawConstants, view_projection) == 0);
static_assert(offsetof(GpuSceneDrawConstants, draw_slot) == 64);
static_assert(sizeof(GpuDrivenIndexedConstants) == 80);
static_assert(alignof(GpuDrivenIndexedConstants) == 16);
static_assert(offsetof(GpuDrivenIndexedConstants, view_projection) == 0);
static_assert(offsetof(GpuDrivenIndexedConstants, visibility_mask) == 64);
static_assert(offsetof(GpuDrivenIndexedConstants, candidate_count) == 68);
static_assert(offsetof(GpuDrivenIndexedConstants, flags) == 72);
static_assert(offsetof(GpuDrivenIndexedConstants, vertex_stride) == 76);
static_assert(sizeof(extraction::DrawVertex) == kGpuDrivenIndexedVertexStride);
static_assert(sizeof(GpuDrivenIndexedDispatchCounters) == 16);
static_assert(alignof(GpuDrivenIndexedDispatchCounters) == 16);
static_assert(offsetof(GpuDrivenIndexedDispatchCounters, candidate_count) == 0);
static_assert(offsetof(GpuDrivenIndexedDispatchCounters, visible_count) == 4);
static_assert(offsetof(GpuDrivenIndexedDispatchCounters,
                  visibility_mask_culled_count) == 8);
static_assert(offsetof(GpuDrivenIndexedDispatchCounters,
                  frustum_culled_count) == 12);
static_assert(sizeof(GpuDrivenForwardConstants) == 64);
static_assert(alignof(GpuDrivenForwardConstants) == 16);
static_assert(offsetof(GpuDrivenForwardConstants, view_projection) == 0);
static_assert(sizeof(GaussianPrepareConstants) == 176);
static_assert(alignof(GaussianPrepareConstants) == 16);
static_assert(offsetof(GaussianPrepareConstants, local_to_camera) == 0);
static_assert(offsetof(GaussianPrepareConstants, projection) == 64);
static_assert(offsetof(GaussianPrepareConstants, viewport_size) == 128);
static_assert(offsetof(GaussianPrepareConstants, sigma_extent) == 136);
static_assert(offsetof(GaussianPrepareConstants, resource_id_low) == 144);
static_assert(offsetof(GaussianPrepareConstants, particle_count) == 152);
static_assert(offsetof(GaussianPrepareConstants,
                  spherical_harmonics_degree) == 160);
static_assert(offsetof(GaussianPrepareConstants, sorting_mode) == 168);
static_assert(sizeof(GaussianPreparedRecord) == 64);
static_assert(alignof(GaussianPreparedRecord) == 16);
static_assert(offsetof(GaussianPreparedRecord, center_pixels) == 0);
static_assert(offsetof(GaussianPreparedRecord, inverse_conic) == 16);
static_assert(offsetof(GaussianPreparedRecord, radiance) == 32);
static_assert(offsetof(GaussianPreparedRecord, resource_id_low) == 48);
static_assert(offsetof(GaussianPreparedRecord, particle_id) == 56);
static_assert(sizeof(GaussianPrepareDispatchCounters) == 32);
static_assert(alignof(GaussianPrepareDispatchCounters) == 16);
static_assert(offsetof(GaussianPrepareDispatchCounters, candidate_count) == 0);
static_assert(offsetof(GaussianPrepareDispatchCounters, invalid_culled_count) ==
              16);
static_assert(sizeof(GaussianSortElement) == 12);
static_assert(alignof(GaussianSortElement) == 4);
static_assert(offsetof(GaussianSortElement, key_high) == 4);
static_assert(offsetof(GaussianSortElement, value) == 8);
static_assert(sizeof(GaussianSortConstants) == 48);
static_assert(alignof(GaussianSortConstants) == 16);
static_assert(offsetof(GaussianSortConstants, digit_shift) == 8);
static_assert(offsetof(GaussianSortConstants, scan_offset) == 16);
static_assert(offsetof(GaussianSortConstants, candidate_base) == 28);
static_assert(offsetof(GaussianSortConstants, visible_count_offset) == 36);
static_assert(sizeof(GaussianSortVerification) ==
              kGaussianSortControlWordCount * sizeof(std::uint32_t));
static_assert(offsetof(GaussianSortConstants, count_word) == 40);
static_assert(offsetof(GaussianSortConstants, flags) == 44);
static_assert(sizeof(GaussianRasterGatherConstants) == 16);
static_assert(sizeof(GaussianRasterGatherConstants) <=
              sizeof(GaussianSortConstants));
static_assert(sizeof(GaussianTileConstants) == 48);
static_assert(alignof(GaussianTileConstants) == 16);
static_assert(offsetof(GaussianTileConstants, tile_count_x) == 8);
static_assert(offsetof(GaussianTileConstants, offsets_offset) == 24);
static_assert(offsetof(GaussianTileConstants, record_pair_limit) == 32);
static_assert(sizeof(GaussianTileConstants) <=
              sizeof(GaussianSortConstants));
static_assert(sizeof(GaussianTileVerification) ==
              kGaussianTileControlWordCount * sizeof(std::uint32_t));
static_assert(offsetof(GaussianTileVerification, requested_pair_count) ==
              kGaussianTileRequestedPairCountWord * sizeof(std::uint32_t));
static_assert(kGaussianTileWorkgroupSize == kGaussianSortWorkgroupSize);
static_assert(kGaussianSortRadixBins == 1U << kGaussianSortRadixBits);
static_assert(kGaussianSortWorkgroupSize == kGaussianSortRadixBins);
static_assert(sizeof(render::GpuIndexedIndirectCommand) == 20);

enum class ResourceClass {
  CombinedImageSampler,
  Sampler,
  SampledImage,
  UniformBuffer,
  StorageBuffer,
};

struct ResourceBinding {
  std::uint32_t set{};
  std::uint32_t binding{};
  ResourceClass resource_class{};
};

inline constexpr ResourceBinding kConventionalBaseColorTexture{
    0, 0, ResourceClass::CombinedImageSampler};
inline constexpr ResourceBinding kConventionalMaterialConstants{
    0, 31, ResourceClass::UniformBuffer};
inline constexpr ResourceBinding kBindlessSamplers{
    0, 0, ResourceClass::Sampler};
inline constexpr ResourceBinding kBindlessTextures{
    0, 1, ResourceClass::SampledImage};
inline constexpr ResourceBinding kBindlessMaterialConstants{
    1, 0, ResourceClass::UniformBuffer};
inline constexpr ResourceBinding kGpuSceneGeometries{
    1, 1, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGpuSceneInstances{
    1, 2, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGpuSceneMaterials{
    1, 3, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGpuSceneDraws{
    1, 4, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGpuDrivenCandidateDrawSlots{
    2, 0, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGpuDrivenCandidateResults{
    2, 1, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGpuDrivenIndirectCommands{
    2, 2, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGpuDrivenDispatchCounters{
    2, 3, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGaussianPositions{
    3, 0, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGaussianCovariances{
    3, 1, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGaussianOpacities{
    3, 2, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGaussianRadiance{
    3, 3, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGaussianCandidateResults{
    3, 4, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGaussianPreparedRecords{
    3, 5, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGaussianPrepareCounters{
    3, 6, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGaussianPrepareConstants{
    3, 7, ResourceClass::UniformBuffer};
// Every sort kernel shares one four-storage-buffer set, the Vulkan
// guaranteed per-stage minimum, and one push-constant block.
inline constexpr ResourceBinding kGaussianSortSource{
    0, 0, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGaussianSortDestination{
    0, 1, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGaussianSortScan{
    0, 2, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGaussianSortPreparedRecords{
    0, 3, ResourceClass::StorageBuffer};
// The raster gather reuses the sort's set layout with its own roles.
inline constexpr ResourceBinding kGaussianRasterSorted{
    0, 0, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGaussianRasterPreparedRecords{
    0, 1, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGaussianRasterRecords{
    0, 2, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGaussianRasterDraw{
    0, 3, ResourceClass::StorageBuffer};
// Tile kernels share the sort's set layout and roles, so the pair sort binds
// the same descriptor sets: pairs ping-pong between source and destination,
// control words and scans share one buffer, and records are the gathered
// raster-order stream.
inline constexpr ResourceBinding kGaussianTileSource{
    0, 0, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGaussianTileDestination{
    0, 1, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGaussianTileControl{
    0, 2, ResourceClass::StorageBuffer};
inline constexpr ResourceBinding kGaussianTileRecords{
    0, 3, ResourceClass::StorageBuffer};

inline constexpr ShaderCapability kConventionalCapabilities =
    ShaderCapability::MaterialConstants |
    ShaderCapability::BaseColorTexture;
inline constexpr ShaderCapability kBindlessCapabilities =
    kConventionalCapabilities | ShaderCapability::BindlessResources |
    ShaderCapability::NonUniformResourceIndexing;

} // namespace merlin::vulkan::shader_abi
