#include "renderer_impl.hpp"

namespace merlin::vulkan {

std::uint64_t Renderer::Impl::ReadGpuTimestampSpanNanoseconds(
    const FrameContext& frame, std::uint32_t first,
    std::uint32_t second) const {
  if (frame.timestamp_pool == VK_NULL_HANDLE) {
    return 0;
  }
  if (second != first + 1U) {
    throw std::logic_error("timestamp span queries must be adjacent");
  }
  std::array<std::uint64_t, 2> timestamps{};
  Check(vkGetQueryPoolResults(device_, frame.timestamp_pool, first,
            static_cast<std::uint32_t>(timestamps.size()),
            sizeof(timestamps), timestamps.data(),
            sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT),
      "read frame timestamp queries");
  const std::uint64_t mask = selected_timestamp_valid_bits_ >= 64U
                                 ? std::numeric_limits<std::uint64_t>::max()
                                 : (std::uint64_t{1}
                                       << selected_timestamp_valid_bits_) -
                                       1U;
  const auto ticks = (timestamps[1] - timestamps[0]) & mask;
  return static_cast<std::uint64_t>(
      static_cast<long double>(ticks) * timestamp_period_ns_);
}

std::uint64_t Renderer::Impl::ReadGpuExecutionNanoseconds(const FrameContext& frame) const {
  return ReadGpuTimestampSpanNanoseconds(frame, 0, 1);
}

ImageRgba8 Renderer::Impl::ReadColor(std::uint32_t width, std::uint32_t height) {
  frame_counters_.readback_bytes +=
      static_cast<std::uint64_t>(width) * height * 4U;
  ImageRgba8 result;
  result.product = MakeRenderProduct(width, height, Aov::Color);
  result.row_pitch_bytes = width * BytesPerPixel(result.product.format);
  result.pixels.resize(static_cast<std::size_t>(result.row_pitch_bytes) *
                       height);
  void* mapped{};
  ++frame_counters_.map_count;
  Check(vkMapMemory(device_, active_target_->color_readback.memory, 0,
            active_target_->color_readback.size, 0, &mapped),
      "map color readback");
  std::memcpy(result.pixels.data(), mapped, result.pixels.size());
  vkUnmapMemory(device_, active_target_->color_readback.memory);
  return result;
}

ImageDepth32 Renderer::Impl::ReadDepth(std::uint32_t width, std::uint32_t height) {
  frame_counters_.readback_bytes +=
      static_cast<std::uint64_t>(width) * height * sizeof(float);
  ImageDepth32 result;
  result.product = MakeRenderProduct(width, height, Aov::Depth);
  result.row_pitch_bytes = width * BytesPerPixel(result.product.format);
  result.pixels.resize(static_cast<std::size_t>(width) * height);
  void* mapped{};
  ++frame_counters_.map_count;
  Check(vkMapMemory(device_, active_target_->depth_readback.memory, 0,
            active_target_->depth_readback.size, 0, &mapped),
      "map depth readback");
  std::memcpy(result.pixels.data(), mapped,
      result.pixels.size() * sizeof(float));
  vkUnmapMemory(device_, active_target_->depth_readback.memory);
  return result;
}

ImageUint32 Renderer::Impl::ReadId(std::uint32_t width, std::uint32_t height, Aov aov,
    const Buffer& readback) {
  frame_counters_.readback_bytes +=
      static_cast<std::uint64_t>(width) * height * sizeof(std::uint32_t);
  ImageUint32 result;
  result.product = MakeRenderProduct(width, height, aov);
  result.row_pitch_bytes = width * BytesPerPixel(result.product.format);
  result.pixels.resize(static_cast<std::size_t>(width) * height);
  void* mapped{};
  ++frame_counters_.map_count;
  Check(vkMapMemory(device_, readback.memory, 0, readback.size, 0, &mapped),
      "map id readback");
  std::memcpy(result.pixels.data(), mapped,
      result.pixels.size() * sizeof(std::uint32_t));
  vkUnmapMemory(device_, readback.memory);
  return result;
}

} // namespace merlin::vulkan
