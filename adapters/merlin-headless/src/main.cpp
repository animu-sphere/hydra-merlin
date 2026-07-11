#include <merlin/core/render_world.hpp>
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
    } else if (argument == "--validate") {
      result.validation = true;
    } else if (argument == "--probe") {
      result.probe_only = true;
    } else if (argument == "--help") {
      std::cout << "Usage: merlin-headless [--output FILE] [--width N] [--height N] "
                   "[--validate] [--probe]\n";
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

void ValidateSmokeImage(const merlin::vulkan::ImageRgba8& image) {
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
}

merlin::ChangeSet BuildSmokeWorld(merlin::RenderWorld& world) {
  merlin::MeshDescriptor mesh;
  mesh.label = "headless-triangle";
  mesh.positions = {{0.0F, -0.72F, 0.0F}, {0.72F, 0.62F, 0.0F},
                    {-0.72F, 0.62F, 0.0F}};
  mesh.indices = {0, 1, 2};
  const auto mesh_handle = world.CreateMesh(std::move(mesh));

  merlin::MaterialDescriptor material;
  material.label = "vertex-color-fallback";
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
      const auto image = renderer.RenderTriangle(arguments.width, arguments.height, shaders);
      ValidateSmokeImage(image);
      WritePpm(arguments.output, image);
      std::cout << "Wrote " << arguments.output.string() << " (" << image.width << 'x'
                << image.height << ")\n";
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "merlin-headless: " << error.what() << '\n';
    return 1;
  }
}
