#include "window.hpp"
#include "presentation_glfw.hpp"
#ifdef MERLIN_VIEWPORT_ENABLE_HYDRA2
#include "hydra_scene.hpp"
#endif

#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/render/backend.hpp>
#include <merlin/vulkan/backend.hpp>
#include <merlin/vulkan/shader_abi.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Arguments {
  std::uint32_t width{1280};
  std::uint32_t height{720};
  std::uint64_t frame_limit{};
  merlin::render::BackendRequest backend{
      merlin::render::BackendRequest::Automatic};
  bool validation{};
  bool vsync{true};
  bool visible{true};
  bool reference_check{};
  bool resize_test{};
  std::filesystem::path screenshot;
  std::filesystem::path benchmark;
  std::filesystem::path usd;
};

std::uint64_t ReadUnsigned(std::string_view value, std::string_view name) {
  std::size_t consumed{};
  const auto result = std::stoull(std::string(value), &consumed);
  if (consumed != value.size()) {
    throw std::invalid_argument(std::string(name) + " must be an integer");
  }
  return result;
}

merlin::render::BackendRequest ReadBackend(std::string_view value) {
  if (value == "automatic" || value == "auto") {
    return merlin::render::BackendRequest::Automatic;
  }
  if (value == "vulkan") {
    return merlin::render::BackendRequest::Vulkan;
  }
  if (value == "metal") {
    return merlin::render::BackendRequest::Metal;
  }
  throw std::invalid_argument(
      "--backend must be automatic, vulkan, or metal");
}

Arguments ParseArguments(int argc, char** argv) {
  Arguments result;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option(argv[index]);
    const auto next = [&]() -> std::string_view {
      if (++index >= argc) {
        throw std::invalid_argument(std::string(option) + " requires a value");
      }
      return argv[index];
    };
    if (option == "--width") {
      result.width = static_cast<std::uint32_t>(ReadUnsigned(next(), option));
    } else if (option == "--height") {
      result.height = static_cast<std::uint32_t>(ReadUnsigned(next(), option));
    } else if (option == "--frames") {
      result.frame_limit = ReadUnsigned(next(), option);
    } else if (option == "--backend") {
      result.backend = ReadBackend(next());
    } else if (option == "--validate") {
      result.validation = true;
    } else if (option == "--vsync") {
      const auto value = next();
      if (value != "on" && value != "off") {
        throw std::invalid_argument("--vsync must be on or off");
      }
      result.vsync = value == "on";
    } else if (option == "--hidden") {
      result.visible = false;
    } else if (option == "--reference-check") {
      result.reference_check = true;
    } else if (option == "--resize-test") {
      result.resize_test = true;
    } else if (option == "--screenshot") {
      result.screenshot = next();
    } else if (option == "--benchmark") {
      result.benchmark = next();
    } else if (option == "--usd") {
      result.usd = next();
    } else if (option == "--help") {
      std::cout
          << "Usage: merlin-viewport [options]\n"
             "  --backend automatic|vulkan|metal\n"
             "  --width N --height N --vsync on|off --validate\n"
             "  --frames N --benchmark report.json --screenshot image.ppm\n"
             "  --usd scene.usd --hidden --reference-check --resize-test\n"
             "USD controls: Alt+LMB tumble, Alt+MMB track, Alt+RMB dolly, "
             "wheel dolly, F frame all.\n"
             "Other controls: arrows pan, left click picks, S captures, Esc exits.\n";
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown option: " + std::string(option));
    }
  }
  if (result.width == 0 || result.height == 0) {
    throw std::invalid_argument("viewport extent must be non-zero");
  }
  if (!result.benchmark.empty() && result.frame_limit == 0) {
    result.frame_limit = 300;
  }
  return result;
}

struct Scene {
  merlin::RenderWorld world;
  merlin::extraction::SceneExtractor extractor;
  merlin::CameraHandle camera;
  merlin::CameraDescriptor camera_descriptor;
};

