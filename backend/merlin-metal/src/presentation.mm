#include "backend_impl.hpp"

namespace merlin::metal {

void Backend::Impl::ResizePresentationTarget(render::PresentationTarget target,
    std::uint32_t width, std::uint32_t height) {
  ValidatePresentation(target, "resize Metal presentation");
  ValidatePresentationExtent(width, height, "resize Metal presentation");
  UpdatePresentationExtent(width, height);
}

void Backend::Impl::ConfigurePresentation(const PresentationOptions& options) {
  MTLSamplerDescriptor* sampler = [MTLSamplerDescriptor new];
  sampler.minFilter = MTLSamplerMinMagFilterLinear;
  sampler.magFilter = MTLSamplerMinMagFilterLinear;
  sampler.sAddressMode = MTLSamplerAddressModeClampToEdge;
  sampler.tAddressMode = MTLSamplerAddressModeClampToEdge;
  presentation_sampler_ = [device_ newSamplerStateWithDescriptor:sampler];
  if (presentation_sampler_ == nil) {
    throw render::RendererError(render::RendererErrorCode::ResourceExhausted,
        "create Metal presentation sampler", "newSamplerState returned nil");
  }
  layer_ = DecodeHandle<CAMetalLayer*>(options.layer);
  layer_.device = device_;
  layer_.pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
  layer_.framebufferOnly = YES;
  layer_.displaySyncEnabled = options.vsync ? YES : NO;
  layer_.maximumDrawableCount = options.drawable_count;
  layer_.allowsNextDrawableTimeout = YES;
  layer_.presentsWithTransaction = NO;
  presentation_color_space_ = options.color_space;
  CGColorSpaceRef color_space = CGColorSpaceCreateWithName(
      options.color_space == PresentationColorSpace::DisplayP3
          ? kCGColorSpaceDisplayP3
          : kCGColorSpaceSRGB);
  layer_.colorspace = color_space;
  CGColorSpaceRelease(color_space);
  presentation_ = render::PresentationTarget(owner_, 1);
  NotifyOverlay(PresentationOverlayPhase::Initialize);
}

void Backend::Impl::NotifyOverlay(PresentationOverlayPhase phase,
    id<MTLCommandBuffer> command,
    id<MTLRenderCommandEncoder> encoder,
    MTLRenderPassDescriptor* pass) {
  if (!options_.presentation ||
      options_.presentation->render_overlay == nullptr) {
    return;
  }
  PresentationOverlayContext context;
  context.phase = phase;
  context.device =
      reinterpret_cast<std::uintptr_t>((__bridge void*)device_);
  context.command_buffer =
      reinterpret_cast<std::uintptr_t>((__bridge void*)command);
  context.render_encoder =
      reinterpret_cast<std::uintptr_t>((__bridge void*)encoder);
  context.render_pass_descriptor =
      reinterpret_cast<std::uintptr_t>((__bridge void*)pass);
  options_.presentation->render_overlay(
      options_.presentation->overlay_user_data, context);
}

void Backend::Impl::ValidatePresentation(render::PresentationTarget target,
    const char* operation) const {
  if (!presentation_ || target != *presentation_) {
    throw render::RendererError(
        render::RendererErrorCode::InvalidRequest, operation,
        "presentation target belongs to another backend");
  }
}

void Backend::Impl::ValidatePresentationExtent(std::uint32_t width, std::uint32_t height,
    const char* operation) const {
  if (width == 0 || height == 0 ||
      width > capabilities_.limits.max_image_dimension_2d ||
      height > capabilities_.limits.max_image_dimension_2d) {
    throw render::RendererError(render::RendererErrorCode::InvalidRequest,
        operation,
        "presentation extent is invalid");
  }
}

void Backend::Impl::UpdatePresentationExtent(std::uint32_t width, std::uint32_t height) {
  ValidatePresentationExtent(width, height, "resize Metal presentation");
  if (presentation_width_ == width && presentation_height_ == height) {
    return;
  }
  if (presentation_width_ != 0 && presentation_height_ != 0) {
    ++statistics_.presentation_recreates;
  }
  presentation_width_ = width;
  presentation_height_ = height;
  layer_.drawableSize = CGSizeMake(width, height);
}

void Backend::Impl::EncodePresentation(id<MTLCommandBuffer> command, FrameContext& frame,
    id<CAMetalDrawable> drawable, FrameBuild& build) {
  MTLRenderPassDescriptor* pass =
      [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = drawable.texture;
  pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTLRenderCommandEncoder> encoder =
      [command renderCommandEncoderWithDescriptor:pass];
  if (encoder == nil) {
    throw render::RendererError(
        render::RendererErrorCode::BackendFailure,
        "encode Metal presentation pass",
        "renderCommandEncoder returned nil");
  }
  [encoder setRenderPipelineState:presentation_pipeline_];
  const auto color_space =
      presentation_color_space_ == PresentationColorSpace::DisplayP3 ? 1U
                                                                     : 0U;
  [encoder setFragmentBytes:&color_space
                     length:sizeof(color_space)
                    atIndex:0];
  [encoder setFragmentTexture:frame.color atIndex:0];
  [encoder setFragmentSamplerState:presentation_sampler_ atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle
              vertexStart:0
              vertexCount:3];
  try {
    NotifyOverlay(PresentationOverlayPhase::Render, command, encoder, pass);
  } catch (...) {
    [encoder endEncoding];
    throw;
  }
  [encoder endEncoding];
  build.telemetry.present_count = 1;
  build.telemetry.presentation_copy_bytes =
      static_cast<std::uint64_t>(frame.width) * frame.height * 4U;
}

} // namespace merlin::metal
