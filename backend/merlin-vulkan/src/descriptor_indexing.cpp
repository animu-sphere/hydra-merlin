#include <merlin/vulkan/descriptor_indexing.hpp>

#include <cstdint>

namespace merlin::vulkan {
namespace {

DescriptorFallbackReason FindRequirementFailure(
    const DescriptorIndexingConfiguration& configuration,
    const DescriptorIndexingFeatures& features,
    const DescriptorIndexingLimits& limits) noexcept {
  if (configuration.texture_capacity < kReservedBindlessTextureSlots) {
    return DescriptorFallbackReason::ConfigurationTextureCapacityInvalid;
  }
  if (configuration.sampler_capacity == 0) {
    return DescriptorFallbackReason::ConfigurationSamplerCapacityInvalid;
  }
  if (!features.shader_sampled_image_array_non_uniform_indexing) {
    return DescriptorFallbackReason::
        FeatureSampledImageNonUniformIndexingMissing;
  }
  if (!features.descriptor_binding_sampled_image_update_after_bind) {
    return DescriptorFallbackReason::FeatureSampledImageUpdateAfterBindMissing;
  }
  if (!features.descriptor_binding_partially_bound) {
    return DescriptorFallbackReason::FeaturePartiallyBoundMissing;
  }
  if (!features.descriptor_binding_variable_descriptor_count) {
    return DescriptorFallbackReason::FeatureVariableDescriptorCountMissing;
  }
  if (!features.runtime_descriptor_array) {
    return DescriptorFallbackReason::FeatureRuntimeDescriptorArrayMissing;
  }

  const auto total_descriptors =
      static_cast<std::uint64_t>(configuration.texture_capacity) +
      configuration.sampler_capacity;
  if (limits.max_update_after_bind_descriptors_in_all_pools <
      total_descriptors) {
    return DescriptorFallbackReason::LimitAllPoolsInsufficient;
  }
  if (limits.max_per_stage_descriptor_update_after_bind_samplers <
      configuration.sampler_capacity) {
    return DescriptorFallbackReason::LimitPerStageSamplersInsufficient;
  }
  if (limits.max_per_stage_descriptor_update_after_bind_sampled_images <
      configuration.texture_capacity) {
    return DescriptorFallbackReason::LimitPerStageSampledImagesInsufficient;
  }
  if (limits.max_per_stage_update_after_bind_resources < total_descriptors) {
    return DescriptorFallbackReason::LimitPerStageResourcesInsufficient;
  }
  if (limits.max_descriptor_set_update_after_bind_samplers <
      configuration.sampler_capacity) {
    return DescriptorFallbackReason::LimitDescriptorSetSamplersInsufficient;
  }
  if (limits.max_descriptor_set_update_after_bind_sampled_images <
      configuration.texture_capacity) {
    return DescriptorFallbackReason::
        LimitDescriptorSetSampledImagesInsufficient;
  }
  return DescriptorFallbackReason::None;
}

}  // namespace

std::string_view DescriptorBackendRequestName(
    DescriptorBackendRequest request) noexcept {
  switch (request) {
    case DescriptorBackendRequest::Automatic: return "automatic";
    case DescriptorBackendRequest::Conventional: return "conventional";
    case DescriptorBackendRequest::Bindless: return "bindless";
  }
  return "unknown";
}

std::string_view DescriptorBackendName(DescriptorBackend backend) noexcept {
  switch (backend) {
    case DescriptorBackend::Conventional: return "conventional";
    case DescriptorBackend::Bindless: return "bindless";
  }
  return "unknown";
}

std::string_view DescriptorFallbackCategoryName(
    DescriptorFallbackCategory category) noexcept {
  switch (category) {
    case DescriptorFallbackCategory::None: return "none";
    case DescriptorFallbackCategory::Configuration: return "configuration";
    case DescriptorFallbackCategory::Feature: return "feature";
    case DescriptorFallbackCategory::Limit: return "limit";
  }
  return "unknown";
}

std::string_view DescriptorFallbackReasonName(
    DescriptorFallbackReason reason) noexcept {
  switch (reason) {
    case DescriptorFallbackReason::None: return "none";
    case DescriptorFallbackReason::ConfigurationForcedConventional:
      return "configuration-forced-conventional";
    case DescriptorFallbackReason::ConfigurationTextureCapacityInvalid:
      return "configuration-texture-capacity-invalid";
    case DescriptorFallbackReason::ConfigurationSamplerCapacityInvalid:
      return "configuration-sampler-capacity-invalid";
    case DescriptorFallbackReason::
        FeatureSampledImageNonUniformIndexingMissing:
      return "feature-sampled-image-non-uniform-indexing-missing";
    case DescriptorFallbackReason::FeatureSampledImageUpdateAfterBindMissing:
      return "feature-sampled-image-update-after-bind-missing";
    case DescriptorFallbackReason::FeaturePartiallyBoundMissing:
      return "feature-partially-bound-missing";
    case DescriptorFallbackReason::FeatureVariableDescriptorCountMissing:
      return "feature-variable-descriptor-count-missing";
    case DescriptorFallbackReason::FeatureRuntimeDescriptorArrayMissing:
      return "feature-runtime-descriptor-array-missing";
    case DescriptorFallbackReason::LimitAllPoolsInsufficient:
      return "limit-all-pools-insufficient";
    case DescriptorFallbackReason::LimitPerStageSamplersInsufficient:
      return "limit-per-stage-samplers-insufficient";
    case DescriptorFallbackReason::LimitPerStageSampledImagesInsufficient:
      return "limit-per-stage-sampled-images-insufficient";
    case DescriptorFallbackReason::LimitPerStageResourcesInsufficient:
      return "limit-per-stage-resources-insufficient";
    case DescriptorFallbackReason::LimitDescriptorSetSamplersInsufficient:
      return "limit-descriptor-set-samplers-insufficient";
    case DescriptorFallbackReason::
        LimitDescriptorSetSampledImagesInsufficient:
      return "limit-descriptor-set-sampled-images-insufficient";
  }
  return "unknown";
}

DescriptorFallbackCategory DescriptorFallbackReasonCategory(
    DescriptorFallbackReason reason) noexcept {
  switch (reason) {
    case DescriptorFallbackReason::None:
      return DescriptorFallbackCategory::None;
    case DescriptorFallbackReason::ConfigurationForcedConventional:
    case DescriptorFallbackReason::ConfigurationTextureCapacityInvalid:
    case DescriptorFallbackReason::ConfigurationSamplerCapacityInvalid:
      return DescriptorFallbackCategory::Configuration;
    case DescriptorFallbackReason::
        FeatureSampledImageNonUniformIndexingMissing:
    case DescriptorFallbackReason::FeatureSampledImageUpdateAfterBindMissing:
    case DescriptorFallbackReason::FeaturePartiallyBoundMissing:
    case DescriptorFallbackReason::FeatureVariableDescriptorCountMissing:
    case DescriptorFallbackReason::FeatureRuntimeDescriptorArrayMissing:
      return DescriptorFallbackCategory::Feature;
    case DescriptorFallbackReason::LimitAllPoolsInsufficient:
    case DescriptorFallbackReason::LimitPerStageSamplersInsufficient:
    case DescriptorFallbackReason::LimitPerStageSampledImagesInsufficient:
    case DescriptorFallbackReason::LimitPerStageResourcesInsufficient:
    case DescriptorFallbackReason::LimitDescriptorSetSamplersInsufficient:
    case DescriptorFallbackReason::
        LimitDescriptorSetSampledImagesInsufficient:
      return DescriptorFallbackCategory::Limit;
  }
  return DescriptorFallbackCategory::None;
}

DescriptorIndexingSelection SelectDescriptorBackend(
    const DescriptorIndexingConfiguration& configuration,
    const DescriptorIndexingFeatures& features,
    const DescriptorIndexingLimits& limits) noexcept {
  DescriptorIndexingSelection selection;
  selection.requested_backend = configuration.request;
  selection.texture_capacity = configuration.texture_capacity;
  selection.sampler_capacity = configuration.sampler_capacity;

  const auto failure = FindRequirementFailure(configuration, features, limits);
  selection.requirements_satisfied = failure == DescriptorFallbackReason::None;
  if (configuration.request == DescriptorBackendRequest::Conventional) {
    selection.fallback_reason =
        DescriptorFallbackReason::ConfigurationForcedConventional;
    return selection;
  }
  if (failure != DescriptorFallbackReason::None) {
    selection.fallback_reason = failure;
    return selection;
  }
  selection.selected_backend = DescriptorBackend::Bindless;
  return selection;
}

}  // namespace merlin::vulkan
