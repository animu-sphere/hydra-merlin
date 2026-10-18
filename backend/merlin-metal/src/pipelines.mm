#include "backend_impl.hpp"
#include "forward_metallib.hpp"
#include "gaussian_metallib.hpp"

namespace merlin::metal {

void Backend::Impl::CreateLibraryAndPipeline() {
  NSError* error = nil;
  dispatch_data_t forward_data = dispatch_data_create(kForwardMetalLibrary,
      sizeof(kForwardMetalLibrary), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  library_ = [device_ newLibraryWithData:forward_data error:&error];
  if (library_ == nil) {
    throw render::RendererError(
        render::RendererErrorCode::BackendFailure, "load Metal Forward library",
        error == nil ? "newLibraryWithData returned nil" : String(error.localizedDescription),
        static_cast<std::int32_t>(error.code));
  }
  id<MTLFunction> vertex = [library_ newFunctionWithName:@"merlin_vertex"];
  NSString* fragment_name = bindless_ ? @"merlin_fragment_argument_buffer"
                                      : @"merlin_fragment_conventional";
  id<MTLFunction> fragment = [library_ newFunctionWithName:fragment_name];
  if (vertex == nil || fragment == nil) {
    throw render::RendererError(render::RendererErrorCode::BackendFailure,
        "load Metal shader entry point",
        "compiled entry point is missing");
  }

  MTLVertexDescriptor* vertices = [MTLVertexDescriptor vertexDescriptor];
  vertices.attributes[0].format = MTLVertexFormatFloat3;
  vertices.attributes[0].offset = offsetof(extraction::DrawVertex, position);
  vertices.attributes[0].bufferIndex = 0;
  vertices.attributes[1].format = MTLVertexFormatFloat3;
  vertices.attributes[1].offset = offsetof(extraction::DrawVertex, normal);
  vertices.attributes[1].bufferIndex = 0;
  vertices.attributes[2].format = MTLVertexFormatFloat4;
  vertices.attributes[2].offset = offsetof(extraction::DrawVertex, color);
  vertices.attributes[2].bufferIndex = 0;
  vertices.attributes[3].format = MTLVertexFormatFloat2;
  vertices.attributes[3].offset = offsetof(extraction::DrawVertex, texcoord);
  vertices.attributes[3].bufferIndex = 0;
  vertices.attributes[4].format = MTLVertexFormatFloat4;
  vertices.attributes[4].offset = offsetof(extraction::DrawVertex, tangent);
  vertices.attributes[4].bufferIndex = 0;
  vertices.layouts[0].stride = sizeof(extraction::DrawVertex);
  vertices.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

  MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
  descriptor.label = @"hdMerlin Forward";
  descriptor.vertexFunction = vertex;
  descriptor.fragmentFunction = fragment;
  descriptor.vertexDescriptor = vertices;
  descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
  descriptor.colorAttachments[1].pixelFormat = MTLPixelFormatR32Uint;
  descriptor.colorAttachments[2].pixelFormat = MTLPixelFormatR32Uint;
  descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
  pipeline_ = [device_ newRenderPipelineStateWithDescriptor:descriptor
                                                      error:&error];
  if (pipeline_ == nil) {
    throw render::RendererError(render::RendererErrorCode::BackendFailure,
        "create Metal render pipeline",
        error == nil
            ? "newRenderPipelineState returned nil"
            : String(error.localizedDescription),
        static_cast<std::int32_t>(error.code));
  }
  descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
  float_pipeline_ = [device_ newRenderPipelineStateWithDescriptor:descriptor error:&error];
  if (!float_pipeline_)
    throw render::RendererError(render::RendererErrorCode::BackendFailure,
        "create Metal float Forward pipeline", String(error.localizedDescription));
  descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
  if (bindless_) {
    id<MTLFunction> gpu_scene_vertex =
        [library_ newFunctionWithName:@"merlin_vertex_gpu_scene"];
    id<MTLFunction> gpu_scene_fragment =
        [library_ newFunctionWithName:@"merlin_fragment_gpu_scene"];
    if (gpu_scene_vertex == nil || gpu_scene_fragment == nil) {
      throw render::RendererError(
          render::RendererErrorCode::BackendFailure,
          "load Metal GPU Scene entry point",
          "compiled GPU Scene entry point is missing");
    }
    descriptor.label = @"hdMerlin GPU Scene Forward";
    descriptor.vertexFunction = gpu_scene_vertex;
    descriptor.fragmentFunction = gpu_scene_fragment;
    gpu_scene_pipeline_ =
        [device_ newRenderPipelineStateWithDescriptor:descriptor
                                                error:&error];
    if (gpu_scene_pipeline_ == nil) {
      throw render::RendererError(
          render::RendererErrorCode::BackendFailure,
          "create Metal GPU Scene pipeline",
          error == nil ? "newRenderPipelineState returned nil"
                       : String(error.localizedDescription),
          static_cast<std::int32_t>(error.code));
    }
  }
  if (bindless_) {
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    float_gpu_scene_pipeline_ = [device_ newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!float_gpu_scene_pipeline_)
      throw render::RendererError(render::RendererErrorCode::BackendFailure,
          "create Metal float GPU Scene pipeline", String(error.localizedDescription));
    argument_encoder_ = [fragment newArgumentEncoderWithBufferIndex:3];
    if (argument_encoder_ == nil) {
      throw render::RendererError(render::RendererErrorCode::BackendFailure,
          "create Metal argument encoder",
          "fragment argument encoder is unavailable");
    }
  }
  // The Gaussian library is compiled at build time and embedded so installed
  // static-library consumers do not depend on the build tree or working directory.
  dispatch_data_t gaussian_data = dispatch_data_create(kGaussianMetalLibrary,
      sizeof(kGaussianMetalLibrary), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  gaussian_library_ = [device_ newLibraryWithData:gaussian_data error:&error];
  if (gaussian_library_ == nil) {
    throw render::RendererError(render::RendererErrorCode::BackendFailure,
        "load Metal Gaussian library",
        error == nil ? "newLibraryWithData returned nil" : String(error.localizedDescription),
        static_cast<std::int32_t>(error.code));
  }
  MTLRenderPipelineDescriptor* gaussian = [MTLRenderPipelineDescriptor new];
  gaussian.label = @"hdMerlin Gaussian";
  gaussian.vertexFunction = [gaussian_library_ newFunctionWithName:@"gaussian_metal_vertex"];
  gaussian.fragmentFunction = [gaussian_library_ newFunctionWithName:@"gaussian_metal_fragment"];
  gaussian.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
  gaussian.colorAttachments[0].blendingEnabled = YES;
  gaussian.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
  gaussian.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
  gaussian.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
  gaussian.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
  gaussian.colorAttachments[1].pixelFormat = MTLPixelFormatR32Uint;
  gaussian.colorAttachments[2].pixelFormat = MTLPixelFormatR32Uint;
  gaussian.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
  gaussian_pipeline_ = [device_ newRenderPipelineStateWithDescriptor:gaussian error:&error];
  if (gaussian_pipeline_ == nil) {
    throw render::RendererError(render::RendererErrorCode::BackendFailure,
        "create Metal Gaussian pipeline",
        error == nil ? "newRenderPipelineState returned nil" : String(error.localizedDescription),
        static_cast<std::int32_t>(error.code));
  }
  color_convert_pipeline_ = [device_ newComputePipelineStateWithFunction:
          [library_ newFunctionWithName:@"merlin_convert_color"]
                                                                   error:&error];
  if (!color_convert_pipeline_)
    throw render::RendererError(render::RendererErrorCode::BackendFailure,
        "create Metal color conversion pipeline", String(error.localizedDescription));
  if (options_.presentation) {
    id<MTLFunction> presentation_vertex =
        [library_ newFunctionWithName:@"merlin_presentation_vertex"];
    id<MTLFunction> presentation_fragment =
        [library_ newFunctionWithName:@"merlin_presentation_fragment"];
    MTLRenderPipelineDescriptor* presentation =
        [MTLRenderPipelineDescriptor new];
    presentation.label = @"hdMerlin native presentation";
    presentation.vertexFunction = presentation_vertex;
    presentation.fragmentFunction = presentation_fragment;
    presentation.colorAttachments[0].pixelFormat =
        MTLPixelFormatBGRA8Unorm_sRGB;
    presentation_pipeline_ =
        [device_ newRenderPipelineStateWithDescriptor:presentation
                                                error:&error];
    if (presentation_pipeline_ == nil) {
      throw render::RendererError(
          render::RendererErrorCode::BackendFailure,
          "create Metal presentation pipeline",
          error == nil ? "newRenderPipelineState returned nil"
                       : String(error.localizedDescription),
          static_cast<std::int32_t>(error.code));
    }
  }
}

void Backend::Impl::CreateDepthState() {
  MTLDepthStencilDescriptor* descriptor = [MTLDepthStencilDescriptor new];
  descriptor.depthCompareFunction = MTLCompareFunctionLessEqual;
  descriptor.depthWriteEnabled = YES;
  depth_state_ = [device_ newDepthStencilStateWithDescriptor:descriptor];
  if (depth_state_ == nil) {
    throw render::RendererError(render::RendererErrorCode::BackendFailure,
        "create Metal depth state",
        "newDepthStencilState returned nil");
  }
  descriptor.depthWriteEnabled = NO;
  gaussian_depth_state_ = [device_ newDepthStencilStateWithDescriptor:descriptor];
  if (gaussian_depth_state_ == nil) {
    throw render::RendererError(render::RendererErrorCode::BackendFailure,
        "create Metal Gaussian depth state", "newDepthStencilState returned nil");
  }
}

namespace {
id<MTLComputePipelineState> GaussianComputePipeline(id<MTLDevice> device, id<MTLLibrary> library,
    NSString* name, NSUInteger threads = 256) {
  auto function = [library newFunctionWithName:name];
  if (!function)
    throw render::RendererError(render::RendererErrorCode::BackendFailure, "encode Metal Gaussian execution", "Missing Gaussian compute entry point");
  NSError* error = nil;
  auto pipeline = [device newComputePipelineStateWithFunction:function error:&error];
  if (!pipeline)
    throw render::RendererError(render::RendererErrorCode::BackendFailure, "encode Metal Gaussian execution", "Metal compute pipeline creation failed");
  if (pipeline.maxTotalThreadsPerThreadgroup < threads)
    throw render::RendererError(render::RendererErrorCode::Unsupported, "encode Metal Gaussian execution", "Metal device cannot run the portable radix workgroup");
  return pipeline;
}
} // namespace

void GaussianExecution::CreatePipelines(id<MTLLibrary> library) {
  prepare_ = GaussianComputePipeline(device_, library, @"gaussian_prepare_compact", 64);
  keys_ = GaussianComputePipeline(device_, library, @"gaussian_sort_keys");
  histogram_ = GaussianComputePipeline(device_, library, @"gaussian_sort_histogram");
  scan_ = GaussianComputePipeline(device_, library, @"gaussian_sort_scan_blocks");
  add_ = GaussianComputePipeline(device_, library, @"gaussian_sort_scan_add");
  scatter_ = GaussianComputePipeline(device_, library, @"gaussian_sort_scatter");
  verify_ = GaussianComputePipeline(device_, library, @"gaussian_sort_verify");
  gather_ = GaussianComputePipeline(device_, library, @"gaussian_metal_gather");
  try {
    tile_count_ = GaussianComputePipeline(device_, library, @"gaussian_tile_count");
    tile_emit_ = GaussianComputePipeline(device_, library, @"gaussian_tile_emit");
    tile_ranges_ = GaussianComputePipeline(device_, library, @"gaussian_tile_ranges");
    tile_verify_ = GaussianComputePipeline(device_, library, @"gaussian_tile_verify");
    tile_select_ = GaussianComputePipeline(device_, library, @"gaussian_tile_raster_select", 1);
    tile_raster_ = GaussianComputePipeline(device_, library, @"gaussian_tile_raster");
  } catch (const render::RendererError& error) {
    if (error.code() != render::RendererErrorCode::Unsupported &&
        error.code() != render::RendererErrorCode::BackendFailure)
      throw;
    tile_raster_ = nil;
  }
}

} // namespace merlin::metal
