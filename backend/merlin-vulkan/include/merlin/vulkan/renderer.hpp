#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace merlin::vulkan {

struct RendererOptions {
  bool enable_validation{};
};

struct DeviceCapabilities {
  std::string device_name;
  std::uint32_t api_version{};
  std::uint32_t max_image_dimension_2d{};
  bool timeline_semaphore{};
  bool validation_enabled{};
};

struct ShaderPaths {
  std::filesystem::path vertex;
  std::filesystem::path fragment;
};

struct ImageRgba8 {
  std::uint32_t width{};
  std::uint32_t height{};
  std::vector<std::uint8_t> pixels;
};

class Renderer {
 public:
  explicit Renderer(RendererOptions options = {});
  ~Renderer();

  Renderer(Renderer&&) noexcept;
  Renderer& operator=(Renderer&&) noexcept;
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  [[nodiscard]] const DeviceCapabilities& capabilities() const noexcept;
  [[nodiscard]] ImageRgba8 RenderTriangle(std::uint32_t width,
                                          std::uint32_t height,
                                          const ShaderPaths& shaders);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace merlin::vulkan
