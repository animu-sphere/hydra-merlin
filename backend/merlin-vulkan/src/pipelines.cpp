#include "renderer_impl.hpp"

#include <merlin/render/surface_geometry.hpp>

namespace merlin::vulkan {

void Renderer::Impl::EnsureBindlessMaterialDescriptorSetLayout() {
  if (bindless_material_descriptor_set_layout_ != VK_NULL_HANDLE) {
    ++frame_counters_.descriptor_layout_cache_hits;
    return;
  }
  std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
  bindings[0].binding = shader_abi::kBindlessMaterialConstants.binding;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  const std::array table_bindings{
      shader_abi::kGpuSceneGeometries.binding,
      shader_abi::kGpuSceneInstances.binding,
      shader_abi::kGpuSceneMaterials.binding,
      shader_abi::kGpuSceneDraws.binding,
  };
  for (std::size_t i = 0; i < table_bindings.size(); ++i) {
    auto& binding = bindings[i + 1];
    binding.binding = table_bindings[i];
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
        VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  info.bindingCount = static_cast<std::uint32_t>(bindings.size());
  info.pBindings = bindings.data();
  Check(vkCreateDescriptorSetLayout(
            device_, &info, nullptr,
            &bindless_material_descriptor_set_layout_),
      "create bindless material descriptor layout");
  ++frame_counters_.descriptor_layout_cache_misses;
}

void Renderer::Impl::EnsureGpuDrivenDescriptorAndPipelineLayouts() {
  if (gpu_driven_descriptor_set_layout_ != VK_NULL_HANDLE) {
    return;
  }
  std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
  const std::array binding_numbers{
      shader_abi::kGpuDrivenCandidateDrawSlots.binding,
      shader_abi::kGpuDrivenCandidateResults.binding,
      shader_abi::kGpuDrivenIndirectCommands.binding,
      shader_abi::kGpuDrivenDispatchCounters.binding,
  };
  for (std::size_t i = 0; i < bindings.size(); ++i) {
    bindings[i].binding = binding_numbers[i];
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
  layout_info.pBindings = bindings.data();
  Check(vkCreateDescriptorSetLayout(device_, &layout_info, nullptr,
            &gpu_driven_descriptor_set_layout_),
      "create GPU-driven descriptor layout");
  ++frame_counters_.descriptor_layout_cache_misses;

  const std::array set_layouts{
      bindless_descriptor_set_layout_,
      bindless_material_descriptor_set_layout_,
      gpu_driven_descriptor_set_layout_,
  };
  VkPushConstantRange push_range{};
  push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  push_range.size = sizeof(shader_abi::GpuDrivenIndexedConstants);
  VkPipelineLayoutCreateInfo pipeline_layout_info{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pipeline_layout_info.setLayoutCount =
      static_cast<std::uint32_t>(set_layouts.size());
  pipeline_layout_info.pSetLayouts = set_layouts.data();
  pipeline_layout_info.pushConstantRangeCount = 1;
  pipeline_layout_info.pPushConstantRanges = &push_range;
  Check(vkCreatePipelineLayout(device_, &pipeline_layout_info, nullptr,
            &gpu_driven_pipeline_layout_),
      "create GPU-driven pipeline layout");
}

VkPipeline Renderer::Impl::EnsureGpuDrivenComputePipeline(const ShaderPaths& shaders) {
  EnsureGpuDrivenDescriptorAndPipelineLayouts();
  const auto path = shaders.gpu_driven_compute.empty()
                        ? shaders.bindless_vertex.parent_path() /
                              "gpu-driven-indexed.comp.spv"
                        : shaders.gpu_driven_compute;
  const auto found = gpu_driven_pipelines_.find(path);
  if (found != gpu_driven_pipelines_.end()) {
    ++frame_counters_.pipeline_cache_hits;
    return found->second;
  }
  VkPipelineShaderStageCreateInfo stage{
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stage.module = GetShaderModule(path);
  stage.pName = "main";
  VkComputePipelineCreateInfo info{
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  info.stage = stage;
  info.layout = gpu_driven_pipeline_layout_;
  VkPipeline pipeline{};
  Check(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr,
            &pipeline),
      "create GPU-driven compute pipeline");
  gpu_driven_pipelines_.emplace(path, pipeline);
  ++frame_counters_.pipeline_creation_count;
  ++frame_counters_.pipeline_cache_misses;
  return pipeline;
}

void Renderer::Impl::EnsureGaussianPrepareDescriptorAndPipelineLayouts() {
  if (gaussian_prepare_descriptor_set_layout_ != VK_NULL_HANDLE) {
    return;
  }
  VkDescriptorSetLayoutCreateInfo empty_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  Check(vkCreateDescriptorSetLayout(
            device_, &empty_info, nullptr,
            &gaussian_prepare_empty_descriptor_set_layout_),
      "create Gaussian preparation empty descriptor layout");

  std::array<VkDescriptorSetLayoutBinding,
      kGaussianPrepareStorageBufferCount +
          kGaussianPrepareUniformBufferCount>
      bindings{};
  static constexpr std::array storage_bindings{
      shader_abi::kGaussianPositions.binding,
      shader_abi::kGaussianCovariances.binding,
      shader_abi::kGaussianOpacities.binding,
      shader_abi::kGaussianRadiance.binding,
      shader_abi::kGaussianCandidateResults.binding,
      shader_abi::kGaussianPreparedRecords.binding,
      shader_abi::kGaussianPrepareCounters.binding,
  };
  static_assert(storage_bindings.size() ==
                kGaussianPrepareStorageBufferCount);
  for (std::size_t i = 0; i < storage_bindings.size(); ++i) {
    bindings[i].binding = storage_bindings[i];
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  bindings.back().binding = shader_abi::kGaussianPrepareConstants.binding;
  bindings.back().descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  bindings.back().descriptorCount = 1;
  bindings.back().stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
  layout_info.pBindings = bindings.data();
  Check(vkCreateDescriptorSetLayout(
            device_, &layout_info, nullptr,
            &gaussian_prepare_descriptor_set_layout_),
      "create Gaussian preparation descriptor layout");
  frame_counters_.descriptor_layout_cache_misses += 2;

  const std::array set_layouts{
      gaussian_prepare_empty_descriptor_set_layout_,
      gaussian_prepare_empty_descriptor_set_layout_,
      gaussian_prepare_empty_descriptor_set_layout_,
      gaussian_prepare_descriptor_set_layout_,
  };
  VkPipelineLayoutCreateInfo pipeline_layout_info{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pipeline_layout_info.setLayoutCount =
      static_cast<std::uint32_t>(set_layouts.size());
  pipeline_layout_info.pSetLayouts = set_layouts.data();
  Check(vkCreatePipelineLayout(device_, &pipeline_layout_info, nullptr,
            &gaussian_prepare_pipeline_layout_),
      "create Gaussian preparation pipeline layout");
}

VkPipeline Renderer::Impl::EnsureGaussianPrepareComputePipeline(const ShaderPaths& shaders) {
  EnsureGaussianPrepareDescriptorAndPipelineLayouts();
  const auto path = GaussianPrepareComputePath(shaders);
  const auto found = gaussian_prepare_compute_pipelines_.find(path);
  if (found != gaussian_prepare_compute_pipelines_.end()) {
    ++frame_counters_.pipeline_cache_hits;
    return found->second;
  }
  VkPipelineShaderStageCreateInfo stage{
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stage.module = GetShaderModule(path);
  stage.pName = "main";
  VkComputePipelineCreateInfo info{
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  info.stage = stage;
  info.layout = gaussian_prepare_pipeline_layout_;
  VkPipeline pipeline{};
  Check(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr,
            &pipeline),
      "create Gaussian preparation compute pipeline");
  gaussian_prepare_compute_pipelines_.emplace(path, pipeline);
  ++frame_counters_.pipeline_creation_count;
  ++frame_counters_.pipeline_cache_misses;
  return pipeline;
}

void Renderer::Impl::EnsureGaussianSortDescriptorAndPipelineLayouts() {
  if (gaussian_sort_descriptor_set_layout_ != VK_NULL_HANDLE) {
    return;
  }
  static constexpr std::array bindings_in_set{
      shader_abi::kGaussianSortSource.binding,
      shader_abi::kGaussianSortDestination.binding,
      shader_abi::kGaussianSortScan.binding,
      shader_abi::kGaussianSortPreparedRecords.binding,
  };
  static_assert(bindings_in_set.size() == kGaussianSortStorageBufferCount);
  std::array<VkDescriptorSetLayoutBinding, kGaussianSortStorageBufferCount>
      bindings{};
  for (std::size_t i = 0; i < bindings.size(); ++i) {
    bindings[i].binding = bindings_in_set[i];
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
  layout_info.pBindings = bindings.data();
  Check(vkCreateDescriptorSetLayout(device_, &layout_info, nullptr,
            &gaussian_sort_descriptor_set_layout_),
      "create Gaussian sort descriptor layout");
  ++frame_counters_.descriptor_layout_cache_misses;

  VkPushConstantRange push_constants{};
  push_constants.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  push_constants.size = sizeof(shader_abi::GaussianSortConstants);
  VkPipelineLayoutCreateInfo pipeline_layout_info{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pipeline_layout_info.setLayoutCount = 1;
  pipeline_layout_info.pSetLayouts = &gaussian_sort_descriptor_set_layout_;
  pipeline_layout_info.pushConstantRangeCount = 1;
  pipeline_layout_info.pPushConstantRanges = &push_constants;
  Check(vkCreatePipelineLayout(device_, &pipeline_layout_info, nullptr,
            &gaussian_sort_pipeline_layout_),
      "create Gaussian sort pipeline layout");
}

std::array<VkPipeline, Renderer::Impl::kGaussianSortKernelCount>
Renderer::Impl::EnsureGaussianSortComputePipelines(const ShaderPaths& shaders) {
  EnsureGaussianSortDescriptorAndPipelineLayouts();
  static constexpr std::array<const char*, kGaussianSortKernelCount>
      artifacts{
          "gaussian-sort-keys.comp.spv",
          "gaussian-sort-histogram.comp.spv",
          "gaussian-sort-scan.comp.spv",
          "gaussian-sort-scan-add.comp.spv",
          "gaussian-sort-scatter.comp.spv",
          "gaussian-sort-verify.comp.spv",
      };
  const auto directory = GaussianSortDirectory(shaders);
  std::array<VkPipeline, kGaussianSortKernelCount> pipelines{};
  for (std::size_t kernel = 0; kernel < artifacts.size(); ++kernel) {
    pipelines[kernel] =
        EnsureGaussianSortLayoutPipeline(directory / artifacts[kernel]);
  }
  return pipelines;
}

// The sort kernels and the raster gather share one set layout and pipeline
// layout, so their pipelines share one cache keyed by artifact path.
VkPipeline Renderer::Impl::EnsureGaussianSortLayoutPipeline(
    const std::filesystem::path& path) {
  const auto found = gaussian_sort_compute_pipelines_.find(path);
  if (found != gaussian_sort_compute_pipelines_.end()) {
    ++frame_counters_.pipeline_cache_hits;
    return found->second;
  }
  VkPipelineShaderStageCreateInfo stage{
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stage.module = GetShaderModule(path);
  stage.pName = "main";
  VkComputePipelineCreateInfo info{
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  info.stage = stage;
  info.layout = gaussian_sort_pipeline_layout_;
  VkPipeline pipeline{};
  Check(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &info,
            nullptr, &pipeline),
      "create Gaussian sort compute pipeline");
  gaussian_sort_compute_pipelines_.emplace(path, pipeline);
  ++frame_counters_.pipeline_creation_count;
  ++frame_counters_.pipeline_cache_misses;
  return pipeline;
}

VkPipeline Renderer::Impl::EnsureGaussianRasterGatherPipeline(const ShaderPaths& shaders) {
  EnsureGaussianSortDescriptorAndPipelineLayouts();
  return EnsureGaussianSortLayoutPipeline(
      shaders.gaussian_raster_gather_compute.empty()
          ? GaussianSortDirectory(shaders) /
                "gaussian-raster-gather.comp.spv"
          : shaders.gaussian_raster_gather_compute);
}

std::array<VkPipeline, Renderer::Impl::kGaussianTileKernelCount>
Renderer::Impl::EnsureGaussianTileComputePipelines(const ShaderPaths& shaders) {
  EnsureGaussianSortDescriptorAndPipelineLayouts();
  static constexpr std::array<const char*, kGaussianTileKernelCount>
      artifacts{
          "gaussian-tile-count.comp.spv",
          "gaussian-tile-emit.comp.spv",
          "gaussian-tile-ranges.comp.spv",
          "gaussian-tile-verify.comp.spv",
      };
  const auto directory = GaussianTileDirectory(shaders);
  std::array<VkPipeline, kGaussianTileKernelCount> pipelines{};
  for (std::size_t kernel = 0; kernel < artifacts.size(); ++kernel) {
    pipelines[kernel] =
        EnsureGaussianSortLayoutPipeline(directory / artifacts[kernel]);
  }
  return pipelines;
}

void Renderer::Impl::EnsureGaussianTileRasterDescriptorAndPipelineLayouts() {
  if (gaussian_tile_raster_descriptor_set_layout_ != VK_NULL_HANDLE) {
    return;
  }
  static constexpr std::array bindings_in_set{
      shader_abi::kGaussianTileRasterPairs,
      shader_abi::kGaussianTileRasterControl,
      shader_abi::kGaussianTileRasterRecords,
      shader_abi::kGaussianTileRasterDraw,
      shader_abi::kGaussianTileRasterColor,
      shader_abi::kGaussianTileRasterPrimId,
      shader_abi::kGaussianTileRasterInstanceId,
      shader_abi::kGaussianTileRasterDepth,
  };
  std::array<VkDescriptorSetLayoutBinding, bindings_in_set.size()>
      bindings{};
  for (std::size_t i = 0; i < bindings.size(); ++i) {
    bindings[i].binding = bindings_in_set[i].binding;
    switch (bindings_in_set[i].resource_class) {
    case shader_abi::ResourceClass::StorageImage:
      bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      break;
    case shader_abi::ResourceClass::SampledImage:
      bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      break;
    default:
      bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      break;
    }
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
  layout_info.pBindings = bindings.data();
  Check(vkCreateDescriptorSetLayout(device_, &layout_info, nullptr,
            &gaussian_tile_raster_descriptor_set_layout_),
      "create Gaussian tile raster descriptor layout");
  ++frame_counters_.descriptor_layout_cache_misses;

  VkPushConstantRange push_constants{};
  push_constants.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  push_constants.size = sizeof(shader_abi::GaussianTileRasterConstants);
  VkPipelineLayoutCreateInfo pipeline_layout_info{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pipeline_layout_info.setLayoutCount = 1;
  pipeline_layout_info.pSetLayouts =
      &gaussian_tile_raster_descriptor_set_layout_;
  pipeline_layout_info.pushConstantRangeCount = 1;
  pipeline_layout_info.pPushConstantRanges = &push_constants;
  Check(vkCreatePipelineLayout(device_, &pipeline_layout_info, nullptr,
            &gaussian_tile_raster_pipeline_layout_),
      "create Gaussian tile raster pipeline layout");
}

VkPipeline Renderer::Impl::EnsureGaussianTileRasterPipeline(
    const std::filesystem::path& path) {
  EnsureGaussianTileRasterDescriptorAndPipelineLayouts();
  const auto found = gaussian_tile_raster_compute_pipelines_.find(path);
  if (found != gaussian_tile_raster_compute_pipelines_.end()) {
    ++frame_counters_.pipeline_cache_hits;
    return found->second;
  }
  VkPipelineShaderStageCreateInfo stage{
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stage.module = GetShaderModule(path);
  stage.pName = "main";
  VkComputePipelineCreateInfo info{
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  info.stage = stage;
  info.layout = gaussian_tile_raster_pipeline_layout_;
  VkPipeline pipeline{};
  Check(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &info,
            nullptr, &pipeline),
      "create Gaussian tile raster compute pipeline");
  gaussian_tile_raster_compute_pipelines_.emplace(path, pipeline);
  ++frame_counters_.pipeline_creation_count;
  ++frame_counters_.pipeline_cache_misses;
  return pipeline;
}

void Renderer::Impl::EnsureDescriptorSetLayout() {
  if (descriptor_set_layout_ != VK_NULL_HANDLE) {
    ++frame_counters_.descriptor_layout_cache_hits;
    return;
  }
  ++frame_counters_.descriptor_layout_cache_misses;
  const auto texture_binding = [](shader_abi::ResourceBinding resource) {
    return VkDescriptorSetLayoutBinding{resource.binding,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
        VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  };
  const std::array bindings{
      texture_binding(shader_abi::kConventionalBaseColorTexture),
      texture_binding(shader_abi::kConventionalNormalTexture),
      texture_binding(shader_abi::kConventionalMetallicTexture),
      texture_binding(shader_abi::kConventionalRoughnessTexture),
      texture_binding(shader_abi::kConventionalEmissiveTexture),
      VkDescriptorSetLayoutBinding{shader_abi::kConventionalMaterialConstants.binding,
          VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
  };
  VkDescriptorSetLayoutCreateInfo info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  info.bindingCount = static_cast<std::uint32_t>(bindings.size());
  info.pBindings = bindings.data();
  Check(vkCreateDescriptorSetLayout(device_, &info, nullptr,
            &descriptor_set_layout_),
      "create material descriptor layout");
}

void Renderer::Impl::EnsureGeneratedDescriptorSetLayouts() {
  for (const auto& [module_key, artifact] : generated_material_artifacts_) {
    if (generated_descriptor_set_layouts_.contains(module_key)) {
      ++frame_counters_.descriptor_layout_cache_hits;
      continue;
    }
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.push_back(
        {artifact.parameter_binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
            VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    for (const auto& resource : artifact.resource_bindings) {
      if (resource.type == MaterialValueType::Texture2D ||
          resource.type == MaterialValueType::CombinedTextureSampler) {
        bindings.push_back(
            {resource.texture_binding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                resource.array_size, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
      }
      if (resource.type == MaterialValueType::Sampler ||
          resource.type == MaterialValueType::CombinedTextureSampler) {
        bindings.push_back(
            {resource.sampler_binding, VK_DESCRIPTOR_TYPE_SAMPLER,
                resource.array_size, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
      }
    }
    bindings.push_back(
        {artifact.material_constants_binding,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            nullptr});
    std::sort(bindings.begin(), bindings.end(),
        [](const auto& left, const auto& right) {
          return left.binding < right.binding;
        });
    const auto duplicate = std::adjacent_find(
        bindings.begin(), bindings.end(), [](const auto& left, const auto& right) {
          return left.binding == right.binding;
        });
    if (duplicate != bindings.end() ||
        std::any_of(bindings.begin(), bindings.end(), [](const auto& binding) {
          return binding.descriptorCount == 0U;
        })) {
      continue;
    }
    VkDescriptorSetLayoutCreateInfo info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VkDescriptorSetLayout layout{};
    Check(vkCreateDescriptorSetLayout(device_, &info, nullptr, &layout),
        "create generated material descriptor layout");
    generated_descriptor_set_layouts_.emplace(module_key, layout);
    ++frame_counters_.descriptor_layout_cache_misses;
  }
}

VkShaderModule Renderer::Impl::GetShaderModule(const std::filesystem::path& path) {
  const auto found = shader_modules_.find(path);
  if (found != shader_modules_.end()) {
    ++frame_counters_.shader_module_cache_hits;
    return found->second;
  }
  ++frame_counters_.shader_module_cache_misses;
  const auto code = ReadSpirv(path);
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = code.size() * sizeof(std::uint32_t);
  info.pCode = code.data();
  VkShaderModule module{};
  Check(vkCreateShaderModule(device_, &info, nullptr, &module),
      "create shader module");
  shader_modules_.emplace(path, module);
  return module;
}

VkPipeline Renderer::Impl::CreatePipeline(
    const ShaderPaths& shaders, std::uint32_t variant_key,
    const GeneratedMaterialArtifact* generated_artifact,
    bool gpu_driven_pipeline, bool dome_pipeline,
    render::MeshDisplayMode display_mode) {
  ++frame_counters_.pipeline_creation_count;
  const bool mesh_display = display_mode != render::MeshDisplayMode::Smooth;
  const bool points = display_mode == render::MeshDisplayMode::Points;
  const bool gpu_scene_pipeline =
      generated_artifact == nullptr &&
      (variant_key & kGpuScenePipelineFlag) != 0U;
  const auto gpu_scene_vertex =
      shaders.gpu_scene_vertex.empty()
          ? shaders.bindless_vertex.parent_path() /
                "triangle.gpu-scene.vert.spv"
          : shaders.gpu_scene_vertex;
  const auto gpu_scene_fragment =
      shaders.gpu_scene_fragment.empty()
          ? shaders.bindless_fragment.parent_path() /
                "triangle.gpu-scene.frag.spv"
          : shaders.gpu_scene_fragment;
  const auto gpu_driven_vertex =
      shaders.gpu_driven_vertex.empty()
          ? shaders.bindless_vertex.parent_path() /
                "gpu-driven-forward.vert.spv"
          : shaders.gpu_driven_vertex;
  const auto gpu_driven_fragment =
      shaders.gpu_driven_fragment.empty()
          ? shaders.bindless_fragment.parent_path() /
                "gpu-driven-forward.frag.spv"
          : shaders.gpu_driven_fragment;
  const auto vertex_shader = GetShaderModule(
      dome_pipeline ? shaders.vertex.parent_path() / "dome.vert.spv"
      : mesh_display ? shaders.vertex.parent_path() / "mesh-display.vert.spv"
      : gpu_driven_pipeline
          ? gpu_driven_vertex
      : gpu_scene_pipeline
          ? gpu_scene_vertex
          : (bindless_texture_table_ ? shaders.bindless_vertex
                                     : shaders.vertex));
  const auto fragment_path =
      dome_pipeline ? shaders.fragment.parent_path() / "dome.frag.spv"
      : mesh_display ? shaders.fragment.parent_path() / "mesh-display.frag.spv"
      : generated_artifact != nullptr
          ? generated_artifact->fragment
          : (gpu_driven_pipeline
                    ? gpu_driven_fragment
                : gpu_scene_pipeline
                    ? gpu_scene_fragment
                    : (bindless_texture_table_ ? shaders.bindless_fragment
                                               : shaders.fragment));
  const auto fragment_shader = GetShaderModule(fragment_path);
  const auto* fragment_entry =
      generated_artifact != nullptr
          ? generated_artifact->fragment_entry_point.c_str()
          : "main";
  const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                                                   VK_SHADER_STAGE_VERTEX_BIT, vertex_shader, "main", nullptr},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
          VK_SHADER_STAGE_FRAGMENT_BIT, fragment_shader, fragment_entry,
          nullptr}}};
  const VkVertexInputBindingDescription binding{
      0, sizeof(extraction::DrawVertex), VK_VERTEX_INPUT_RATE_VERTEX};
  const std::array<VkVertexInputAttributeDescription, 5> attributes{{
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT,
          static_cast<std::uint32_t>(offsetof(extraction::DrawVertex, position))},
      {1, 0, VK_FORMAT_R32G32B32_SFLOAT,
          static_cast<std::uint32_t>(offsetof(extraction::DrawVertex, normal))},
      {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
          static_cast<std::uint32_t>(offsetof(extraction::DrawVertex, color))},
      {3, 0, VK_FORMAT_R32G32_SFLOAT,
          static_cast<std::uint32_t>(offsetof(extraction::DrawVertex, texcoord))},
      {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
          static_cast<std::uint32_t>(offsetof(extraction::DrawVertex, tangent))},
  }};
  const VkVertexInputBindingDescription display_binding{0,
      sizeof(render::MeshDisplayVertex),
      points ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
  const std::array<VkVertexInputAttributeDescription, 2> display_attributes{{
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT,
          offsetof(render::MeshDisplayVertex, position)},
      {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
          offsetof(render::MeshDisplayVertex, color)},
  }};
  VkPipelineVertexInputStateCreateInfo vertex_input{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex_input.vertexBindingDescriptionCount = 1;
  vertex_input.pVertexBindingDescriptions = &binding;
  vertex_input.vertexAttributeDescriptionCount =
      static_cast<std::uint32_t>(attributes.size());
  vertex_input.pVertexAttributeDescriptions = attributes.data();
  if (dome_pipeline) {
    vertex_input.vertexBindingDescriptionCount = 0;
    vertex_input.vertexAttributeDescriptionCount = 0;
  } else if (mesh_display) {
    vertex_input.pVertexBindingDescriptions = &display_binding;
    vertex_input.vertexAttributeDescriptionCount =
        static_cast<std::uint32_t>(display_attributes.size());
    vertex_input.pVertexAttributeDescriptions = display_attributes.data();
  }
  VkPipelineInputAssemblyStateCreateInfo input_assembly{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  input_assembly.topology = mesh_display && !points ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                   : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport_state{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport_state.viewportCount = 1;
  viewport_state.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.depthBiasEnable = !mesh_display && !dome_pipeline;
  raster.cullMode = mesh_display || (variant_key & kDoubleSidedFlag) != 0U
                        ? VK_CULL_MODE_NONE
                        : VK_CULL_MODE_BACK_BIT;
  raster.frontFace =
      (variant_key & kCounterClockwiseFrontFaceFlag) != 0U
          ? VK_FRONT_FACE_COUNTER_CLOCKWISE
          : VK_FRONT_FACE_CLOCKWISE;
  raster.lineWidth = 1.0F;
  VkPipelineMultisampleStateCreateInfo multisample{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depth{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = dome_pipeline ? VK_FALSE : VK_TRUE;
  depth.depthWriteEnable = dome_pipeline ? VK_FALSE : VK_TRUE;
  depth.depthCompareOp = mesh_display ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_LESS;
  VkPipelineColorBlendAttachmentState blend_attachment{};
  blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                    VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT |
                                    VK_COLOR_COMPONENT_A_BIT;
  std::array<VkPipelineColorBlendAttachmentState, 3> blend_attachments{
      blend_attachment, blend_attachment, blend_attachment};
  VkPipelineColorBlendStateCreateInfo blend{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount =
      static_cast<std::uint32_t>(blend_attachments.size());
  blend.pAttachments = blend_attachments.data();
  const std::array<VkDynamicState, 3> dynamic_states{
      VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS};
  VkPipelineDynamicStateCreateInfo dynamic{
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount =
      static_cast<std::uint32_t>(dynamic_states.size());
  dynamic.pDynamicStates = dynamic_states.data();
  VkGraphicsPipelineCreateInfo pipeline_info{
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pipeline_info.stageCount = static_cast<std::uint32_t>(stages.size());
  pipeline_info.pStages = stages.data();
  pipeline_info.pVertexInputState = &vertex_input;
  pipeline_info.pInputAssemblyState = &input_assembly;
  pipeline_info.pViewportState = &viewport_state;
  pipeline_info.pRasterizationState = &raster;
  pipeline_info.pMultisampleState = &multisample;
  pipeline_info.pDepthStencilState = &depth;
  pipeline_info.pColorBlendState = &blend;
  pipeline_info.pDynamicState = &dynamic;
  pipeline_info.layout = dome_pipeline ? dome_pipeline_layout_
                         : mesh_display ? mesh_display_pipeline_layout_
                         : generated_artifact != nullptr
                             ? active_target_->generated_pipeline_layouts.at(
                                   generated_artifact->module_key)
                             : active_target_->pipeline_layout;
  pipeline_info.renderPass = active_target_->render_pass;
  VkPipeline pipeline{};
  Check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1,
            &pipeline_info, nullptr, &pipeline),
      "create scene graphics pipeline");
  return pipeline;
}

void Renderer::Impl::RecordMeshDisplay(VkCommandBuffer command,
    FrameContext& frame, const RenderRequest& request) {
  const auto batch = render::PrepareMeshDisplay(*request.snapshot,
      request.width, request.height, request.mesh_display);
  if (batch.draws.empty()) {
    return;
  }
  const auto bytes = batch.vertices.size() * sizeof(render::MeshDisplayVertex);
  auto& buffer = frame.mesh_display_vertices;
  if (buffer.size < bytes) {
    DestroyBuffer(buffer);
    buffer = CreateBuffer(bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  }
  void* mapped{};
  Check(vkMapMemory(device_, buffer.memory, 0, bytes, 0, &mapped),
      "map mesh display vertices");
  std::memcpy(mapped, batch.vertices.data(), bytes);
  vkUnmapMemory(device_, buffer.memory);

  constexpr auto stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  if (mesh_display_pipeline_layout_ == VK_NULL_HANDLE) {
    const VkPushConstantRange range{stages, 0, sizeof(render::MeshDisplayConstants)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &range;
    Check(vkCreatePipelineLayout(device_, &layout, nullptr, &mesh_display_pipeline_layout_),
        "create mesh display pipeline layout");
  }
  using Mode = render::MeshDisplayMode;
  const bool points = request.mesh_display.mode == Mode::Points;
  auto& pipeline = active_target_->mesh_display_pipelines[points ? 1 : 0];
  if (pipeline == VK_NULL_HANDLE) {
    pipeline = CreatePipeline(active_target_->shaders, 0, nullptr, false, false,
        points ? Mode::Points : Mode::Wireframe);
  }
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  for (const auto& draw : batch.draws) {
    const VkDeviceSize offset = draw.first * sizeof(render::MeshDisplayVertex);
    vkCmdBindVertexBuffers(command, 0, 1, &buffer.handle, &offset);
    vkCmdPushConstants(command, mesh_display_pipeline_layout_, stages, 0,
        sizeof(draw.constants), &draw.constants);
    if (points) {
      vkCmdDraw(command, 6, draw.count, 0, 0);
    } else {
      vkCmdDraw(command, draw.count, 1, 0, 0);
    }
  }
}

void Renderer::Impl::EnsureEnvironmentDescriptorSetLayout() {
  if (environment_descriptor_set_layout_ == VK_NULL_HANDLE) {
    const VkDescriptorSetLayoutBinding binding{shader_abi::kConventionalSpecularEnvironment.binding,
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = 1;
    info.pBindings = &binding;
    Check(vkCreateDescriptorSetLayout(device_, &info, nullptr, &environment_descriptor_set_layout_),
        "create environment descriptor layout");
  }
}

void Renderer::Impl::WriteEnvironmentDescriptor(FrameContext& frame,
    VkDescriptorSet& set, VkImageView view) {
  constexpr auto type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  if (frame.environment_descriptor_pool == VK_NULL_HANDLE) {
    const VkDescriptorPoolSize size{type, 2};
    VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    info.maxSets = 2;
    info.poolSizeCount = 1;
    info.pPoolSizes = &size;
    Check(vkCreateDescriptorPool(device_, &info, nullptr, &frame.environment_descriptor_pool),
        "create environment descriptor pool");
    ++frame_counters_.descriptor_pool_creation_count;
  }
  if (set == VK_NULL_HANDLE) {
    VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    info.descriptorPool = frame.environment_descriptor_pool;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &environment_descriptor_set_layout_;
    Check(vkAllocateDescriptorSets(device_, &info, &set),
        "allocate environment descriptor set");
    ++frame_counters_.descriptor_allocation_count;
  }
  const VkDescriptorImageInfo image{VK_NULL_HANDLE, view,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = set;
  write.dstBinding = shader_abi::kConventionalSpecularEnvironment.binding;
  write.descriptorCount = 1;
  write.descriptorType = type;
  write.pImageInfo = &image;
  vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  ++frame_counters_.descriptor_update_count;
}

void Renderer::Impl::PrepareSpecularEnvironment(FrameContext& frame, TextureHandle atlas) {
  const auto view = atlas.valid() ? texture_slots_.at(atlas.value()).view : FallbackTextureView();
  const auto generation = atlas.valid() ? material_resource_generation_ : 0;
  if (frame.specular_environment_set == VK_NULL_HANDLE ||
      frame.specular_environment_view != view ||
      frame.specular_environment_generation != generation) {
    WriteEnvironmentDescriptor(frame, frame.specular_environment_set, view);
    frame.specular_environment_view = view;
    frame.specular_environment_generation = generation;
  }
}

void Renderer::Impl::RecordDome(VkCommandBuffer command, FrameContext& frame,
    const render::ForwardDomeLight& dome) {
  const auto texture = texture_slots_.find(dome.rgbe_texture.value());
  if (texture == texture_slots_.end()) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        "record dome background", "dome texture is absent from the snapshot");
  }
  if (dome_pipeline_layout_ == VK_NULL_HANDLE) {
    const VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(render::DomeDrawConstants)};
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = 1;
    info.pSetLayouts = &environment_descriptor_set_layout_;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &range;
    Check(vkCreatePipelineLayout(device_, &info, nullptr, &dome_pipeline_layout_),
        "create dome pipeline layout");
  }
  if (active_target_->dome_pipeline == VK_NULL_HANDLE) {
    active_target_->dome_pipeline = CreatePipeline(active_target_->shaders,
        kDoubleSidedFlag, nullptr, false, true);
  }
  WriteEnvironmentDescriptor(frame, frame.dome_descriptor_set, texture->second.view);
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, active_target_->dome_pipeline);
  vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
      dome_pipeline_layout_, 0, 1, &frame.dome_descriptor_set, 0, nullptr);
  const auto constants = render::PackDomeDrawConstants(dome);
  vkCmdPushConstants(command, dome_pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT,
      0, sizeof(constants), &constants);
  vkCmdDraw(command, 3, 1, 0, 0);
}

VkPipeline Renderer::Impl::EnsurePipeline(const ShaderPaths& shaders,
    std::uint32_t variant_key) {
  const auto found = active_target_->pipelines.find(variant_key);
  if (found != active_target_->pipelines.end()) {
    return found->second;
  }
  const auto pipeline = CreatePipeline(shaders, variant_key);
  active_target_->pipelines.emplace(variant_key, pipeline);
  return pipeline;
}

VkPipeline Renderer::Impl::EnsureGpuDrivenGraphicsPipeline(const ShaderPaths& shaders,
    std::uint32_t variant_key) {
  const auto found = active_target_->gpu_driven_pipelines.find(variant_key);
  if (found != active_target_->gpu_driven_pipelines.end()) {
    ++frame_counters_.pipeline_cache_hits;
    return found->second;
  }
  ++frame_counters_.pipeline_cache_misses;
  const auto pipeline = CreatePipeline(shaders, variant_key, nullptr, true);
  active_target_->gpu_driven_pipelines.emplace(variant_key, pipeline);
  return pipeline;
}

VkPipeline Renderer::Impl::EnsureGaussianPipeline(const ShaderPaths& shaders,
    bool id_pass, bool sorted_stream) {
  auto& cached =
      sorted_stream ? (id_pass ? active_target_->gaussian_sorted_id_pipeline
                               : active_target_->gaussian_sorted_pipeline)
                    : (id_pass ? active_target_->gaussian_id_pipeline
                               : active_target_->gaussian_pipeline);
  if (cached != VK_NULL_HANDLE) {
    return cached;
  }
  const auto vertex_path = id_pass
                               ? (shaders.gaussian_id_vertex.empty()
                                         ? shaders.vertex.parent_path() / "gaussian-id.vert.spv"
                                         : shaders.gaussian_id_vertex)
                               : (shaders.gaussian_vertex.empty()
                                         ? shaders.vertex.parent_path() / "gaussian.vert.spv"
                                         : shaders.gaussian_vertex);
  const auto fragment_path = id_pass
                                 ? (shaders.gaussian_id_fragment.empty()
                                           ? shaders.fragment.parent_path() / "gaussian-id.frag.spv"
                                           : shaders.gaussian_id_fragment)
                                 : (shaders.gaussian_fragment.empty()
                                           ? shaders.fragment.parent_path() / "gaussian.frag.spv"
                                           : shaders.gaussian_fragment);
  ++frame_counters_.pipeline_creation_count;
  const auto vertex_shader = GetShaderModule(vertex_path);
  const auto fragment_shader = GetShaderModule(fragment_path);
  const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
          VK_SHADER_STAGE_VERTEX_BIT, vertex_shader, "main", nullptr},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
          VK_SHADER_STAGE_FRAGMENT_BIT, fragment_shader, "main", nullptr},
  }};
  const auto& layout = sorted_stream ? kSortedGaussianInstanceLayout
                                     : kCpuGaussianInstanceLayout;
  const std::array<VkVertexInputBindingDescription, 2> bindings{{
      {0, sizeof(Vec2), VK_VERTEX_INPUT_RATE_VERTEX},
      {1, layout.stride, VK_VERTEX_INPUT_RATE_INSTANCE},
  }};
  const std::array<VkVertexInputAttributeDescription, 9> attributes{{
      {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
      {1, 1, VK_FORMAT_R32G32_SFLOAT, layout.center_pixels},
      {2, 1, VK_FORMAT_R32G32B32_SFLOAT, layout.inverse_conic},
      {3, 1, VK_FORMAT_R32G32B32_SFLOAT, layout.radiance},
      {4, 1, VK_FORMAT_R32_SFLOAT, layout.opacity},
      {5, 1, VK_FORMAT_R32_SFLOAT, layout.radius_pixels},
      {6, 1, VK_FORMAT_R32_SFLOAT, layout.depth},
      {7, 1, VK_FORMAT_R32_UINT, layout.resource_id},
      {8, 1, VK_FORMAT_R32_UINT, layout.particle_id},
  }};
  VkPipelineVertexInputStateCreateInfo vertex_input{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex_input.vertexBindingDescriptionCount =
      static_cast<std::uint32_t>(bindings.size());
  vertex_input.pVertexBindingDescriptions = bindings.data();
  vertex_input.vertexAttributeDescriptionCount = id_pass
                                                     ? static_cast<std::uint32_t>(attributes.size())
                                                     : static_cast<std::uint32_t>(attributes.size() - 2U);
  vertex_input.pVertexAttributeDescriptions = attributes.data();
  VkPipelineInputAssemblyStateCreateInfo input_assembly{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport_state{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport_state.viewportCount = 1;
  viewport_state.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0F;
  VkPipelineMultisampleStateCreateInfo multisample{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depth{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = VK_TRUE;
  depth.depthWriteEnable = VK_FALSE;
  depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
  VkPipelineColorBlendAttachmentState attachment{};
  attachment.colorWriteMask = id_pass
                                  ? VK_COLOR_COMPONENT_R_BIT
                                  : VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  if (!id_pass) {
    attachment.blendEnable = VK_TRUE;
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.colorBlendOp = VK_BLEND_OP_ADD;
    attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.alphaBlendOp = VK_BLEND_OP_ADD;
  }
  const std::array blend_attachments{attachment, attachment};
  VkPipelineColorBlendStateCreateInfo blend{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = id_pass ? 2U : 1U;
  blend.pAttachments = blend_attachments.data();
  const std::array<VkDynamicState, 2> dynamic_states{
      VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount =
      static_cast<std::uint32_t>(dynamic_states.size());
  dynamic.pDynamicStates = dynamic_states.data();
  VkGraphicsPipelineCreateInfo pipeline_info{
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pipeline_info.stageCount = static_cast<std::uint32_t>(stages.size());
  pipeline_info.pStages = stages.data();
  pipeline_info.pVertexInputState = &vertex_input;
  pipeline_info.pInputAssemblyState = &input_assembly;
  pipeline_info.pViewportState = &viewport_state;
  pipeline_info.pRasterizationState = &raster;
  pipeline_info.pMultisampleState = &multisample;
  pipeline_info.pDepthStencilState = &depth;
  pipeline_info.pColorBlendState = &blend;
  pipeline_info.pDynamicState = &dynamic;
  pipeline_info.layout = active_target_->pipeline_layout;
  pipeline_info.renderPass = active_target_->render_pass;
  pipeline_info.subpass = id_pass ? 2U : 1U;
  Check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1,
            &pipeline_info, nullptr,
            &cached),
      "create Gaussian graphics pipeline");
  return cached;
}

VkPipeline Renderer::Impl::EnsureGeneratedPipeline(
    const ShaderPaths& shaders, const GeneratedMaterialArtifact& artifact,
    std::uint32_t variant_key) {
  const auto key = std::pair{artifact.module_key, variant_key};
  const auto found = active_target_->generated_pipelines.find(key);
  if (found != active_target_->generated_pipelines.end()) {
    ++frame_counters_.pipeline_cache_hits;
    return found->second;
  }
  ++frame_counters_.pipeline_cache_misses;
  const auto pipeline = CreatePipeline(shaders, variant_key, &artifact);
  active_target_->generated_pipelines.emplace(key, pipeline);
  return pipeline;
}

Renderer::Impl::DrawPipelineVariant Renderer::Impl::MakeDrawPipelineVariant(
    const extraction::DrawRecord& draw,
    const extraction::FrameSnapshot& snapshot) const {
  const auto& geometry = geometry_records_[draw.geometry_index];
  const auto& material = material_records_[draw.material_index];
  auto feature_mask = static_cast<std::uint32_t>(material.features);
  if (!geometry.has_colors) {
    feature_mask &=
        ~static_cast<std::uint32_t>(MaterialFeature::VertexColor);
  }
  if (!geometry.has_texcoords) {
    // Normal mapping without texture coordinates is disabled for the draw
    // and reported through render::ForwardNormalMapDisabled.
    feature_mask &=
        ~static_cast<std::uint32_t>(MaterialFeature::BaseColorTexture |
                                    MaterialFeature::NormalTexture |
                                    MaterialFeature::MetallicTexture |
                                    MaterialFeature::RoughnessTexture |
                                    MaterialFeature::EmissiveTexture);
  }
  auto variant_key = feature_mask;
  if (material.alpha_mode == AlphaMode::Masked) {
    variant_key |= kMaskedAlphaFlag;
  }
  if (material.double_sided || geometry.double_sided) {
    variant_key |= kDoubleSidedFlag;
  }
  // A mirrored instance reverses the camera winding so that its outward faces
  // stay in front; the variant therefore carries the effective winding.
  const auto& instance = instance_records_[draw.instance_index];
  if (render::ForwardDrawFrontFace(snapshot.front_face, instance.mirrored) ==
      FrontFaceWinding::CounterClockwise) {
    variant_key |= kCounterClockwiseFrontFaceFlag;
  }
  return {feature_mask, variant_key};
}

void Renderer::Impl::PreflightGeneratedMaterialPipelines(
    const extraction::FrameSnapshot& snapshot) {
  if (std::none_of(selected_material_artifacts_.begin(),
          selected_material_artifacts_.end(),
          [](const auto* artifact) { return artifact != nullptr; })) {
    return;
  }
  if (generated_preflight_draws_.table_identity() != snapshot.draws.table_identity() ||
      generated_preflight_geometries_.table_identity() != snapshot.geometries.table_identity() ||
      generated_preflight_materials_.table_identity() != snapshot.materials.table_identity() ||
      generated_preflight_front_face_ != snapshot.front_face ||
      generated_preflight_orientation_revision_ !=
          snapshot.instance_orientation_revision) {
    // Build transactionally. Artifact selection is reevaluated every frame;
    // this immutable plan only caches material membership and variant keys.
    std::set<std::pair<std::uint32_t, std::uint32_t>> variants;
    for (std::size_t i = 0; i < draw_records_.size(); ++i) {
      ++frame_counters_.mesh_cpu_draw_visit_count;
      const auto& draw = draw_records_[i];
      if (material_records_[draw.material_index].module) {
        variants.emplace(draw.material_index,
            MakeDrawPipelineVariant(draw, snapshot).variant_key);
      }
    }
    generated_preflight_variants_.assign(variants.begin(), variants.end());
    generated_preflight_draws_ = snapshot.draws;
    generated_preflight_geometries_ = snapshot.geometries;
    generated_preflight_materials_ = snapshot.materials;
    generated_preflight_front_face_ = snapshot.front_face;
    generated_preflight_orientation_revision_ =
        snapshot.instance_orientation_revision;
  }
  std::set<std::string> failed_modules;
  for (const auto& [material_index, variant_key] : generated_preflight_variants_) {
    const auto* artifact = selected_material_artifacts_[material_index];
    if (artifact == nullptr) {
      continue;
    }
    if (failed_modules.contains(artifact->module_key)) {
      RejectGeneratedMaterial(
          material_index, MaterialDiagnosticCategory::TargetFailure,
          "the registered Vulkan material artifact could not create a "
          "Forward pipeline");
      continue;
    }
    try {
      (void)EnsureGeneratedPipeline(active_target_->shaders, *artifact,
          variant_key);
    } catch (const RendererError& error) {
      if (error.code() == RendererErrorCode::DeviceLost ||
          error.code() == RendererErrorCode::ResourceExhausted ||
          error.code() == RendererErrorCode::Timeout) {
        throw;
      }
      failed_modules.insert(artifact->module_key);
      RejectGeneratedMaterial(
          material_index, MaterialDiagnosticCategory::TargetFailure,
          "the registered Vulkan material artifact could not create a "
          "Forward pipeline");
    }
  }
}

} // namespace merlin::vulkan
