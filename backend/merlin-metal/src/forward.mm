#include "backend_impl.hpp"

#include <merlin/render/surface_geometry.hpp>

namespace merlin::metal {

MaterialConstants
Backend::Impl::MakeMaterial(const extraction::MaterialRecord& material,
    const render::ForwardLighting& lighting,
    const render::ForwardFramePolicy& frame_policy) {
  return render::MakeForwardMaterialConstants<MaterialConstants>(material, lighting, frame_policy);
}

void Backend::Impl::EncodeRender(id<MTLCommandBuffer> command, FrameContext& frame,
    const render::RenderRequest& request, FrameBuild& build) {
  MTLRenderPassDescriptor* pass =
      [MTLRenderPassDescriptor renderPassDescriptor];
  const bool float_color = !request.snapshot->gaussians.empty() ||
                           request.gpu_driven_gaussian.mode != render::GpuDrivenGaussianMode::Disabled;
  pass.colorAttachments[0].texture = float_color ? frame.gaussian_color : frame.color;
  build.telemetry.gaussian_float_color_frame_count = float_color ? 1 : 0;
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

  // Metal maps NDC +Y up in framebuffer coordinates.
  const render::ForwardFramePolicy frame_policy{
      render::PackForwardOutputTransform(request.output_transform),
      render::PackForwardSurfacePolicy(request.snapshot->front_face,
          render::NdcYDirection::Up, request.debug_view, request.mesh_display.mode)};
  const auto lighting = render::PrepareForwardLighting(*request.snapshot,
      render::PackagedDiffuseEnvironment().coefficients, request.forward_lighting);
  const auto specular_environment = lighting.specular_texture.valid()
      ? textures_.at(lighting.specular_texture.value()).texture
      : disabled_specular_environment_;
  id<MTLRenderCommandEncoder> encoder =
      [command renderCommandEncoderWithDescriptor:pass];
  if (encoder == nil) {
    throw render::RendererError(render::RendererErrorCode::BackendFailure,
        "encode Metal render pass",
        "renderCommandEncoder returned nil");
  }
  const MTLViewport viewport{0.0,
      0.0,
      static_cast<double>(request.width),
      static_cast<double>(request.height),
      0.0,
      1.0};
  const MTLScissorRect scissor{0, 0, request.width, request.height};
  [encoder setViewport:viewport];
  [encoder setScissorRect:scissor];
  const auto& dome = request.forward_lighting.dome;
  if (dome && dome->camera_visible) {
    const auto texture = textures_.find(dome->rgbe_texture.value());
    if (texture == textures_.end()) {
      [encoder endEncoding];
      throw render::RendererError(render::RendererErrorCode::InvalidRequest,
          "encode Metal dome background", "dome texture is absent from the snapshot");
    }
    const auto constants = render::PackDomeDrawConstants(*dome);
    [encoder setRenderPipelineState:float_color ? float_dome_pipeline_ : dome_pipeline_];
    [encoder setFragmentBytes:&constants length:sizeof(constants) atIndex:0];
    [encoder setFragmentTexture:texture->second.texture atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  }
  [encoder setRenderPipelineState:float_color ? float_pipeline_ : pipeline_];
  [encoder setFragmentTexture:specular_environment atIndex:8];
  [encoder setDepthStencilState:depth_state_];
  const bool offset_surfaces = render::OffsetMeshSurfaces(request.mesh_display.mode);
  if (offset_surfaces) {
    [encoder setDepthBias:1.0F slopeScale:1.0F clamp:0.0F];
  }
  if (bindless_) {
    [encoder setFragmentBuffer:frame.argument_buffer offset:0 atIndex:3];
    for (const auto& [handle, texture] : textures_) {
      (void)handle;
      [encoder useResource:texture.texture
                     usage:MTLResourceUsageRead
                    stages:MTLRenderStageFragment];
    }
  }

  const auto view_projection =
      render::MultiplyForwardMatrices(request.snapshot->projection, request.snapshot->view);
  const auto gpu_scene_draw_slots =
      build.has_gpu_scene_update
          ? build.gpu_scene_draw_slot_indices
          : (gpu_scene_buffers_.has_resident_update &&
                        gpu_scene_buffers_.source_id ==
                            request.snapshot->source_id &&
                        gpu_scene_buffers_.revision == request.snapshot->revision
                    ? gpu_scene_buffers_.draw_slot_indices
                    : nullptr);
  const bool gpu_scene_ready =
      bindless_ && gpu_scene_pipeline_ != nil && gpu_scene_draw_slots &&
      gpu_scene_draw_slots->size() == request.snapshot->draws.size();
  if (gpu_scene_ready) {
    [encoder useResource:gpu_scene_buffers_.geometries
                   usage:MTLResourceUsageRead
                  stages:MTLRenderStageVertex | MTLRenderStageFragment];
    [encoder useResource:gpu_scene_buffers_.instances
                   usage:MTLResourceUsageRead
                  stages:MTLRenderStageVertex | MTLRenderStageFragment];
    [encoder useResource:gpu_scene_buffers_.materials
                   usage:MTLResourceUsageRead
                  stages:MTLRenderStageVertex | MTLRenderStageFragment];
    [encoder useResource:gpu_scene_buffers_.draws
                   usage:MTLResourceUsageRead
                  stages:MTLRenderStageVertex | MTLRenderStageFragment];
  }
  const auto surface_draw_count = render::DrawMeshSurfaces(request.mesh_display.mode)
      ? request.snapshot->draws.size() : 0;
  for (std::size_t draw_index = 0; draw_index < surface_draw_count; ++draw_index) {
    const auto& draw = request.snapshot->draws[draw_index];
    if (draw.geometry_index >= request.snapshot->geometries.size() ||
        draw.material_index >= request.snapshot->materials.size() ||
        draw.instance_index >= request.snapshot->instances.size()) {
      [encoder endEncoding];
      throw render::RendererError(render::RendererErrorCode::InvalidRequest,
          "encode Metal draw",
          "draw record index is invalid");
    }
    const auto& geometry_record =
        request.snapshot->geometries[draw.geometry_index];
    const auto geometry = geometries_.find(geometry_record.mesh);
    if (geometry == geometries_.end()) {
      continue;
    }
    const auto& instance = request.snapshot->instances[draw.instance_index];
    if (!instance.visible) {
      continue;
    }
    const auto& material = request.snapshot->materials[draw.material_index];
    // Metal evaluates winding with NDC +Y up, opposite to the logical
    // framebuffer convention, so the native winding is reversed. A mirrored
    // instance reverses it again to keep its outward faces in front.
    [encoder setFrontFacingWinding:render::ForwardDrawFrontFace(
                                       request.snapshot->front_face,
                                       instance.mirrored) ==
                                           FrontFaceWinding::Clockwise
                                       ? MTLWindingCounterClockwise
                                       : MTLWindingClockwise];
    [encoder setCullMode:material.double_sided || geometry_record.double_sided
                             ? MTLCullModeNone
                             : MTLCullModeBack];

    DrawConstants constants;
    constants.model_view_projection =
        render::MultiplyForwardMatrices(view_projection, instance.transform);
    const auto normal = render::ForwardNormalMatrix(instance.transform);
    constants.normal_matrix_column0 = normal[0];
    constants.normal_matrix_column1 = normal[1];
    constants.normal_matrix_column2 = normal[2];
    constants.feature_mask = static_cast<std::uint32_t>(material.features);
    if (geometry_record.has_colors) {
      constants.feature_mask |= render::kForwardGeometryColorFlag;
    }
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

    // Resolves one material texture binding to its native objects and its
    // argument-table slots. A binding whose resources are not resident drops
    // `feature` and records the basic-material fallback.
    struct ResolvedTexture {
      id<MTLTexture> texture = nil;
      id<MTLSamplerState> sampler = nil;
      std::uint32_t texture_index{};
      std::uint32_t sampler_index{};
    };
    const auto resolve_texture =
        [&](const std::optional<extraction::TextureBindingRecord>& binding,
            MaterialFeature feature) {
          ResolvedTexture resolved;
          if (!binding) {
            return resolved;
          }
          if (binding->texture_index >= request.snapshot->textures.size() ||
              binding->sampler_index >= request.snapshot->samplers.size()) {
            [encoder endEncoding];
            throw render::RendererError(
                render::RendererErrorCode::InvalidRequest,
                "encode Metal material",
                "material texture binding index is invalid");
          }
          const auto texture_handle =
              request.snapshot->textures[binding->texture_index].texture;
          const auto sampler_handle =
              request.snapshot->samplers[binding->sampler_index].sampler;
          const auto texture_found = textures_.find(texture_handle);
          const auto sampler_found = samplers_.find(sampler_handle);
          if (texture_found == textures_.end() ||
              sampler_found == samplers_.end()) {
            constants.feature_mask &= ~static_cast<std::uint32_t>(feature);
            build.telemetry.material_fallbacks.Record(
                MaterialFallback::BasicMaterial);
            return resolved;
          }
          resolved.texture = texture_found->second.texture;
          resolved.sampler = sampler_found->second.sampler;
          const auto texture_slot = texture_slots_.Find(texture_handle);
          const auto sampler_slot = sampler_slots_.Find(sampler_handle);
          if (texture_slot && sampler_slot) {
            resolved.texture_index = texture_slot->index;
            resolved.sampler_index = sampler_slot->index;
          }
          return resolved;
        };
    const auto base_color = resolve_texture(
        material.base_color_texture, MaterialFeature::BaseColorTexture);
    constants.texture_index = base_color.texture_index;
    constants.sampler_index = base_color.sampler_index;
    // Normal mapping needs texture coordinates; without them it is disabled
    // for the draw and reported through render::ForwardNormalMapDisabled.
    if (!geometry_record.has_texcoords) {
      constants.feature_mask &=
          ~static_cast<std::uint32_t>(MaterialFeature::NormalTexture |
                                      MaterialFeature::MetallicTexture |
                                      MaterialFeature::RoughnessTexture);
    }
    const auto normal_map = resolve_texture(
        material.normal_texture, MaterialFeature::NormalTexture);
    const auto metallic_map = resolve_texture(
        material.metallic_texture, MaterialFeature::MetallicTexture);
    const auto roughness_map = resolve_texture(
        material.roughness_texture, MaterialFeature::RoughnessTexture);

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

    const bool use_gpu_scene = gpu_scene_ready && !material.module;
    auto material_constants = MakeMaterial(material, lighting, frame_policy);
    material_constants.normal_map_resource = {
        normal_map.texture_index, normal_map.sampler_index, 0U, 0U};
    material_constants.scalar_texture_resources = {metallic_map.texture_index,
        metallic_map.sampler_index, roughness_map.texture_index,
        roughness_map.sampler_index};
    [encoder setRenderPipelineState:use_gpu_scene
                                        ? (float_color ? float_gpu_scene_pipeline_ : gpu_scene_pipeline_)
                                        : (float_color ? float_pipeline_ : pipeline_)];
    [encoder setVertexBuffer:geometry->second.vertices offset:0 atIndex:0];
    [encoder setFragmentBytes:&material_constants
                       length:sizeof(material_constants)
                      atIndex:2];
    if (use_gpu_scene) {
      GpuSceneDrawConstants gpu_scene_constants;
      gpu_scene_constants.view_projection = view_projection;
      gpu_scene_constants.draw_slot = (*gpu_scene_draw_slots)[draw_index];
      [encoder setVertexBytes:&gpu_scene_constants
                       length:sizeof(gpu_scene_constants)
                      atIndex:1];
      [encoder setFragmentBytes:&gpu_scene_constants
                         length:sizeof(gpu_scene_constants)
                        atIndex:1];
      [encoder setVertexBuffer:gpu_scene_buffers_.geometries
                        offset:0
                       atIndex:4];
      [encoder setVertexBuffer:gpu_scene_buffers_.instances
                        offset:0
                       atIndex:5];
      [encoder setVertexBuffer:gpu_scene_buffers_.materials
                        offset:0
                       atIndex:6];
      [encoder setVertexBuffer:gpu_scene_buffers_.draws offset:0 atIndex:7];
      [encoder setFragmentBuffer:gpu_scene_buffers_.geometries
                          offset:0
                         atIndex:4];
      [encoder setFragmentBuffer:gpu_scene_buffers_.instances
                          offset:0
                         atIndex:5];
      [encoder setFragmentBuffer:gpu_scene_buffers_.materials
                          offset:0
                         atIndex:6];
      [encoder setFragmentBuffer:gpu_scene_buffers_.draws
                          offset:0
                         atIndex:7];
      ++build.telemetry.gpu_scene_draw_count;
    } else {
      [encoder setVertexBytes:&constants length:sizeof(constants) atIndex:1];
      [encoder setFragmentBytes:&constants length:sizeof(constants) atIndex:1];
    }
    if (!bindless_) {
      [encoder setFragmentTexture:base_color.texture atIndex:0];
      [encoder setFragmentSamplerState:base_color.sampler atIndex:0];
      [encoder setFragmentTexture:normal_map.texture atIndex:1];
      [encoder setFragmentSamplerState:normal_map.sampler atIndex:1];
      [encoder setFragmentTexture:metallic_map.texture atIndex:2];
      [encoder setFragmentSamplerState:metallic_map.sampler atIndex:2];
      [encoder setFragmentTexture:roughness_map.texture atIndex:3];
      [encoder setFragmentSamplerState:roughness_map.sampler atIndex:3];
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
  if (offset_surfaces) {
    [encoder setDepthBias:0.0F slopeScale:0.0F clamp:0.0F];
  }
  const auto display = render::PrepareMeshDisplay(*request.snapshot,
      request.width, request.height, request.mesh_display);
  if (!display.draws.empty()) {
    const auto vertices = [device_ newBufferWithBytes:display.vertices.data()
        length:display.vertices.size() * sizeof(render::MeshDisplayVertex)
        options:MTLResourceStorageModeShared];
    if (!vertices) {
      [encoder endEncoding];
      throw render::RendererError(render::RendererErrorCode::BackendFailure,
          "upload Metal mesh display vertices", "newBufferWithBytes returned nil");
    }
    const bool points = request.mesh_display.mode == render::MeshDisplayMode::Points;
    const std::size_t index = points ? 1 : 0;
    [encoder setRenderPipelineState:float_color ? float_mesh_display_pipelines_[index]
                                              : mesh_display_pipelines_[index]];
    [encoder setCullMode:MTLCullModeNone];
    for (const auto& draw : display.draws) {
      [encoder setVertexBuffer:vertices
                        offset:draw.first * sizeof(render::MeshDisplayVertex) atIndex:0];
      [encoder setVertexBytes:&draw.constants length:sizeof(draw.constants) atIndex:1];
      [encoder setFragmentBytes:&draw.constants length:sizeof(draw.constants) atIndex:1];
      if (points) {
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6
                  instanceCount:draw.count];
      } else {
        [encoder drawPrimitives:MTLPrimitiveTypeLine vertexStart:0 vertexCount:draw.count];
      }
    }
  }
  const bool has_gaussians = frame.gaussian_execution || frame.gaussian_count != 0;
  const bool sample_raster = has_gaussians && frame.gaussian_timestamps != nil;
  if (sample_raster) {
    // Stage-boundary sampling requires a separate render pass on Apple GPUs.
    // Store the opaque Mesh attachments, then load them for Gaussian blend
    // and depth/ID tests. The extra pass is included in this timing.
    [encoder endEncoding];
    for (NSUInteger i = 0; i < 3; ++i)
      pass.colorAttachments[i].loadAction = MTLLoadActionLoad;
    pass.depthAttachment.loadAction = MTLLoadActionLoad;
    pass.sampleBufferAttachments[0].sampleBuffer = frame.gaussian_timestamps;
    pass.sampleBufferAttachments[0].startOfVertexSampleIndex = kGaussianRasterBegin;
    pass.sampleBufferAttachments[0].endOfFragmentSampleIndex = kGaussianRasterEnd;
    encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!encoder)
      throw render::RendererError(render::RendererErrorCode::BackendFailure,
          "encode Metal Gaussian render pass", "renderCommandEncoder returned nil");
    [encoder setViewport:viewport];
    [encoder setScissorRect:scissor];
  }
  if (has_gaussians) {
    const auto begin = Clock::now();
    [encoder setRenderPipelineState:gaussian_pipeline_];
    [encoder setDepthStencilState:gaussian_depth_state_];
    [encoder setCullMode:MTLCullModeNone];
    auto instances = frame.gaussian_execution
                         ? frame.gaussian_execution->scratch->instances
                         : frame.gaussian_instances;
    [encoder setVertexBuffer:instances offset:0 atIndex:MERLIN_GAUSSIAN_INSTANCES_BINDING];
    const Vec2 inverse_extent{1.0F / request.width, 1.0F / request.height};
    [encoder setVertexBytes:&inverse_extent length:sizeof(inverse_extent) atIndex:MERLIN_GAUSSIAN_CONSTANTS_BINDING];
    [encoder setFragmentBytes:&inverse_extent length:sizeof(inverse_extent) atIndex:MERLIN_GAUSSIAN_CONSTANTS_BINDING];
    // Color blends back-to-front; integer IDs retain the nearest contributing
    // particle. Test against opaque mesh depth without replacing that depth.
    if (frame.gaussian_execution) {
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                indirectBuffer:frame.gaussian_execution->scratch->draw
          indirectBufferOffset:0];
    } else {
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                  vertexStart:0
                  vertexCount:6
                instanceCount:frame.gaussian_count];
    }
    ++build.telemetry.gaussian_draw_count;
    ++build.telemetry.draw_count;
    build.gaussian_raster_ns = DurationNs(begin, Clock::now());
  }
  [encoder endEncoding];
  if (frame.gaussian_execution && frame.gaussian_execution->tiled)
    gaussian_execution_->EncodeTileRaster(*frame.gaussian_execution, command,
        frame.gaussian_color, frame.depth, frame.prim_id, frame.instance_id,
        frame.gaussian_timestamps);
  if (float_color) {
    auto descriptor = [MTLComputePassDescriptor computePassDescriptor];
    if (sample_raster) {
      descriptor.sampleBufferAttachments[0].sampleBuffer = frame.gaussian_timestamps;
      descriptor.sampleBufferAttachments[0].startOfEncoderSampleIndex = kGaussianColorConvertBegin;
      descriptor.sampleBufferAttachments[0].endOfEncoderSampleIndex = kGaussianColorConvertEnd;
    }
    id<MTLComputeCommandEncoder> convert = [command computeCommandEncoderWithDescriptor:descriptor];
    if (!convert)
      throw render::RendererError(render::RendererErrorCode::BackendFailure,
          "encode Metal color conversion", "compute encoder returned nil");
    [convert setComputePipelineState:color_convert_pipeline_];
    [convert setTexture:frame.gaussian_color atIndex:0];
    [convert setTexture:frame.color atIndex:1];
    [convert dispatchThreads:MTLSizeMake(request.width, request.height, 1)
        threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [convert endEncoding];
  }
}

} // namespace merlin::metal
