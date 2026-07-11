#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/vulkan/renderer.hpp>

#include <vulkan/vulkan_core.h>

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

struct Arguments {
  std::filesystem::path output{"merlin.ppm"};
  std::uint32_t width{512};
  std::uint32_t height{512};
  std::uint32_t frames{3};
  bool validation{};
  bool probe_only{};
};

std::uint32_t ParseUnsigned(std::string_view text, std::string_view option) {
  std::uint32_t value{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value == 0) {
    throw std::invalid_argument(std::string(option) + " requires a positive integer");
  }
  return value;
}

Arguments ParseArguments(int argc, char** argv) {
  Arguments result;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    auto require_value = [&](std::string_view option) -> std::string_view {
      if (++i >= argc) {
        throw std::invalid_argument(std::string(option) + " requires a value");
      }
      return argv[i];
    };
    if (argument == "--output") {
      result.output = require_value(argument);
    } else if (argument == "--width") {
      result.width = ParseUnsigned(require_value(argument), argument);
    } else if (argument == "--height") {
      result.height = ParseUnsigned(require_value(argument), argument);
    } else if (argument == "--frames") {
      result.frames = ParseUnsigned(require_value(argument), argument);
    } else if (argument == "--validate") {
      result.validation = true;
    } else if (argument == "--probe") {
      result.probe_only = true;
    } else if (argument == "--help") {
      std::cout << "Usage: merlin-headless [--output FILE] [--width N] [--height N] "
                   "[--frames N] [--validate] [--probe]\n";
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown option: " + std::string(argument));
    }
  }
  return result;
}

void WritePpm(const std::filesystem::path& path,
              const merlin::vulkan::ImageRgba8& image) {
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("could not create output image: " + path.string());
  }
  stream << "P6\n" << image.width << ' ' << image.height << "\n255\n";
  for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
    stream.write(reinterpret_cast<const char*>(image.pixels.data() + i), 3);
  }
  if (!stream) {
    throw std::runtime_error("could not write output image: " + path.string());
  }
}

void ValidateSmokeResult(const merlin::vulkan::RenderResult& result) {
  const auto& image = result.color;
  const auto expected_size = static_cast<std::size_t>(image.width) * image.height * 4U;
  if (image.pixels.size() != expected_size) {
    throw std::runtime_error("Vulkan readback returned an invalid byte count");
  }
  const std::size_t center =
      (static_cast<std::size_t>(image.height / 2U) * image.width + image.width / 2U) * 4U;
  if (image.pixels[0] == image.pixels[center] &&
      image.pixels[1] == image.pixels[center + 1U] &&
      image.pixels[2] == image.pixels[center + 2U]) {
    throw std::runtime_error("offscreen smoke image does not contain the triangle");
  }
  const auto depth_center = result.depth.pixels[
      static_cast<std::size_t>(image.height / 2U) * image.width + image.width / 2U];
  const auto depth_corner = result.depth.pixels.front();
  if (!(depth_center < depth_corner)) {
    throw std::runtime_error("depth AOV does not contain triangle depth");
  }
}

merlin::ChangeSet BuildSmokeWorld(merlin::RenderWorld& world) {
  merlin::MeshDescriptor mesh;
  mesh.label = "headless-triangle";
  mesh.positions = {{0.0F, -0.72F, 0.0F}, {0.72F, 0.62F, 0.0F},
                    {-0.72F, 0.62F, 0.0F}};
  mesh.indices = {0, 1, 2};
  const auto mesh_handle = world.CreateMesh(std::move(mesh));

  merlin::MaterialDescriptor material;
  material.label = "fallback";
  material.base_color = {0.18F, 0.78F, 1.0F, 1.0F};
  const auto material_handle = world.CreateMaterial(std::move(material));

  merlin::InstanceDescriptor instance;
  instance.label = "triangle-instance";
  instance.mesh = mesh_handle;
  instance.material = material_handle;
  world.CreateInstance(std::move(instance));
  return world.Commit();
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto arguments = ParseArguments(argc, argv);
    merlin::RenderWorld world;
    const auto changes = BuildSmokeWorld(world);
    merlin::extraction::SceneExtractor extractor;
    extractor.Apply(world, changes);

    merlin::vulkan::Renderer renderer({arguments.validation});
    const auto& capabilities = renderer.capabilities();
    std::cout << "Merlin Vulkan device: " << capabilities.device_name << '\n'
              << "API version: " << VK_VERSION_MAJOR(capabilities.api_version) << '.'
              << VK_VERSION_MINOR(capabilities.api_version) << '.'
              << VK_VERSION_PATCH(capabilities.api_version) << '\n'
              << "Timeline semaphore: "
              << (capabilities.timeline_semaphore ? "yes" : "no") << '\n'
              << "Validation: " << (capabilities.validation_enabled ? "on" : "off") << '\n'
              << "RenderWorld revision: " << changes.revision << " ("
              << changes.changes.size() << " changes)\n";

    if (!arguments.probe_only) {
      const auto executable_dir = std::filesystem::absolute(argv[0]).parent_path();
      const merlin::vulkan::ShaderPaths shaders{
          executable_dir / "shaders" / "triangle.vert.spv",
          executable_dir / "shaders" / "triangle.frag.spv"};
      merlin::vulkan::RenderResult result;
      for (std::uint32_t frame = 0; frame < arguments.frames; ++frame) {
        result = renderer.Render(extractor.scene(), arguments.width,
                                 arguments.height, shaders);
      }
      ValidateSmokeResult(result);
      WritePpm(arguments.output, result.color);
      const auto statistics = renderer.statistics();
      if (statistics.scene_uploads != 1) {
        throw std::runtime_error("unchanged scene was uploaded more than once");
      }
      if (arguments.validation && statistics.validation_messages != 0) {
        throw std::runtime_error("Vulkan validation reported warnings or errors");
      }
      std::cout << "Wrote " << arguments.output.string() << " ("
                << result.color.width << 'x' << result.color.height << ", "
                << statistics.frames_submitted << " frames, completion "
                << result.completion_value << ")\n";
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "merlin-headless: " << error.what() << '\n';
    return 1;
  }
}
