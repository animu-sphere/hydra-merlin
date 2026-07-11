#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <merlin/core/render_product.hpp>
#include <merlin/extraction/scene_extractor.hpp>

namespace merlin::vulkan {

struct RendererOptions {
  bool enable_validation{};
  std::uint32_t frames_in_flight{3};
};

struct RendererCapabilities {
  std::string device_name;
  std::uint32_t api_version{};
  std::uint32_t max_image_dimension_2d{};
  bool timeline_semaphore{};
  bool validation_enabled{};
  bool graphics_queue{};
  bool compute_queue{};
  bool transfer_queue{};
};

struct RendererStatistics {
  std::uint64_t frames_submitted{};
  std::uint64_t scene_uploads{};
  std::uint64_t validation_messages{};
  std::uint32_t frame_context_count{};
};

struct ShaderPaths {
  std::filesystem::path vertex;
  std::filesystem::path fragment;
};

struct ImageRgba8 {
  RenderProduct product;
  std::uint32_t row_pitch_bytes{};
  std::vector<std::uint8_t> pixels;
};

struct ImageDepth32 {
  RenderProduct product;
  std::uint32_t row_pitch_bytes{};
  std::vector<float> pixels;
};

struct RenderResult {
  ImageRgba8 color;
  ImageDepth32 depth;
  std::uint64_t scene_revision{};
  std::uint64_t completion_value{};
};

// Throws std::invalid_argument when a backend result violates Merlin's Tier 0
// CPU readback contract.
void ValidateRenderResult(const RenderResult& result);

class Renderer {
 public:
  explicit Renderer(RendererOptions options = {});
  ~Renderer();

  Renderer(Renderer&&) noexcept;
  Renderer& operator=(Renderer&&) noexcept;
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  [[nodiscard]] const RendererCapabilities& capabilities() const noexcept;
  [[nodiscard]] RendererStatistics statistics() const noexcept;
  [[nodiscard]] RenderResult Render(const extraction::ExtractedScene& scene,
                                    std::uint32_t width,
                                    std::uint32_t height,
                                    const ShaderPaths& shaders);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace merlin::vulkan