Scene BuildScene() {
  Scene scene;
  merlin::MeshDescriptor mesh;
  mesh.label = "viewport-triangle";
  mesh.positions = {{0.0F, -0.72F, 0.0F}, {0.72F, 0.62F, 0.0F},
                    {-0.72F, 0.62F, 0.0F}};
  mesh.normals = {{0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 1.0F},
                  {0.0F, 0.0F, 1.0F}};
  mesh.colors = {{1.0F, 0.18F, 0.08F}, {0.08F, 0.85F, 0.28F},
                 {0.08F, 0.38F, 1.0F}};
  mesh.indices = {0, 1, 2};
  const auto mesh_handle = scene.world.CreateMesh(std::move(mesh));

  merlin::MaterialDescriptor material;
  material.label = "viewport-material";
  material.parameters.base_color = {1.0F, 1.0F, 1.0F, 1.0F};
  const auto material_handle = scene.world.CreateMaterial(std::move(material));

  merlin::InstanceDescriptor instance;
  instance.label = "viewport-instance";
  instance.mesh = mesh_handle;
  instance.material = material_handle;
  scene.world.CreateInstance(std::move(instance));

  merlin::LightDescriptor light;
  light.label = "viewport-key";
  scene.world.CreateLight(std::move(light));

  scene.camera_descriptor.label = "viewport-camera";
  scene.camera = scene.world.CreateCamera(scene.camera_descriptor);
  scene.extractor.SetActiveCamera(scene.camera);
  scene.extractor.Apply(scene.world, scene.world.Commit());
  return scene;
}

void WritePpm(const std::filesystem::path& path,
              const merlin::render::ImageRgba8& image) {
  if (image.pixels.empty()) {
    throw std::runtime_error("screenshot request returned no color pixels");
  }
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("could not create screenshot: " + path.string());
  }
  stream << "P6\n" << image.product.width << ' ' << image.product.height
         << "\n255\n";
  for (std::size_t offset = 0; offset < image.pixels.size(); offset += 4) {
    stream.write(reinterpret_cast<const char*>(image.pixels.data() + offset),
                 3);
  }
}

void WriteBenchmark(const std::filesystem::path& path,
                    const merlin::render::BackendSelection& selection,
                    const merlin::render::RendererStatistics& statistics,
                    std::uint64_t frames, std::uint64_t elapsed_ns,
                    std::uint64_t gpu_ns,
                    std::uint64_t presented_readback_bytes,
                    std::uint64_t presentation_copy_bytes,
                    std::uint64_t zero_readback_frames) {
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("could not create benchmark report: " +
                             path.string());
  }
  stream << "{\n"
         << "  \"schema\": \"merlin.viewport-benchmark/v1\",\n"
         << "  \"backend\": \""
         << merlin::render::BackendKindName(selection.selected) << "\",\n"
         << "  \"frames\": " << frames << ",\n"
         << "  \"cpu_total_ns\": " << elapsed_ns << ",\n"
         << "  \"cpu_average_frame_ns\": "
         << (frames == 0 ? 0 : elapsed_ns / frames) << ",\n"
         << "  \"gpu_total_ns\": " << gpu_ns << ",\n"
         << "  \"gpu_average_frame_ns\": "
         << (frames == 0 ? 0 : gpu_ns / frames) << ",\n"
         << "  \"presented_readback_bytes\": " << presented_readback_bytes
         << ",\n"
         << "  \"presentation_copy_bytes\": " << presentation_copy_bytes
         << ",\n"
         << "  \"zero_readback_frames\": " << zero_readback_frames << ",\n"
         << "  \"frames_presented\": " << statistics.frames_presented
         << ",\n"
         << "  \"presentation_recreates\": "
         << statistics.presentation_recreates << ",\n"
         << "  \"validation_messages\": "
         << statistics.validation_messages << "\n"
         << "}\n";
}

