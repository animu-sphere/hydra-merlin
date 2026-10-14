#include "backend_impl.hpp"

namespace merlin::metal {

MaterialConstants
Backend::Impl::MakeMaterial(const extraction::MaterialRecord& material,
    const render::ForwardDirectionalLighting& lighting,
    Vec4 output_transform) {
  return render::MakeForwardMaterialConstants<MaterialConstants>(material, lighting,
      render::PackagedDiffuseEnvironment().coefficients, output_transform);
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

  const auto output_transform =
      render::PackForwardOutputTransform(request.output_transform);
  const auto lighting =
      render::ExtractForwardDirectionalLighting(*request.snapshot);
  id<MTLRenderCommandEncoder> encoder =
      [command renderCommandEncoderWithDescriptor:pass];
  if (encoder == nil) {
    throw render::RendererError(render::RendererErrorCode::BackendFailure,
        "encode Metal render pass",
        "renderCommandEncoder returned nil");
  }
  [encoder setRenderPipelineState:float_color ? float_pipeline_ : pipeline_];
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
  for (std::size_t draw_index = 0;
      draw_index < request.snapshot->draws.size(); ++draw_index) {
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
    [encoder setCullMode:material.double_sided ? MTLCullModeNone
                                               : MTLCullModeBack];

    DrawConstants constants;
    constants.model_view_projection =
        render::MultiplyForwardMatrices(view_projection, instance.transform);
    const auto normal = render::ForwardNormalMatrix(instance.transform);
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

    const bool use_gpu_scene = gpu_scene_ready && !material.module;
    const auto material_constants = MakeMaterial(material, lighting, output_transform);
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
