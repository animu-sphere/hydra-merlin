#include "renderer_impl.hpp"

namespace merlin::vulkan {

render::ForwardDirectionalLighting Renderer::Impl::ExtractDirectionalLighting(
    const extraction::FrameSnapshot& snapshot) {
  try {
    return render::ExtractForwardDirectionalLighting(snapshot);
  } catch (const render::RendererError& error) {
    throw RendererError(RendererErrorCode::InvalidRequest,
        error.operation(), error.what());
  }
}

void Renderer::Impl::EnsureEnvironmentLighting(const std::filesystem::path& path) {
  if (path == environment_path_) {
    return;
  }
  auto replacement = detail::LoadDiffuseEnvironment(path);
  environment_lighting_ = std::move(replacement);
  environment_path_ = path;
}

MaterialUniforms Renderer::Impl::MakeMaterialUniforms(
    const extraction::MaterialRecord& material,
    const render::ForwardDirectionalLighting& lighting,
    const render::ForwardFramePolicy& frame_policy) const {
  auto uniforms = render::MakeForwardMaterialConstants<MaterialUniforms>(
      material, lighting, environment_lighting_.coefficients, frame_policy);
  // The bindless entry reads the normal texture by table slot from the
  // per-material block; the conventional entry binds it in the set instead.
  if (bindless_texture_table_ && material.normal_texture) {
    const auto texture_handle =
        texture_records_[material.normal_texture->texture_index].texture;
    const auto sampler_handle =
        sampler_records_[material.normal_texture->sampler_index].sampler;
    uniforms.normal_map_resource = {
        texture_slots_.at(texture_handle).bindless_slot.index,
        sampler_slots_.at(sampler_handle).bindless_slot.index, 0U, 0U};
  }
  return uniforms;
}

void Renderer::Impl::RejectGeneratedMaterial(std::size_t index,
    MaterialDiagnosticCategory category,
    std::string message) {
  const auto& module = *material_records_[index].module;
  selected_material_artifacts_[index] = nullptr;
  MaterialDiagnostic diagnostic;
  diagnostic.category = category;
  diagnostic.severity = DiagnosticSeverity::Error;
  diagnostic.fallback = MaterialFallback::BasicMaterial;
  diagnostic.message = std::move(message);
  diagnostic.context.material_identity = module.key;
  diagnostic.context.backend_target = "spirv";
  frame_counters_.material_fallbacks.Record(diagnostic.fallback);
  frame_material_diagnostics_.push_back(std::move(diagnostic));
}

std::optional<std::size_t> Renderer::Impl::PackedMaterialValueSize(
    MaterialValueType type) noexcept {
  switch (type) {
  case MaterialValueType::Float:
    return sizeof(float);
  case MaterialValueType::Float2:
    return sizeof(Vec2);
  case MaterialValueType::Float3:
    return sizeof(Vec3);
  case MaterialValueType::Float4:
    return sizeof(Vec4);
  case MaterialValueType::Integer:
    return sizeof(std::int32_t);
  case MaterialValueType::Boolean:
    return sizeof(std::uint32_t);
  default:
    return std::nullopt;
  }
}

bool Renderer::Impl::GeneratedBindingFits(
    const GeneratedMaterialParameterBinding& binding,
    std::uint32_t parameter_buffer_size) noexcept {
  const auto value_size = PackedMaterialValueSize(binding.type);
  if (!value_size || binding.array_size == 0U) {
    return false;
  }
  const auto buffer_size = static_cast<std::size_t>(parameter_buffer_size);
  const auto offset = static_cast<std::size_t>(binding.offset);
  if (offset > buffer_size || *value_size > buffer_size - offset) {
    return false;
  }
  if (binding.array_size == 1U) {
    return true;
  }
  const auto stride =
      binding.array_stride == 0U
          ? *value_size
          : static_cast<std::size_t>(binding.array_stride);
  if (stride < *value_size) {
    return false;
  }
  const auto remaining = buffer_size - offset - *value_size;
  return static_cast<std::size_t>(binding.array_size - 1U) <=
         remaining / stride;
}

void Renderer::Impl::SelectGeneratedMaterials() {
  selected_material_artifacts_.assign(material_records_.size(), nullptr);
  frame_material_diagnostics_.clear();
  for (std::size_t index = 0; index < material_records_.size(); ++index) {
    const auto& material = material_records_[index];
    if (!material.module || !mesh_material_is_drawn_[index]) {
      continue;
    }
    const auto& module = *material.module;
    if (bindless_texture_table_) {
      RejectGeneratedMaterial(
          index, MaterialDiagnosticCategory::TargetFailure,
          "generated materials currently require conventional Vulkan "
          "descriptors");
      continue;
    }
    const auto found = generated_material_artifacts_.find(module.key);
    if (found == generated_material_artifacts_.end()) {
      RejectGeneratedMaterial(
          index, MaterialDiagnosticCategory::CacheIncompatible,
          "no Vulkan artifact is registered for the material module");
      continue;
    }
    const auto& artifact = found->second;
    bool concrete_resource_layout_matches =
        artifact.resource_bindings.size() == module.resources.entries.size() &&
        artifact.material_constants_binding ==
            shader_abi::kConventionalMaterialConstants.binding;
    for (const auto& resource : module.resources.entries) {
      const auto binding = std::find_if(
          artifact.resource_bindings.begin(),
          artifact.resource_bindings.end(), [&](const auto& candidate) {
            return candidate.name == resource.name &&
                   candidate.type == resource.type &&
                   candidate.array_size == resource.array_size;
          });
      concrete_resource_layout_matches =
          concrete_resource_layout_matches &&
          binding != artifact.resource_bindings.end();
    }
    if (!concrete_resource_layout_matches ||
        !generated_descriptor_set_layouts_.contains(module.key)) {
      RejectGeneratedMaterial(
          index, MaterialDiagnosticCategory::ReflectionMismatch,
          "the Vulkan artifact's concrete resource layout disagrees "
          "with the material module");
      continue;
    }
    auto diagnostics = VerifyMaterialAbi(module);
    auto reflected =
        VerifyMaterialTargetReflection(module, artifact.reflection);
    diagnostics.insert(diagnostics.end(),
        std::make_move_iterator(reflected.begin()),
        std::make_move_iterator(reflected.end()));
    bool concrete_layout_matches =
        artifact.parameter_bindings.size() ==
        module.parameters.entries.size();
    for (const auto& parameter : module.parameters.entries) {
      const auto binding = std::find_if(
          artifact.parameter_bindings.begin(),
          artifact.parameter_bindings.end(), [&](const auto& candidate) {
            return candidate.name == parameter.name &&
                   candidate.type == parameter.type &&
                   candidate.array_size == parameter.array_size &&
                   GeneratedBindingFits(
                       candidate, artifact.parameter_buffer_size);
          });
      concrete_layout_matches =
          concrete_layout_matches &&
          binding != artifact.parameter_bindings.end();
    }
    if (!concrete_layout_matches) {
      RejectGeneratedMaterial(
          index, MaterialDiagnosticCategory::ReflectionMismatch,
          "the Vulkan artifact's concrete parameter layout disagrees "
          "with the material module");
      continue;
    }
    bool resources_available = true;
    const auto has_texture = [&](std::uint64_t handle) {
      for (std::size_t i = 0; i < texture_records_.size(); ++i) {
        if (texture_records_[i].texture == handle) {
          return true;
        }
      }
      return false;
    };
    const auto has_sampler = [&](std::uint64_t handle) {
      for (std::size_t i = 0; i < sampler_records_.size(); ++i) {
        if (sampler_records_[i].sampler == handle) {
          return true;
        }
      }
      return false;
    };
    for (const auto& entry : material.generated_resources.entries) {
      for (const auto& value : entry.values) {
        if (value.texture.valid()) {
          resources_available =
              resources_available && has_texture(value.texture.value());
        }
        if (value.sampler.valid()) {
          resources_available =
              resources_available && has_sampler(value.sampler.value());
        }
      }
    }
    if (!resources_available) {
      RejectGeneratedMaterial(
          index, MaterialDiagnosticCategory::MissingTexture,
          "a generated material texture or sampler is unavailable");
      continue;
    }
    if (!diagnostics.empty()) {
      for (auto& diagnostic : diagnostics) {
        frame_counters_.material_fallbacks.Record(diagnostic.fallback);
        frame_material_diagnostics_.push_back(std::move(diagnostic));
      }
      continue;
    }
    try {
      (void)GetShaderModule(artifact.fragment);
    } catch (const RendererError& error) {
      if (error.code() == RendererErrorCode::DeviceLost ||
          error.code() == RendererErrorCode::ResourceExhausted) {
        throw;
      }
      RejectGeneratedMaterial(
          index, MaterialDiagnosticCategory::CacheCorrupt,
          "the registered Vulkan material artifact could not be loaded as "
          "SPIR-V");
      continue;
    }
    selected_material_artifacts_[index] = &artifact;
  }
}

std::size_t Renderer::Impl::MaterialValueSize(MaterialValueType type) {
  if (const auto size = PackedMaterialValueSize(type)) {
    return *size;
  }
  throw RendererError(RendererErrorCode::Unsupported,
      "pack generated material parameters",
      "unsupported generated material parameter type");
}

void Renderer::Impl::CopyMaterialValue(std::byte* destination,
    const MaterialValue& value,
    MaterialValueType type) {
  const auto copy = [&](const auto* typed) {
    if (typed == nullptr) {
      throw RendererError(RendererErrorCode::InvalidRequest,
          "pack generated material parameters",
          "generated material parameter value has the "
          "wrong declared type");
    }
    std::memcpy(destination, typed, sizeof(*typed));
  };
  switch (type) {
  case MaterialValueType::Float:
    copy(std::get_if<float>(&value));
    break;
  case MaterialValueType::Float2:
    copy(std::get_if<Vec2>(&value));
    break;
  case MaterialValueType::Float3:
    copy(std::get_if<Vec3>(&value));
    break;
  case MaterialValueType::Float4:
    copy(std::get_if<Vec4>(&value));
    break;
  case MaterialValueType::Integer:
    copy(std::get_if<std::int32_t>(&value));
    break;
  case MaterialValueType::Boolean: {
    const auto* boolean = std::get_if<bool>(&value);
    if (boolean == nullptr) {
      throw RendererError(RendererErrorCode::InvalidRequest,
          "pack generated material parameters",
          "generated material boolean has the wrong "
          "declared type");
    }
    const std::uint32_t encoded = *boolean ? 1U : 0U;
    std::memcpy(destination, &encoded, sizeof(encoded));
    break;
  }
  default:
    (void)MaterialValueSize(type);
  }
}

void Renderer::Impl::PackGeneratedMaterialParameters(
    std::byte* destination, const extraction::MaterialRecord& material,
    const GeneratedMaterialArtifact& artifact) const {
  std::memset(destination, 0, artifact.parameter_buffer_size);
  for (const auto& binding : artifact.parameter_bindings) {
    const auto state = std::find_if(
        material.generated_parameters.entries.begin(),
        material.generated_parameters.entries.end(),
        [&](const auto& entry) { return entry.name == binding.name; });
    if (state == material.generated_parameters.entries.end() ||
        state->type != binding.type ||
        state->values.size() != binding.array_size) {
      throw RendererError(RendererErrorCode::InvalidRequest,
          "pack generated material parameters",
          "generated parameter state does not match the "
          "registered Vulkan artifact");
    }
    const auto value_size = MaterialValueSize(binding.type);
    const auto stride =
        binding.array_stride == 0U ? value_size : binding.array_stride;
    const auto end = static_cast<std::size_t>(binding.offset) +
                     stride * (binding.array_size - 1U) + value_size;
    if (end > artifact.parameter_buffer_size) {
      throw RendererError(RendererErrorCode::InvalidRequest,
          "pack generated material parameters",
          "generated parameter binding exceeds its buffer");
    }
    for (std::size_t value_index = 0;
        value_index < state->values.size(); ++value_index) {
      CopyMaterialValue(destination + binding.offset + stride * value_index,
          state->values[value_index], binding.type);
    }
  }
}

void Renderer::Impl::PrepareBindlessMaterialDescriptors(
    FrameContext& frame, const extraction::FrameSnapshot& snapshot,
    const render::ForwardFramePolicy& frame_policy) {
  frame.material_descriptor_sets.clear();
  if (material_records_.empty()) {
    return;
  }
  const auto lighting = ExtractDirectionalLighting(snapshot);
  const auto material_count =
      static_cast<std::uint32_t>(material_records_.size());
  const auto uniform_stride =
      AlignUp(sizeof(MaterialUniforms), uniform_buffer_alignment_);
  const auto uniform_bytes = uniform_stride * material_count;
  bool descriptor_dirty{};
  if (frame.material_uniforms.handle == VK_NULL_HANDLE ||
      frame.material_uniforms.size < uniform_bytes) {
    DestroyBuffer(frame.material_uniforms);
    frame.material_uniforms = CreateBuffer(
        uniform_bytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    descriptor_dirty = true;
  }
  if (frame.descriptor_pool == VK_NULL_HANDLE) {
    std::array<VkDescriptorPoolSize, 2> sizes{{
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4U},
    }};
    VkDescriptorPoolCreateInfo pool_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 1;
    // The shared bindless layout contains the optional GPU Scene bindings.
    // Vulkan descriptor pools must cover every binding in an allocated set
    // even when the conventional bindless shader does not statically use it.
    pool_info.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    pool_info.pPoolSizes = sizes.data();
    Check(vkCreateDescriptorPool(device_, &pool_info, nullptr,
              &frame.descriptor_pool),
        "create bindless material descriptor pool");
    ++frame_counters_.descriptor_pool_creation_count;
    VkDescriptorSetAllocateInfo allocate{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = frame.descriptor_pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &bindless_material_descriptor_set_layout_;
    Check(vkAllocateDescriptorSets(
              device_, &allocate,
              &frame.bindless_material_descriptor_set),
        "allocate bindless material descriptor set");
    ++frame_counters_.descriptor_allocation_count;
    descriptor_dirty = true;
  }
  if (descriptor_dirty) {
    std::array<VkDescriptorBufferInfo, 5> buffer_infos{};
    buffer_infos[0] = {
        frame.material_uniforms.handle, 0, sizeof(MaterialUniforms)};
    std::array<VkWriteDescriptorSet, 5> writes{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[0].dstSet = frame.bindless_material_descriptor_set;
    writes[0].dstBinding =
        shader_abi::kBindlessMaterialConstants.binding;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    writes[0].pBufferInfo = &buffer_infos[0];
    std::uint32_t write_count = 1;
    if (gpu_scene_buffers_.enabled()) {
      const std::array<const Buffer*, 4> table_buffers{
          &gpu_scene_buffers_.geometries,
          &gpu_scene_buffers_.instances,
          &gpu_scene_buffers_.materials,
          &gpu_scene_buffers_.draws,
      };
      const std::array table_binding_indices{
          shader_abi::kGpuSceneGeometries.binding,
          shader_abi::kGpuSceneInstances.binding,
          shader_abi::kGpuSceneMaterials.binding,
          shader_abi::kGpuSceneDraws.binding,
      };
      for (std::size_t i = 0; i < table_buffers.size(); ++i) {
        buffer_infos[i + 1] = {
            table_buffers[i]->handle, 0, table_buffers[i]->size};
        auto& write = writes[i + 1];
        write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = frame.bindless_material_descriptor_set;
        write.dstBinding = table_binding_indices[i];
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &buffer_infos[i + 1];
      }
      write_count = static_cast<std::uint32_t>(writes.size());
    }
    vkUpdateDescriptorSets(device_, write_count, writes.data(), 0, nullptr);
    frame_counters_.descriptor_update_count += write_count;
  }
  frame.material_uniform_stride = uniform_stride;

  void* mapped{};
  Check(vkMapMemory(device_, frame.material_uniforms.memory, 0,
            uniform_bytes, 0, &mapped),
      "map bindless material uniform buffer");
  for (std::uint32_t i = 0; i < material_count; ++i) {
    const auto& material = material_records_[i];
    const auto uniforms = MakeMaterialUniforms(material, lighting, frame_policy);
    std::memcpy(static_cast<std::byte*>(mapped) + uniform_stride * i,
        &uniforms, sizeof(uniforms));
  }
  vkUnmapMemory(device_, frame.material_uniforms.memory);
}

void Renderer::Impl::ValidateMaterialTextureBindings() const {
  for (std::size_t i = 0; i < material_records_.size(); ++i) {
    const auto& material = material_records_[i];
    for (const auto& binding :
        {material.base_color_texture, material.normal_texture}) {
      if (binding && (binding->texture_index >= texture_records_.size() ||
                         binding->sampler_index >= sampler_records_.size())) {
        throw RendererError(RendererErrorCode::InvalidRequest,
            "prepare material descriptors",
            "material texture binding index is invalid");
      }
    }
  }
}

void Renderer::Impl::PrepareMaterialDescriptors(
    FrameContext& frame, const extraction::FrameSnapshot& snapshot,
    const render::ForwardFramePolicy& frame_policy) {
  ValidateMaterialTextureBindings();
  if (bindless_texture_table_) {
    PrepareBindlessMaterialDescriptors(frame, snapshot, frame_policy);
    return;
  }
  const auto lighting = ExtractDirectionalLighting(snapshot);
  const auto& cached = frame.material_descriptor_source;
  const bool reuse_descriptors = cached &&
                                 cached->source_id == snapshot.source_id &&
                                 cached->resource_generation == material_resource_generation_ &&
                                 cached->materials.table_identity() == snapshot.materials.table_identity() &&
                                 cached->textures.table_identity() == snapshot.textures.table_identity() &&
                                 cached->samplers.table_identity() == snapshot.samplers.table_identity() &&
                                 cached->selected_artifacts == selected_material_artifacts_;
  if (!reuse_descriptors) {
    // The context has completed before preparation; invalidate before any
    // pool/buffer mutation so failed preparation cannot reuse old bindings.
    frame.material_descriptor_source.reset();
    frame.material_descriptor_sets.clear();
  }
  if (material_records_.empty()) {
    return;
  }
  EnsureFallbackTextureAndSampler();
  const auto material_count =
      static_cast<std::uint32_t>(material_records_.size());
  if (frame.descriptor_pool == VK_NULL_HANDLE ||
      frame.descriptor_capacity < material_count) {
    if (frame.descriptor_pool != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(device_, frame.descriptor_pool, nullptr);
      frame.descriptor_pool = VK_NULL_HANDLE;
      frame.descriptor_capacity = 0;
    }
    std::uint32_t maximum_sampled_images{};
    std::uint32_t maximum_samplers{};
    for (const auto& [key, artifact] : generated_material_artifacts_) {
      (void)key;
      std::uint32_t artifact_images{};
      std::uint32_t artifact_samplers{};
      for (const auto& resource : artifact.resource_bindings) {
        if (resource.type == MaterialValueType::Texture2D ||
            resource.type == MaterialValueType::CombinedTextureSampler) {
          artifact_images += resource.array_size;
        }
        if (resource.type == MaterialValueType::Sampler ||
            resource.type == MaterialValueType::CombinedTextureSampler) {
          artifact_samplers += resource.array_size;
        }
      }
      maximum_sampled_images =
          std::max(maximum_sampled_images, artifact_images);
      maximum_samplers = std::max(maximum_samplers, artifact_samplers);
    }
    std::vector<VkDescriptorPoolSize> sizes{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, material_count * 2U},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, material_count * 2U},
    };
    if (maximum_sampled_images != 0U) {
      sizes.push_back({VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          maximum_sampled_images * material_count});
    }
    if (maximum_samplers != 0U) {
      sizes.push_back({VK_DESCRIPTOR_TYPE_SAMPLER,
          maximum_samplers * material_count});
    }
    VkDescriptorPoolCreateInfo info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    info.maxSets = material_count;
    info.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    info.pPoolSizes = sizes.data();
    Check(vkCreateDescriptorPool(device_, &info, nullptr,
              &frame.descriptor_pool),
        "create material descriptor pool");
    ++frame_counters_.descriptor_pool_creation_count;
    frame.descriptor_capacity = material_count;
  } else if (!reuse_descriptors) {
    Check(vkResetDescriptorPool(device_, frame.descriptor_pool, 0),
        "reset material descriptor pool");
  }

  const auto uniform_stride =
      AlignUp(sizeof(MaterialUniforms), uniform_buffer_alignment_);
  const auto uniform_bytes = uniform_stride * material_count;
  if (frame.material_uniforms.handle == VK_NULL_HANDLE ||
      frame.material_uniforms.size < uniform_bytes) {
    DestroyBuffer(frame.material_uniforms);
    frame.material_uniforms = CreateBuffer(
        uniform_bytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  }
  void* mapped{};
  Check(vkMapMemory(device_, frame.material_uniforms.memory, 0,
            uniform_bytes, 0, &mapped),
      "map material uniform buffer");
  for (std::uint32_t i = 0; i < material_count; ++i) {
    const auto& material = material_records_[i];
    const auto uniforms = MakeMaterialUniforms(material, lighting, frame_policy);
    std::memcpy(static_cast<std::byte*>(mapped) + uniform_stride * i,
        &uniforms, sizeof(uniforms));
  }
  vkUnmapMemory(device_, frame.material_uniforms.memory);

  frame.generated_parameter_offsets.assign(material_count, 0);
  VkDeviceSize generated_uniform_bytes{};
  for (std::uint32_t i = 0; i < material_count; ++i) {
    if (const auto* artifact = selected_material_artifacts_[i]) {
      generated_uniform_bytes =
          AlignUp(generated_uniform_bytes, uniform_buffer_alignment_);
      frame.generated_parameter_offsets[i] = generated_uniform_bytes;
      generated_uniform_bytes += artifact->parameter_buffer_size;
    }
  }
  if (generated_uniform_bytes != 0U) {
    if (frame.generated_parameter_uniforms.handle == VK_NULL_HANDLE ||
        frame.generated_parameter_uniforms.size <
            generated_uniform_bytes) {
      DestroyBuffer(frame.generated_parameter_uniforms);
      frame.generated_parameter_uniforms = CreateBuffer(
          generated_uniform_bytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    void* generated_mapped{};
    Check(vkMapMemory(device_, frame.generated_parameter_uniforms.memory, 0,
              generated_uniform_bytes, 0, &generated_mapped),
        "map generated material uniform buffer");
    for (std::uint32_t i = 0; i < material_count; ++i) {
      if (const auto* artifact = selected_material_artifacts_[i]) {
        PackGeneratedMaterialParameters(
            static_cast<std::byte*>(generated_mapped) +
                frame.generated_parameter_offsets[i],
            material_records_[i], *artifact);
      }
    }
    vkUnmapMemory(device_, frame.generated_parameter_uniforms.memory);
  }

  // Lighting and parameter uniforms above still refresh on every frame.
  // Stable resource/layout roots keep native descriptor sets intact.
  if (reuse_descriptors) {
    return;
  }
  std::vector<VkDescriptorSetLayout> layouts(material_count);
  for (std::uint32_t i = 0; i < material_count; ++i) {
    const auto* artifact = selected_material_artifacts_[i];
    layouts[i] =
        artifact == nullptr
            ? descriptor_set_layout_
            : generated_descriptor_set_layouts_.at(artifact->module_key);
  }
  frame.material_descriptor_sets.resize(material_count);
  VkDescriptorSetAllocateInfo allocate{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorPool = frame.descriptor_pool;
  allocate.descriptorSetCount = material_count;
  allocate.pSetLayouts = layouts.data();
  Check(vkAllocateDescriptorSets(device_, &allocate,
            frame.material_descriptor_sets.data()),
      "allocate material descriptor sets");
  frame_counters_.descriptor_allocation_count += material_count;

  // Conventional sets hold the base-color and normal combined samplers.
  // Writes point into image_infos, so its reservation must never grow.
  std::size_t maximum_image_infos = material_count * 2U;
  for (const auto* artifact : selected_material_artifacts_) {
    if (artifact == nullptr) {
      continue;
    }
    for (const auto& resource : artifact->resource_bindings) {
      if (resource.type == MaterialValueType::Texture2D ||
          resource.type == MaterialValueType::CombinedTextureSampler) {
        maximum_image_infos += resource.array_size;
      }
      if (resource.type == MaterialValueType::Sampler ||
          resource.type == MaterialValueType::CombinedTextureSampler) {
        maximum_image_infos += resource.array_size;
      }
    }
  }
  std::vector<VkDescriptorImageInfo> image_infos;
  image_infos.reserve(maximum_image_infos);
  std::vector<VkDescriptorBufferInfo> buffer_infos(material_count);
  std::vector<VkDescriptorBufferInfo> generated_buffer_infos(material_count);
  std::vector<VkWriteDescriptorSet> writes;
  writes.reserve(material_count * 3U + maximum_image_infos);
  for (std::uint32_t i = 0; i < material_count; ++i) {
    buffer_infos[i] = {frame.material_uniforms.handle, uniform_stride * i,
        sizeof(MaterialUniforms)};
    if (const auto* artifact = selected_material_artifacts_[i]) {
      generated_buffer_infos[i] = {
          frame.generated_parameter_uniforms.handle,
          frame.generated_parameter_offsets[i],
          artifact->parameter_buffer_size};
      VkWriteDescriptorSet parameter_write{
          VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      parameter_write.dstSet = frame.material_descriptor_sets[i];
      parameter_write.dstBinding = artifact->parameter_binding;
      parameter_write.descriptorCount = 1;
      parameter_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      parameter_write.pBufferInfo = &generated_buffer_infos[i];
      writes.push_back(parameter_write);

      for (const auto& resource : artifact->resource_bindings) {
        const auto state = std::find_if(
            material_records_[i].generated_resources.entries.begin(),
            material_records_[i].generated_resources.entries.end(),
            [&](const auto& entry) { return entry.name == resource.name; });
        if (state ==
                material_records_[i].generated_resources.entries.end() ||
            state->type != resource.type ||
            state->values.size() != resource.array_size) {
          throw RendererError(
              RendererErrorCode::InvalidRequest,
              "prepare generated material descriptors",
              "generated resource state does not match the Vulkan artifact");
        }
        const auto write_resources =
            [&](VkDescriptorType descriptor_type, std::uint32_t binding) {
              const auto first_info = image_infos.size();
              for (const auto& value : state->values) {
                VkDescriptorImageInfo info{};
                if (descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
                  info.imageView =
                      texture_slots_.at(value.texture.value()).view;
                  info.imageLayout =
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                } else {
                  info.sampler =
                      sampler_slots_.at(value.sampler.value()).sampler;
                }
                image_infos.push_back(info);
              }
              VkWriteDescriptorSet write{
                  VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
              write.dstSet = frame.material_descriptor_sets[i];
              write.dstBinding = binding;
              write.descriptorCount = resource.array_size;
              write.descriptorType = descriptor_type;
              write.pImageInfo = image_infos.data() + first_info;
              writes.push_back(write);
            };
        if (resource.type == MaterialValueType::Texture2D ||
            resource.type == MaterialValueType::CombinedTextureSampler) {
          write_resources(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
              resource.texture_binding);
        }
        if (resource.type == MaterialValueType::Sampler ||
            resource.type == MaterialValueType::CombinedTextureSampler) {
          write_resources(VK_DESCRIPTOR_TYPE_SAMPLER,
              resource.sampler_binding);
        }
      }

      VkWriteDescriptorSet material_write{
          VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      material_write.dstSet = frame.material_descriptor_sets[i];
      material_write.dstBinding = artifact->material_constants_binding;
      material_write.descriptorCount = 1;
      material_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      material_write.pBufferInfo = &buffer_infos[i];
      writes.push_back(material_write);
      continue;
    }
    const auto& material = material_records_[i];
    const auto write_texture =
        [&](const std::optional<extraction::TextureBindingRecord>& binding,
            std::uint32_t destination) {
          auto image_view = FallbackTextureView();
          auto sampler = fallback_sampler_.sampler;
          if (binding) {
            if (binding->texture_index >= texture_records_.size() ||
                binding->sampler_index >= sampler_records_.size()) {
              throw RendererError(RendererErrorCode::InvalidRequest,
                  "prepare material descriptors",
                  "material texture binding index is invalid");
            }
            const auto texture_handle =
                texture_records_[binding->texture_index].texture;
            const auto sampler_handle =
                sampler_records_[binding->sampler_index].sampler;
            image_view = texture_slots_.at(texture_handle).view;
            sampler = sampler_slots_.at(sampler_handle).sampler;
          }
          image_infos.push_back(
              {sampler, image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
          VkWriteDescriptorSet image_write{
              VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          image_write.dstSet = frame.material_descriptor_sets[i];
          image_write.dstBinding = destination;
          image_write.descriptorCount = 1;
          image_write.descriptorType =
              VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
          image_write.pImageInfo = &image_infos.back();
          writes.push_back(image_write);
        };
    write_texture(material.base_color_texture,
        shader_abi::kConventionalBaseColorTexture.binding);
    write_texture(material.normal_texture,
        shader_abi::kConventionalNormalTexture.binding);
    VkWriteDescriptorSet buffer_write{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    buffer_write.dstSet = frame.material_descriptor_sets[i];
    buffer_write.dstBinding = shader_abi::kConventionalMaterialConstants.binding;
    buffer_write.descriptorCount = 1;
    buffer_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    buffer_write.pBufferInfo = &buffer_infos[i];
    writes.push_back(buffer_write);
  }
  vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
      writes.data(), 0, nullptr);
  frame_counters_.descriptor_update_count += writes.size();
  frame.material_descriptor_source = std::make_shared<const MaterialDescriptorSource>(
      MaterialDescriptorSource{snapshot.source_id, material_resource_generation_,
          snapshot.materials, snapshot.textures, snapshot.samplers,
          selected_material_artifacts_});
}

void Renderer::Impl::RecordFrame(VkCommandBuffer command,
    const FrameContext& frame,
    const extraction::FrameSnapshot& snapshot,
    const Vec4& clear_color,
    const std::vector<Aov>& cpu_readback_aovs) {
  std::array<VkClearValue, 4> clear{};
  clear[0].color = {
      {std::clamp(clear_color.x, 0.0F, 1.0F),
          std::clamp(clear_color.y, 0.0F, 1.0F),
          std::clamp(clear_color.z, 0.0F, 1.0F),
          std::clamp(clear_color.w, 0.0F, 1.0F)}};
  clear[1].depthStencil = {1.0F, 0};
  clear[2].color.uint32[0] = std::numeric_limits<std::uint32_t>::max();
  clear[3].color.uint32[0] = std::numeric_limits<std::uint32_t>::max();
  VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  pass.renderPass = active_target_->render_pass;
  pass.framebuffer = active_target_->framebuffer;
  pass.renderArea = {{0, 0}, {active_target_->width, active_target_->height}};
  pass.clearValueCount = static_cast<std::uint32_t>(clear.size());
  pass.pClearValues = clear.data();
  vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
  const VkViewport viewport{0.0F, 0.0F, static_cast<float>(active_target_->width),
      static_cast<float>(active_target_->height), 0.0F, 1.0F};
  const VkRect2D scissor{{0, 0}, {active_target_->width, active_target_->height}};
  vkCmdSetViewport(command, 0, 1, &viewport);
  vkCmdSetScissor(command, 0, 1, &scissor);
  const auto view_projection = render::MultiplyForwardMatrices(snapshot.projection, snapshot.view);
  const auto gpu_scene_draw_slots =
      gpu_scene_buffers_.pending_update
          ? gpu_scene_buffers_.pending_draw_slot_indices
          : (gpu_scene_buffers_.has_resident_update &&
                        gpu_scene_buffers_.source_id == snapshot.source_id &&
                        gpu_scene_buffers_.revision == snapshot.revision
                    ? gpu_scene_buffers_.draw_slot_indices
                    : nullptr);
  const bool gpu_scene_ready =
      bindless_texture_table_ && gpu_scene_draw_slots &&
      gpu_scene_draw_slots->size() == draw_records_.size();
  if (frame.gpu_driven.selected) {
    const std::array descriptor_sets{
        bindless_descriptor_set_, frame.bindless_material_descriptor_set};
    const std::uint32_t dynamic_offset{};
    vkCmdBindDescriptorSets(
        command, VK_PIPELINE_BIND_POINT_GRAPHICS,
        active_target_->pipeline_layout, 0,
        static_cast<std::uint32_t>(descriptor_sets.size()),
        descriptor_sets.data(), 1, &dynamic_offset);
    const shader_abi::GpuDrivenForwardConstants push{view_projection};
    vkCmdPushConstants(command, active_target_->pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT |
            VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(push), &push);
    for (const auto& batch : frame.gpu_driven.batches) {
      const auto vertex_buffer = vertex_arena_.buffer(batch.vertex_block);
      constexpr VkDeviceSize zero_offset{};
      vkCmdBindVertexBuffers(command, 0, 1, &vertex_buffer, &zero_offset);
      vkCmdBindIndexBuffer(command, index_arena_.buffer(batch.index_block), 0,
          VK_INDEX_TYPE_UINT32);
      const auto pipeline = EnsureGpuDrivenGraphicsPipeline(
          active_target_->shaders, batch.pipeline_variant);
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      vkCmdDrawIndexedIndirectCount(
          command, frame.gpu_driven.indirect_commands.handle,
          batch.command_offset,
          frame.gpu_driven.dispatch_counters.handle,
          batch.counter_offset +
              offsetof(shader_abi::GpuDrivenIndexedDispatchCounters,
                  visible_count),
          batch.candidate_count,
          sizeof(render::GpuIndexedIndirectCommand));
      ++frame_counters_.gpu_driven_indirect_draw_count;
    }
  } else {
    for (std::size_t i = 0; i < draw_records_.size(); ++i) {
      const auto& draw = draw_records_[i];
      const auto& geometry = geometry_records_[draw.geometry_index];
      const auto& slot = geometry_slots_.at(geometry.mesh);
      const auto vertex_buffer = vertex_arena_.buffer(slot.vertices.block);
      vkCmdBindVertexBuffers(command, 0, 1, &vertex_buffer,
          &slot.vertices.offset);
      vkCmdBindIndexBuffer(command, index_arena_.buffer(slot.indices.block),
          slot.indices.offset, VK_INDEX_TYPE_UINT32);
      const auto& instance = instance_records_[draw.instance_index];
      const auto& material = material_records_[draw.material_index];
      const auto variant = MakeDrawPipelineVariant(draw, snapshot);
      auto feature_mask = variant.feature_mask;
      const auto* generated_artifact =
          selected_material_artifacts_[draw.material_index];
      const bool use_gpu_scene =
          gpu_scene_ready && generated_artifact == nullptr;
      if (generated_artifact != nullptr) {
        ++frame_counters_.generated_material_draw_count;
      } else if (material.module) {
        ++frame_counters_.generated_material_fallback_count;
      }
      const auto pipeline =
          generated_artifact != nullptr
              ? EnsureGeneratedPipeline(active_target_->shaders,
                    *generated_artifact,
                    variant.variant_key)
              : EnsurePipeline(active_target_->shaders,
                    variant.variant_key |
                        (use_gpu_scene
                                ? kGpuScenePipelineFlag
                                : 0U));
      const auto pipeline_layout =
          generated_artifact != nullptr
              ? active_target_->generated_pipeline_layouts.at(
                    generated_artifact->module_key)
              : active_target_->pipeline_layout;
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      if (bindless_texture_table_) {
        const auto dynamic_offset_value =
            frame.material_uniform_stride * draw.material_index;
        if (dynamic_offset_value >
            std::numeric_limits<std::uint32_t>::max()) {
          throw RendererError(RendererErrorCode::Unsupported,
              "record bindless material",
              "dynamic material offset exceeds uint32 range");
        }
        const auto dynamic_offset =
            static_cast<std::uint32_t>(dynamic_offset_value);
        std::array<VkDescriptorSet, 2> descriptor_sets{};
        descriptor_sets[shader_abi::kBindlessTextures.set] =
            bindless_descriptor_set_;
        descriptor_sets[shader_abi::kBindlessMaterialConstants.set] =
            frame.bindless_material_descriptor_set;
        vkCmdBindDescriptorSets(
            command, VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipeline_layout, 0,
            static_cast<std::uint32_t>(descriptor_sets.size()),
            descriptor_sets.data(), 1, &dynamic_offset);
      } else {
        const auto descriptor_set =
            frame.material_descriptor_sets[draw.material_index];
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipeline_layout, 0, 1,
            &descriptor_set, 0, nullptr);
      }
      if (use_gpu_scene) {
        shader_abi::GpuSceneDrawConstants push;
        push.view_projection = view_projection;
        push.draw_slot = (*gpu_scene_draw_slots)[i];
        vkCmdPushConstants(command, pipeline_layout,
            VK_SHADER_STAGE_VERTEX_BIT |
                VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(push), &push);
        ++frame_counters_.gpu_scene_draw_count;
      } else {
        PushConstants push;
        push.model_view_projection =
            render::MultiplyForwardMatrices(view_projection, instance.transform);
        const auto normal_matrix = render::ForwardNormalMatrix(instance.transform);
        push.normal_matrix_column0 = normal_matrix[0];
        push.normal_matrix_column1 = normal_matrix[1];
        push.normal_matrix_column2 = normal_matrix[2];
        push.feature_mask = feature_mask;
        if (material.alpha_mode == AlphaMode::Masked) {
          push.feature_mask |= kMaskedAlphaFlag;
        }
        if (bindless_texture_table_ && material.base_color_texture) {
          const auto texture_handle =
              texture_records_[material.base_color_texture->texture_index]
                  .texture;
          const auto sampler_handle =
              sampler_records_[material.base_color_texture->sampler_index]
                  .sampler;
          const auto texture_slot =
              texture_slots_.at(texture_handle).bindless_slot;
          const auto sampler_slot =
              sampler_slots_.at(sampler_handle).bindless_slot;
          if (sampler_slot.index > kSamplerIndexMask) {
            throw RendererError(
                RendererErrorCode::Unsupported,
                "record bindless material",
                "bindless sampler index exceeds shader encoding");
          }
          push.texture_index = texture_slot.index;
          push.feature_mask |= sampler_slot.index << kSamplerIndexShift;
        }
        push.prim_id = static_cast<std::uint32_t>(geometry.mesh);
        push.instance_id = static_cast<std::uint32_t>(instance.instance);
        vkCmdPushConstants(command, pipeline_layout,
            VK_SHADER_STAGE_VERTEX_BIT |
                VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(push), &push);
      }
      vkCmdDrawIndexed(command, slot.index_count, 1, 0, 0, 0);
    }
  }
  vkCmdNextSubpass(command, VK_SUBPASS_CONTENTS_INLINE);
  if (frame.timestamp_pool != VK_NULL_HANDLE) {
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
        frame.timestamp_pool, 2);
  }
  // The color and ID subpasses consume one stream in one order: either the
  // CPU-sorted upload or the GPU-sorted records with a device-written
  // instance count. When tile raster replaces the draws on the device, that
  // count is zero and compute composites the tiles after the render pass.
  const auto& sorted_raster = frame.gaussian_gpu_raster;
  const auto record_gaussian_draw = [&](bool id_pass) {
    if (!sorted_raster.selected && frame.gaussian_instance_count == 0) {
      return;
    }
    const auto pipeline = EnsureGaussianPipeline(active_target_->shaders,
        id_pass, sorted_raster.selected);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const std::array<VkBuffer, 2> vertex_buffers{
        gaussian_corner_vertices_.handle,
        sorted_raster.selected ? sorted_raster.records.handle
                               : frame.gaussian_instances.handle};
    const std::array<VkDeviceSize, 2> offsets{};
    vkCmdBindVertexBuffers(command, 0,
        static_cast<std::uint32_t>(vertex_buffers.size()),
        vertex_buffers.data(), offsets.data());
    const GaussianPushConstants push{{
        1.0F / static_cast<float>(active_target_->width),
        1.0F / static_cast<float>(active_target_->height),
    }};
    vkCmdPushConstants(command, active_target_->pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT |
            VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(push), &push);
    if (sorted_raster.selected) {
      vkCmdDrawIndirect(command, sorted_raster.draw_arguments.handle, 0, 1,
          sizeof(VkDrawIndirectCommand));
      ++frame_counters_.gaussian_gpu_raster_indirect_draw_count;
    } else {
      vkCmdDraw(command, shader_abi::kGaussianRasterVertexCount,
          frame.gaussian_instance_count, 0, 0);
    }
    ++frame_counters_.gaussian_draw_count;
  };
  record_gaussian_draw(false);
  vkCmdNextSubpass(command, VK_SUBPASS_CONTENTS_INLINE);
  record_gaussian_draw(true);
  const bool tile_raster = frame.gaussian_gpu_tile_raster.selected;
  if (frame.timestamp_pool != VK_NULL_HANDLE && !tile_raster &&
      !active_target_->gaussian_float_color) {
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
        frame.timestamp_pool, 3);
  }
  vkCmdEndRenderPass(command);
  if (tile_raster) {
    RecordGaussianGpuTileRaster(command, frame);
    if (frame.timestamp_pool != VK_NULL_HANDLE &&
        !active_target_->gaussian_float_color) {
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
          frame.timestamp_pool, 3);
    }
  }

  if (active_target_->gaussian_float_color) {
    RecordGaussianColorConversion(command);
    if (frame.timestamp_pool != VK_NULL_HANDLE) {
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
          frame.timestamp_pool, 3);
    }
  }

  if (frame.gpu_driven.selected) {
    for (const auto& batch : frame.gpu_driven.batches) {
      const VkBufferCopy batch_copy{
          batch.counter_offset, batch.counter_offset,
          sizeof(shader_abi::GpuDrivenIndexedDispatchCounters)};
      vkCmdCopyBuffer(command,
          frame.gpu_driven.dispatch_counters.handle,
          frame.gpu_driven.counter_readback.handle, 1,
          &batch_copy);
    }
  }

  if (!cpu_readback_aovs.empty()) {
    // Completion protects CPU ownership, but does not create a device
    // memory dependency between successive writes to retained staging.
    VkMemoryBarrier readback_reuse{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    readback_reuse.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
    readback_reuse.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &readback_reuse, 0, nullptr, 0, nullptr);
  }
  if (HasAov(cpu_readback_aovs, Aov::Color)) {
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {active_target_->width, active_target_->height, 1};
    vkCmdCopyImageToBuffer(command, active_target_->color,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        active_target_->color_readback.handle, 1, &copy);
  }
  if (HasAov(cpu_readback_aovs, Aov::Depth)) {
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {active_target_->width, active_target_->height, 1};
    vkCmdCopyImageToBuffer(command, active_target_->depth,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        active_target_->depth_readback.handle, 1, &copy);
  }
  VkBufferImageCopy id_copy{};
  id_copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  id_copy.imageSubresource.layerCount = 1;
  id_copy.imageExtent = {active_target_->width, active_target_->height, 1};
  if (HasAov(cpu_readback_aovs, Aov::PrimId)) {
    vkCmdCopyImageToBuffer(command, active_target_->prim_id,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        active_target_->prim_id_readback.handle, 1,
        &id_copy);
  }
  if (HasAov(cpu_readback_aovs, Aov::InstanceId)) {
    vkCmdCopyImageToBuffer(command, active_target_->instance_id,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        active_target_->instance_id_readback.handle, 1,
        &id_copy);
  }
  if (frame.present_pending) {
    RecordPresentation(command, frame);
  }
}

} // namespace merlin::vulkan
