#include <merlin/vulkan/descriptor_indexing.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

using merlin::vulkan::DescriptorBackend;
using merlin::vulkan::DescriptorBackendRequest;
using merlin::vulkan::DescriptorFallbackCategory;
using merlin::vulkan::DescriptorFallbackReason;
using merlin::vulkan::DescriptorIndexingConfiguration;
using merlin::vulkan::DescriptorIndexingFeatures;
using merlin::vulkan::DescriptorIndexingLimits;

DescriptorIndexingFeatures FullFeatures() {
  return {true, true, true, true, true};
}

DescriptorIndexingLimits FullLimits() {
  return {8192, 1024, 8192, 8192, 1024, 8192, 1024};
}

void ExpectFallback(const DescriptorIndexingConfiguration& configuration,
                    const DescriptorIndexingFeatures& features,
                    const DescriptorIndexingLimits& limits,
                    DescriptorFallbackReason reason,
                    DescriptorFallbackCategory category) {
  const auto selection = merlin::vulkan::SelectDescriptorBackend(
      configuration, features, limits);
  assert(selection.selected_backend == DescriptorBackend::Conventional);
  assert(selection.fallback_reason == reason);
  assert(selection.requirements_satisfied ==
         (reason ==
          DescriptorFallbackReason::ConfigurationForcedConventional));
  assert(merlin::vulkan::DescriptorFallbackReasonCategory(reason) == category);
  assert(merlin::vulkan::DescriptorFallbackReasonName(reason) != "unknown");
}

}  // namespace

