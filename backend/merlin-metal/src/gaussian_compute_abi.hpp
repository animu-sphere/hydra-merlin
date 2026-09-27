#pragma once

#include <cstddef>
#include <cstdint>

#include <merlin/core/types.hpp>

// Private host records for the shared Gaussian compute kernels. Metal accesses
// prepared records through byte-addressed buffers to preserve the scalar ABI.
namespace merlin::metal::gaussian_compute {

struct alignas(16) PrepareConstants {
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

struct alignas(16) PreparedRecord {
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

struct PrepareCounters {
  std::uint32_t candidate_count{};
  std::uint32_t visible_count{};
  std::uint32_t opacity_culled_count{};
  std::uint32_t frustum_culled_count{};
  std::uint32_t invalid_culled_count{};
  std::uint32_t padding[3]{};
};

struct SortElement {
  std::uint32_t key_low{};
  std::uint32_t key_high{};
  std::uint32_t value{};
};

struct SortConstants {
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

static_assert(sizeof(PrepareConstants) == 176);
static_assert(offsetof(PrepareConstants, projection) == 64);
static_assert(offsetof(PrepareConstants, viewport_size) == 128);
static_assert(offsetof(PrepareConstants, particle_count) == 152);
static_assert(offsetof(PrepareConstants, sorting_mode) == 168);
static_assert(sizeof(PreparedRecord) == 64);
static_assert(offsetof(PreparedRecord, inverse_conic) == 16);
static_assert(offsetof(PreparedRecord, opacity) == 28);
static_assert(offsetof(PreparedRecord, radiance) == 32);
static_assert(offsetof(PreparedRecord, sort_key) == 44);
static_assert(offsetof(PreparedRecord, resource_id_low) == 48);
static_assert(offsetof(PreparedRecord, particle_id) == 56);
static_assert(sizeof(PrepareCounters) == 32);
static_assert(sizeof(SortElement) == 12);
static_assert(sizeof(SortConstants) == 48);
static_assert(offsetof(SortConstants, scan_offset) == 16);
static_assert(offsetof(SortConstants, visible_count_offset) == 36);
static_assert(offsetof(SortConstants, flags) == 44);

} // namespace merlin::metal::gaussian_compute
