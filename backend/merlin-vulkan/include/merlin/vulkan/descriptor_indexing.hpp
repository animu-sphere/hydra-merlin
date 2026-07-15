#pragma once

#include <cstdint>
#include <string_view>

namespace merlin::vulkan {

inline constexpr std::uint32_t kReservedBindlessTextureSlots = 4;

enum class DescriptorBackendRequest {
  Automatic,
  Conventional,
  Bindless,
};

enum class DescriptorBackend {
  Conventional,
  Bindless,
};

enum class DescriptorFallbackCategory {
  None,
  Configuration,
  Feature,
  Limit,
};

enum class DescriptorFallbackReason {
  None,
  ConfigurationForcedConventional,
  ConfigurationTextureCapacityInvalid,
  ConfigurationSamplerCapacityInvalid,
  FeatureSampledImageNonUniformIndexingMissing,
  FeatureSampledImageUpdateAfterBindMissing,
  FeaturePartiallyBoundMissing,
  FeatureVariableDescriptorCountMissing,
  FeatureRuntimeDescriptorArrayMissing,
  LimitAllPoolsInsufficient,
  LimitSamplerAllocationCountInsufficient,
  LimitPerStageSamplersInsufficient,
  LimitPerStageSampledImagesInsufficient,
  LimitPerStageResourcesInsufficient,
  LimitDescriptorSetSamplersInsufficient,
  LimitDescriptorSetSampledImagesInsufficient,
};

[[nodiscard]] std::string_view DescriptorBackendRequestName(
    DescriptorBackendRequest request) noexcept;
[[nodiscard]] std::string_view DescriptorBackendName(
    DescriptorBackend backend) noexcept;
[[nodiscard]] std::string_view DescriptorFallbackCategoryName(
    DescriptorFallbackCategory category) noexcept;
[[nodiscard]] std::string_view DescriptorFallbackReasonName(
    DescriptorFallbackReason reason) noexcept;
[[nodiscard]] DescriptorFallbackCategory DescriptorFallbackReasonCategory(
    DescriptorFallbackReason reason) noexcept;

struct DescriptorIndexingFeatures {
  bool shader_sampled_image_array_non_uniform_indexing{};
  bool descriptor_binding_sampled_image_update_after_bind{};
  bool descriptor_binding_partially_bound{};
  bool descriptor_binding_variable_descriptor_count{};
  bool runtime_descriptor_array{};
};

struct DescriptorIndexingLimits {
  std::uint32_t max_update_after_bind_descriptors_in_all_pools{};
  std::uint32_t max_per_stage_descriptor_update_after_bind_samplers{};
  std::uint32_t max_per_stage_descriptor_update_after_bind_sampled_images{};
  std::uint32_t max_per_stage_update_after_bind_resources{};
  std::uint32_t max_descriptor_set_update_after_bind_samplers{};
  std::uint32_t max_descriptor_set_update_after_bind_sampled_images{};
  std::uint32_t max_sampler_allocation_count{};
};

struct DescriptorIndexingConfiguration {
  DescriptorBackendRequest request{DescriptorBackendRequest::Automatic};
  std::uint32_t texture_capacity{4096};
  std::uint32_t sampler_capacity{256};
  // Sampler objects that can coexist outside the live table, including
  // completion-delayed versions retained for in-flight frames.
  std::uint64_t additional_sampler_allocation_count{};
  // Resources in addition to the bindless sampled-image table, excluding
  // standalone samplers and including fragment color attachments.
  std::uint32_t additional_per_stage_resource_count{};
};

struct DescriptorIndexingSelection {
  std::uint32_t schema_version{1};
  DescriptorBackendRequest requested_backend{
      DescriptorBackendRequest::Automatic};
  DescriptorBackend selected_backend{DescriptorBackend::Conventional};
  DescriptorFallbackReason fallback_reason{DescriptorFallbackReason::None};
  std::uint32_t texture_capacity{};
  std::uint32_t sampler_capacity{};
  std::uint64_t additional_sampler_allocation_count{};
  std::uint32_t additional_per_stage_resource_count{};
  // True when the configured table sizes satisfy every required feature and
  // limit, independent of a configuration-forced conventional selection.
  bool requirements_satisfied{};
};

[[nodiscard]] DescriptorIndexingSelection SelectDescriptorBackend(
    const DescriptorIndexingConfiguration& configuration,
    const DescriptorIndexingFeatures& features,
    const DescriptorIndexingLimits& limits) noexcept;

}  // namespace merlin::vulkan