int main() {
  DescriptorIndexingConfiguration configuration;
  const auto features = FullFeatures();
  const auto limits = FullLimits();

  const auto automatic = merlin::vulkan::SelectDescriptorBackend(
      configuration, features, limits);
  assert(automatic.schema_version == 1);
  assert(automatic.selected_backend == DescriptorBackend::Bindless);
  assert(automatic.fallback_reason == DescriptorFallbackReason::None);
  assert(automatic.requirements_satisfied);
  assert(merlin::vulkan::DescriptorBackendRequestName(
             automatic.requested_backend) == "automatic");
  assert(merlin::vulkan::DescriptorBackendName(automatic.selected_backend) ==
         "bindless");

  configuration.request = DescriptorBackendRequest::Conventional;
  const auto conventional = merlin::vulkan::SelectDescriptorBackend(
      configuration, features, limits);
  assert(conventional.requirements_satisfied);
  ExpectFallback(configuration, features, limits,
                 DescriptorFallbackReason::ConfigurationForcedConventional,
                 DescriptorFallbackCategory::Configuration);
  configuration.request = DescriptorBackendRequest::Bindless;
  const auto forced_bindless = merlin::vulkan::SelectDescriptorBackend(
      configuration, features, limits);
  assert(forced_bindless.selected_backend == DescriptorBackend::Bindless);
  assert(forced_bindless.requirements_satisfied);

  const std::array feature_failures{
      DescriptorFallbackReason::
          FeatureSampledImageNonUniformIndexingMissing,
      DescriptorFallbackReason::FeatureSampledImageUpdateAfterBindMissing,
      DescriptorFallbackReason::FeaturePartiallyBoundMissing,
      DescriptorFallbackReason::FeatureVariableDescriptorCountMissing,
      DescriptorFallbackReason::FeatureRuntimeDescriptorArrayMissing,
  };
  const std::array feature_members{
      &DescriptorIndexingFeatures::
          shader_sampled_image_array_non_uniform_indexing,
      &DescriptorIndexingFeatures::
          descriptor_binding_sampled_image_update_after_bind,
      &DescriptorIndexingFeatures::descriptor_binding_partially_bound,
      &DescriptorIndexingFeatures::descriptor_binding_variable_descriptor_count,
      &DescriptorIndexingFeatures::runtime_descriptor_array,
  };
  for (std::size_t index = 0; index < feature_failures.size(); ++index) {
    auto missing = features;
    missing.*feature_members[index] = false;
    ExpectFallback(configuration, missing, limits, feature_failures[index],
                   DescriptorFallbackCategory::Feature);
  }

  auto invalid = configuration;
  invalid.texture_capacity =
      merlin::vulkan::kReservedBindlessTextureSlots - 1;
  ExpectFallback(invalid, features, limits,
                 DescriptorFallbackReason::ConfigurationTextureCapacityInvalid,
                 DescriptorFallbackCategory::Configuration);
  invalid = configuration;
  invalid.sampler_capacity = 0;
  ExpectFallback(invalid, features, limits,
                 DescriptorFallbackReason::ConfigurationSamplerCapacityInvalid,
                 DescriptorFallbackCategory::Configuration);

  // Standalone sampler descriptors have their own per-stage limit and do not
  // consume maxPerStageResources.
  auto sampler_excluded = limits;
  sampler_excluded.max_per_stage_update_after_bind_resources =
      configuration.texture_capacity;
  const auto sampler_excluded_selection =
      merlin::vulkan::SelectDescriptorBackend(configuration, features,
                                               sampler_excluded);
  assert(sampler_excluded_selection.selected_backend ==
         DescriptorBackend::Bindless);

  auto resource_overhead = configuration;
  resource_overhead.additional_per_stage_resource_count = 4;
  auto overhead_limit = limits;
  overhead_limit.max_per_stage_update_after_bind_resources =
      resource_overhead.texture_capacity +
      resource_overhead.additional_per_stage_resource_count - 1;
  ExpectFallback(resource_overhead, features, overhead_limit,
                 DescriptorFallbackReason::LimitPerStageResourcesInsufficient,
                 DescriptorFallbackCategory::Limit);
  ++overhead_limit.max_per_stage_update_after_bind_resources;
  const auto exact_resource_limit = merlin::vulkan::SelectDescriptorBackend(
      resource_overhead, features, overhead_limit);
  assert(exact_resource_limit.selected_backend == DescriptorBackend::Bindless);

  auto retained_samplers = configuration;
  retained_samplers.additional_sampler_allocation_count = 768;
  auto sampler_allocation_limit = limits;
  sampler_allocation_limit.max_sampler_allocation_count =
      static_cast<std::uint32_t>(
          retained_samplers.sampler_capacity +
          retained_samplers.additional_sampler_allocation_count - 1);
  ExpectFallback(
      retained_samplers, features, sampler_allocation_limit,
      DescriptorFallbackReason::LimitSamplerAllocationCountInsufficient,
      DescriptorFallbackCategory::Limit);
  ++sampler_allocation_limit.max_sampler_allocation_count;
  const auto exact_sampler_allocation_limit =
      merlin::vulkan::SelectDescriptorBackend(
          retained_samplers, features, sampler_allocation_limit);
  assert(exact_sampler_allocation_limit.selected_backend ==
         DescriptorBackend::Bindless);

  const std::array limit_failures{
      DescriptorFallbackReason::LimitAllPoolsInsufficient,
      DescriptorFallbackReason::LimitSamplerAllocationCountInsufficient,
      DescriptorFallbackReason::LimitPerStageSamplersInsufficient,
      DescriptorFallbackReason::LimitPerStageSampledImagesInsufficient,
      DescriptorFallbackReason::LimitPerStageResourcesInsufficient,
      DescriptorFallbackReason::LimitDescriptorSetSamplersInsufficient,
      DescriptorFallbackReason::LimitDescriptorSetSampledImagesInsufficient,
  };
  const std::array limit_members{
      &DescriptorIndexingLimits::max_update_after_bind_descriptors_in_all_pools,
      &DescriptorIndexingLimits::max_sampler_allocation_count,
      &DescriptorIndexingLimits::
          max_per_stage_descriptor_update_after_bind_samplers,
      &DescriptorIndexingLimits::
          max_per_stage_descriptor_update_after_bind_sampled_images,
      &DescriptorIndexingLimits::max_per_stage_update_after_bind_resources,
      &DescriptorIndexingLimits::max_descriptor_set_update_after_bind_samplers,
      &DescriptorIndexingLimits::
          max_descriptor_set_update_after_bind_sampled_images,
  };
  for (std::size_t index = 0; index < limit_failures.size(); ++index) {
    auto insufficient = limits;
    insufficient.*limit_members[index] = 0;
    ExpectFallback(configuration, features, insufficient,
                   limit_failures[index], DescriptorFallbackCategory::Limit);
  }

  std::cout << "Descriptor-indexing negotiation verified\n";
}
