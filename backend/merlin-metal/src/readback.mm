#include "backend_impl.hpp"

namespace merlin::metal {

void Backend::Impl::EncodeReadback(id<MTLCommandBuffer> command, FrameContext& frame,
    const std::vector<Aov>& readbacks) {
  if (readbacks.empty()) {
    return;
  }
  id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
  const auto pitch = AlignedRowPitch(frame.width);
  const auto size = static_cast<NSUInteger>(pitch) * frame.height;
  const MTLOrigin origin{0, 0, 0};
  const MTLSize extent{frame.width, frame.height, 1};
  const auto copy = [&](Aov aov, id<MTLTexture> texture,
                        id<MTLBuffer> buffer) {
    if (!HasAov(readbacks, aov)) {
      return;
    }
    [encoder copyFromTexture:texture
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:origin
                      sourceSize:extent
                        toBuffer:buffer
               destinationOffset:0
          destinationBytesPerRow:pitch
        destinationBytesPerImage:size];
  };
  copy(Aov::Color, frame.color, frame.color_readback);
  copy(Aov::Depth, frame.depth, frame.depth_readback);
  copy(Aov::PrimId, frame.prim_id, frame.prim_readback);
  copy(Aov::InstanceId, frame.instance_id, frame.instance_readback);
  [encoder endEncoding];
}

std::vector<std::uint8_t> Backend::Impl::TightColorCopy(id<MTLBuffer> buffer,
    std::uint32_t width,
    std::uint32_t height) {
  std::vector<std::uint8_t> result(static_cast<std::size_t>(width) * height *
                                   4U);
  const auto source_pitch = AlignedRowPitch(width);
  const auto row_bytes = static_cast<std::size_t>(width) * 4U;
  const auto* source = static_cast<const std::byte*>(buffer.contents);
  auto* destination = reinterpret_cast<std::byte*>(result.data());
  for (std::uint32_t row = 0; row < height; ++row) {
    std::memcpy(destination + row * row_bytes,
        source + static_cast<std::size_t>(row) * source_pitch,
        row_bytes);
  }
  return result;
}

void Backend::Impl::CopyReadbacks(FrameContext& frame, Pending& pending) {
  const auto width = pending.width;
  const auto height = pending.height;
  const auto tight_pitch = width * 4U;
  const auto bytes = static_cast<std::uint64_t>(tight_pitch) * height;
  if (HasAov(pending.readback_aovs, Aov::Color)) {
    pending.result.color.product =
        MakeRenderProduct(width, height, Aov::Color);
    pending.result.color.row_pitch_bytes = tight_pitch;
    pending.result.color.pixels =
        TightColorCopy(frame.color_readback, width, height);
    pending.result.telemetry.readback_bytes += bytes;
  }
  if (HasAov(pending.readback_aovs, Aov::Depth)) {
    pending.result.depth.product =
        MakeRenderProduct(width, height, Aov::Depth);
    pending.result.depth.row_pitch_bytes = tight_pitch;
    pending.result.depth.pixels =
        TightCopy<float>(frame.depth_readback, width, height);
    pending.result.telemetry.readback_bytes += bytes;
  }
  if (HasAov(pending.readback_aovs, Aov::PrimId)) {
    pending.result.prim_id.product =
        MakeRenderProduct(width, height, Aov::PrimId);
    pending.result.prim_id.row_pitch_bytes = tight_pitch;
    pending.result.prim_id.pixels =
        TightCopy<std::uint32_t>(frame.prim_readback, width, height);
    pending.result.telemetry.readback_bytes += bytes;
  }
  if (HasAov(pending.readback_aovs, Aov::InstanceId)) {
    pending.result.instance_id.product =
        MakeRenderProduct(width, height, Aov::InstanceId);
    pending.result.instance_id.row_pitch_bytes = tight_pitch;
    pending.result.instance_id.pixels =
        TightCopy<std::uint32_t>(frame.instance_readback, width, height);
    pending.result.telemetry.readback_bytes += bytes;
  }
  for (const auto aov : pending.rendered_aovs) {
    pending.result.telemetry.requested_aov_mask |= AovBit(aov);
    pending.result.telemetry.rendered_aov_mask |= AovBit(aov);
  }
  for (const auto aov : pending.readback_aovs) {
    pending.result.telemetry.cpu_readback_aov_mask |= AovBit(aov);
  }
  pending.result.telemetry.requested_aov_count = pending.rendered_aovs.size();
  pending.result.telemetry.rendered_aov_count = pending.rendered_aovs.size();
  pending.result.telemetry.cpu_readback_aov_count =
      pending.readback_aovs.size();
  pending.result.telemetry.resolve_count = 1;
  pending.result.telemetry.map_count = pending.readback_aovs.empty() ? 0 : 1;
  pending.result.telemetry.wait_count = 1;
}

} // namespace merlin::metal