std::string WindowTitle(std::string_view backend, std::uint32_t width,
                        std::uint32_t height, std::uint64_t frame_ns) {
  std::ostringstream title;
  title << "merlin-viewport | " << backend << " | " << width << 'x' << height;
  if (frame_ns != 0) {
    title << " | " << std::fixed << std::setprecision(2)
          << static_cast<double>(frame_ns) / 1'000'000.0 << " ms";
  }
  return title.str();
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto arguments = ParseArguments(argc, argv);
    if (!arguments.usd.empty()) {
#ifdef MERLIN_VIEWPORT_ENABLE_HYDRA2
      merlin::viewport::HydraViewportOptions options;
      options.stage = arguments.usd;
      options.executable = argv[0];
      options.screenshot = arguments.screenshot;
      options.benchmark = arguments.benchmark;
      options.width = arguments.width;
      options.height = arguments.height;
      options.frame_limit = arguments.frame_limit;
      options.backend = arguments.backend;
      options.validation = arguments.validation;
      options.vsync = arguments.vsync;
      options.visible = arguments.visible;
      options.reference_check = arguments.reference_check;
      options.resize_test = arguments.resize_test;
      return merlin::viewport::RunHydraViewport(options);
#else
      throw std::runtime_error(
          "this build does not include the Hydra viewport scene source; "
          "configure with MERLIN_ENABLE_HYDRA2=ON");
#endif
    }

    auto window = merlin::viewport::Window::Create(
        "merlin-viewport", arguments.width, arguments.height,
        arguments.visible);
    const auto executable_dir =
        std::filesystem::absolute(argv[0]).parent_path();
    const auto shader_dir =
        executable_dir / merlin::vulkan::shader_abi::ArtifactDirectory();
    merlin::vulkan::BackendFactoryOptions vulkan_options;
    vulkan_options.renderer.presentation =
        merlin::viewport::MakeGlfwVulkanPresentation(*window,
                                                     arguments.vsync);
    vulkan_options.shaders = {
        shader_dir / "triangle.vert.spv",
        shader_dir / "triangle.frag.spv",
        shader_dir / "triangle.bindless.vert.spv",
        shader_dir / "triangle.bindless.frag.spv"};
    merlin::vulkan::BackendFactory vulkan_factory(
        std::move(vulkan_options));
    std::vector<merlin::render::BackendFactory*> factories{&vulkan_factory};
    merlin::render::BackendCreateInfo create_info;
    create_info.backend = arguments.backend;
    create_info.enable_validation = arguments.validation;
    merlin::render::BackendSelection selection;
    auto backend =
        merlin::render::CreateBackend(create_info, factories, &selection);
    const auto presentation = backend->default_presentation_target();
    if (!presentation) {
      throw std::runtime_error(
          "selected backend does not provide external presentation");
    }

    auto scene = BuildScene();
    bool running = true;
    bool screenshot_pending = !arguments.screenshot.empty();
    std::filesystem::path screenshot_path = arguments.screenshot;
    std::optional<std::pair<std::int32_t, std::int32_t>> pick;
    std::uint32_t width = window->width();
    std::uint32_t height = window->height();
    std::uint64_t frames{};
    std::uint64_t gpu_ns{};
    std::uint64_t presented_readback_bytes{};
    std::uint64_t presentation_copy_bytes{};
    std::uint64_t zero_readback_frames{};
    std::uint64_t latest_frame_ns{};
    bool resized_for_test{};
    bool reference_checked{};
    const auto benchmark_start = Clock::now();
    auto title_update = benchmark_start;

    std::cout << "Selected backend: "
              << merlin::render::BackendKindName(selection.selected) << " ("
              << selection.reason << ")\n"
              << "Device: " << backend->capabilities().device_name << '\n'
              << "Presentation: GPU swapchain, CPU readback disabled by default\n";

    while (running &&
           (arguments.frame_limit == 0 || frames < arguments.frame_limit)) {
      merlin::viewport::Event event;
      bool camera_changed{};
      while (window->PollEvent(event)) {
        switch (event.type) {
          case merlin::viewport::EventType::Close:
            running = false;
            break;
          case merlin::viewport::EventType::Resize:
            width = event.width;
            height = event.height;
            if (width != 0 && height != 0) {
              backend->ResizePresentationTarget(*presentation, width, height);
            }
            break;
          case merlin::viewport::EventType::KeyDown:
            if (event.key == merlin::viewport::Key::Escape) {
              running = false;
            } else if (event.key == merlin::viewport::Key::Screenshot) {
              screenshot_pending = true;
              screenshot_path = "merlin-viewport.ppm";
            } else if (event.key == merlin::viewport::Key::Left) {
              scene.camera_descriptor.view.values[12] -= 0.05F;
              camera_changed = true;
            } else if (event.key == merlin::viewport::Key::Right) {
              scene.camera_descriptor.view.values[12] += 0.05F;
              camera_changed = true;
            } else if (event.key == merlin::viewport::Key::Up) {
              scene.camera_descriptor.view.values[13] += 0.05F;
              camera_changed = true;
            } else if (event.key == merlin::viewport::Key::Down) {
              scene.camera_descriptor.view.values[13] -= 0.05F;
              camera_changed = true;
            }
            break;
          case merlin::viewport::EventType::PointerDown:
            if (event.button == merlin::viewport::MouseButton::Left &&
                !event.modifiers.alt && !event.modifiers.super) {
              pick = std::pair{event.x, event.y};
            }
            break;
          case merlin::viewport::EventType::PointerUp:
            break;
          case merlin::viewport::EventType::PointerMove:
            break;
          case merlin::viewport::EventType::Scroll:
            break;
        }
      }
      if (!running) {
        break;
      }
      if (width == 0 || height == 0) {
        window->WaitForEvent();
        continue;
      }
      if (camera_changed) {
        scene.world.UpdateCamera(scene.camera, scene.camera_descriptor,
                                 merlin::ChangeAspect::Camera);
        const auto changes = scene.world.Commit();
        if (!changes.empty()) {
          scene.extractor.Apply(scene.world, changes);
        }
      }

      merlin::render::RenderRequest request;
      request.snapshot = scene.extractor.snapshot();
      request.width = width;
      request.height = height;
      request.presentation = *presentation;
      const bool check_reference =
          arguments.reference_check && !reference_checked && frames != 0;
      request.products = {
          {merlin::Aov::Color, screenshot_pending || check_reference}};
      if (check_reference) {
        request.products.push_back({merlin::Aov::Depth, true});
      }
      if (pick) {
        request.products.push_back({merlin::Aov::PrimId, true});
        request.products.push_back({merlin::Aov::InstanceId, true});
      }
      const auto frame_start = Clock::now();
      auto result = backend->Resolve(backend->Submit(request));
      latest_frame_ns = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                               frame_start)
              .count());
      gpu_ns += result.timings.gpu_execution_ns;
      presented_readback_bytes += result.telemetry.readback_bytes;
      presentation_copy_bytes += result.telemetry.presentation_copy_bytes;
      if (result.telemetry.cpu_readback_aov_count == 0 &&
          result.telemetry.readback_bytes == 0) {
        ++zero_readback_frames;
      }
      ++frames;

      if (check_reference) {
        auto reference_request = request;
        reference_request.presentation = {};
        const auto reference =
            backend->Resolve(backend->Submit(reference_request));
        if (result.color.pixels != reference.color.pixels ||
            result.depth.pixels != reference.depth.pixels ||
            result.telemetry.present_count != 1 ||
            reference.telemetry.present_count != 0) {
          throw std::runtime_error(
              "headless and viewport render products do not match");
        }
        reference_checked = true;
      }

      if (screenshot_pending) {
        WritePpm(screenshot_path, result.color);
        std::cout << "Screenshot: " << screenshot_path.string() << '\n';
        screenshot_pending = false;
      }
      if (pick) {
        const auto x = std::clamp(pick->first, 0,
                                  static_cast<std::int32_t>(width) - 1);
        const auto y = std::clamp(pick->second, 0,
                                  static_cast<std::int32_t>(height) - 1);
        const auto index = static_cast<std::size_t>(y) * width + x;
        std::cout << "Pick " << x << ',' << y << ": primId="
                  << result.prim_id.pixels.at(index) << " instanceId="
                  << result.instance_id.pixels.at(index) << '\n';
        pick.reset();
      }
      if (arguments.resize_test && !resized_for_test && frames == 2) {
        window->SetSize(width + 64U, height + 32U);
        resized_for_test = true;
      }

      const auto now = Clock::now();
      if (now - title_update >= std::chrono::milliseconds(250)) {
        window->SetTitle(WindowTitle(
            merlin::render::BackendKindName(selection.selected), width, height,
            latest_frame_ns));
        title_update = now;
      }
    }

    const auto elapsed_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                             benchmark_start)
            .count());
    const auto statistics = backend->statistics();
    if (!arguments.benchmark.empty()) {
      WriteBenchmark(arguments.benchmark, selection, statistics, frames,
                     elapsed_ns, gpu_ns, presented_readback_bytes,
                     presentation_copy_bytes, zero_readback_frames);
      std::cout << "Benchmark: " << arguments.benchmark.string() << '\n';
    }
    if (arguments.reference_check &&
        (!reference_checked || zero_readback_frames == 0)) {
      throw std::runtime_error("viewport reference check did not execute");
    }
    if (arguments.resize_test && statistics.presentation_recreates == 0) {
      throw std::runtime_error("viewport resize did not recreate the swapchain");
    }
    if (statistics.validation_messages != 0) {
      throw std::runtime_error("Vulkan validation reported viewport messages");
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "merlin-viewport: " << error.what() << '\n';
    return 1;
  }
}
