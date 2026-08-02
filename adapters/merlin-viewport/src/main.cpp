#include "developer_ui.hpp"
#include "window.hpp"
#ifdef MERLIN_VIEWPORT_ENABLE_VULKAN
#include "presentation_glfw.hpp"
#endif
#ifdef MERLIN_VIEWPORT_ENABLE_METAL
#include "presentation_metal.hpp"
#endif
#ifdef MERLIN_VIEWPORT_ENABLE_HYDRA2
#include "hydra_scene.hpp"
#endif

#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/render/backend.hpp>
#ifdef MERLIN_VIEWPORT_ENABLE_METAL
#include <merlin/metal/backend.hpp>
#endif
#ifdef MERLIN_VIEWPORT_ENABLE_VULKAN
#include <merlin/vulkan/backend.hpp>
#include <merlin/vulkan/shader_abi.hpp>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Arguments {
  static constexpr std::uint64_t kDefaultMetalHeapBytes =
      64ULL * 1024ULL * 1024ULL;

  std::uint32_t width{1280};
  std::uint32_t height{720};
  std::uint64_t frame_limit{};
  std::uint64_t metal_heap_capacity_bytes{kDefaultMetalHeapBytes};
  merlin::render::BackendRequest backend{
      merlin::render::BackendRequest::Automatic};
  bool validation{};
  bool vsync{true};
  bool visible{true};
  bool reference_check{};
  bool resize_test{};
  bool allow_unavailable{};
  std::filesystem::path screenshot;
  std::filesystem::path benchmark;
  std::filesystem::path usd;
};

std::uint64_t ReadUnsigned(std::string_view value, std::string_view name) {
  // std::stoull would accept and wrap a leading minus sign.
  if (value.empty() || value.front() < '0' || value.front() > '9') {
    throw std::invalid_argument(std::string(name) +
                                " must be a non-negative integer");
  }
  std::size_t consumed{};
  std::uint64_t result{};
  try {
    result = std::stoull(std::string(value), &consumed);
  } catch (const std::exception&) {
    throw std::invalid_argument(std::string(name) +
                                " must be a non-negative integer");
  }
  if (consumed != value.size()) {
    throw std::invalid_argument(std::string(name) +
                                " must be a non-negative integer");
  }
  return result;
}

std::uint32_t ReadUnsigned32(std::string_view value, std::string_view name) {
  const auto result = ReadUnsigned(value, name);
  if (result > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument(std::string(name) + " is out of range");
  }
  return static_cast<std::uint32_t>(result);
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
      result.width = ReadUnsigned32(next(), option);
    } else if (option == "--height") {
      result.height = ReadUnsigned32(next(), option);
    } else if (option == "--frames") {
      result.frame_limit = ReadUnsigned(next(), option);
    } else if (option == "--metal-heap-mib") {
      constexpr std::uint64_t bytes_per_mib = 1024ULL * 1024ULL;
      const auto capacity_mib = ReadUnsigned(next(), option);
      if (capacity_mib == 0 ||
          capacity_mib >
              std::numeric_limits<std::uint64_t>::max() / bytes_per_mib) {
        throw std::invalid_argument(
            "--metal-heap-mib must be a positive in-range integer");
      }
      result.metal_heap_capacity_bytes = capacity_mib * bytes_per_mib;
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
    } else if (option == "--allow-unavailable") {
      result.allow_unavailable = true;
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
             "  --metal-heap-mib N (default 64)\n"
             "  --frames N --benchmark report.json --screenshot image.ppm\n"
             "  --usd scene.usd --hidden --reference-check --resize-test\n"
             "  --allow-unavailable (capability-test skip)\n"
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

merlin::viewport::DeveloperUiGaussian SummarizeGaussians(
    const merlin::extraction::FrameSnapshot& snapshot) {
  merlin::viewport::DeveloperUiGaussian result;
  result.available = true;
  result.resources = snapshot.gaussians.size();
  for (const auto& gaussian : snapshot.gaussians) {
    if (gaussian.positions) {
      result.particles += gaussian.positions->size();
    }
    if (gaussian.visible) {
      ++result.visible_resources;
    }
    const auto degree = std::min<std::size_t>(
        gaussian.spherical_harmonics_degree,
        result.spherical_harmonics_degree_resources.size() - 1U);
    ++result.spherical_harmonics_degree_resources[degree];
    if (gaussian.projection_mode ==
        merlin::GaussianProjectionMode::Tangential) {
      ++result.tangential_resources;
    } else {
      ++result.perspective_resources;
    }
    if (gaussian.sorting_mode ==
        merlin::GaussianSortingMode::CameraDistance) {
      ++result.camera_distance_resources;
    } else {
      ++result.z_depth_resources;
    }
  }
  return result;
}

#ifdef MERLIN_VIEWPORT_ENABLE_HYDRA2
merlin::viewport::HydraViewportOptions MakeHydraViewportOptions(
    const Arguments& arguments, const std::filesystem::path& executable,
    std::filesystem::path stage) {
  merlin::viewport::HydraViewportOptions options;
  options.stage = std::move(stage);
  options.executable = executable;
  options.screenshot = arguments.screenshot;
  options.benchmark = arguments.benchmark;
  options.width = arguments.width;
  options.height = arguments.height;
  options.frame_limit = arguments.frame_limit;
  options.metal_heap_capacity_bytes = arguments.metal_heap_capacity_bytes;
  options.backend = arguments.backend;
  options.validation = arguments.validation;
  options.vsync = arguments.vsync;
  options.visible = arguments.visible;
  options.reference_check = arguments.reference_check;
  options.resize_test = arguments.resize_test;
  return options;
}
#endif

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
                    std::uint64_t zero_readback_frames,
                    std::uint64_t generated_material_draws,
                    std::uint64_t generated_material_fallbacks,
                    merlin::MaterialFallback effective_material_fallback) {
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
         << "  \"generated_material_draws\": "
         << generated_material_draws << ",\n"
         << "  \"generated_material_fallbacks\": "
         << generated_material_fallbacks << ",\n"
         << "  \"material_effective_fallback\": \""
         << merlin::MaterialFallbackName(effective_material_fallback)
         << "\",\n"
         << "  \"frames_presented\": " << statistics.frames_presented
         << ",\n"
         << "  \"presentation_recreates\": "
         << statistics.presentation_recreates << ",\n"
         << "  \"validation_messages\": "
         << statistics.validation_messages << "\n"
         << "}\n";
}

merlin::viewport::DeveloperUiBenchmark MakeUiBenchmark(
    std::uint64_t frames, std::uint64_t elapsed_ns, std::uint64_t gpu_ns,
    std::filesystem::path path = {}) {
  merlin::viewport::DeveloperUiBenchmark result;
  result.available = frames != 0;
  result.path = std::move(path);
  result.frames = frames;
  if (frames != 0) {
    result.cpu_average_frame_ns = elapsed_ns / frames;
    result.gpu_average_frame_ns = gpu_ns / frames;
  }
  return result;
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
  bool allow_unavailable{};
  try {
    const auto arguments = ParseArguments(argc, argv);
    allow_unavailable = arguments.allow_unavailable;
    if (!arguments.usd.empty()) {
#ifdef MERLIN_VIEWPORT_ENABLE_HYDRA2
      return merlin::viewport::RunHydraViewport(MakeHydraViewportOptions(
          arguments, argv[0], arguments.usd));
#else
      throw std::runtime_error(
          "this build does not include the Hydra viewport scene source; "
          "configure with MERLIN_ENABLE_HYDRA2=ON");
#endif
    }

    auto window = merlin::viewport::Window::Create(
        "merlin-viewport", arguments.width, arguments.height,
        arguments.visible);
    auto developer_ui = merlin::viewport::DeveloperUi::Create(*window);
    std::vector<merlin::render::BackendFactory*> factories;
#ifdef MERLIN_VIEWPORT_ENABLE_VULKAN
    std::unique_ptr<merlin::vulkan::BackendFactory> vulkan_factory;
    if (arguments.backend != merlin::render::BackendRequest::Metal) {
      const auto executable_dir =
          std::filesystem::absolute(argv[0]).parent_path();
      const auto shader_dir =
          executable_dir / merlin::vulkan::shader_abi::ArtifactDirectory();
      merlin::vulkan::BackendFactoryOptions vulkan_options;
      auto presentation_options =
          merlin::viewport::MakeGlfwVulkanPresentation(*window,
                                                       arguments.vsync);
      developer_ui->ConfigurePresentation(presentation_options);
      vulkan_options.renderer.presentation =
          std::move(presentation_options);
      vulkan_options.shaders = {
          shader_dir / "triangle.vert.spv",
          shader_dir / "triangle.frag.spv",
          shader_dir / "triangle.bindless.vert.spv",
          shader_dir / "triangle.bindless.frag.spv",
          shader_dir / "environment.hdr",
          shader_dir / "gaussian.vert.spv",
          shader_dir / "gaussian.frag.spv"};
      vulkan_factory =
          std::make_unique<merlin::vulkan::BackendFactory>(
              std::move(vulkan_options));
      factories.push_back(vulkan_factory.get());
    }
#endif
#ifdef MERLIN_VIEWPORT_ENABLE_METAL
    std::unique_ptr<merlin::metal::BackendFactory> metal_factory;
    if (arguments.backend != merlin::render::BackendRequest::Vulkan) {
      merlin::metal::BackendOptions metal_options;
      metal_options.heap_capacity_bytes =
          arguments.metal_heap_capacity_bytes;
      auto metal_presentation =
          merlin::viewport::MakeGlfwMetalPresentation(*window,
                                                      arguments.vsync);
      developer_ui->ConfigurePresentation(metal_presentation);
      metal_options.presentation = std::move(metal_presentation);
      metal_factory = std::make_unique<merlin::metal::BackendFactory>(
          std::move(metal_options));
      factories.push_back(metal_factory.get());
    }
#endif
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
    bool benchmark_snapshot_pending{};
    std::filesystem::path screenshot_path = arguments.screenshot;
    std::optional<std::pair<std::int32_t, std::int32_t>> pick;
    std::uint32_t width = window->width();
    std::uint32_t height = window->height();
    std::uint64_t frames{};
    std::uint64_t gpu_ns{};
    std::uint64_t presented_readback_bytes{};
    std::uint64_t presentation_copy_bytes{};
    std::uint64_t zero_readback_frames{};
    std::uint64_t generated_material_draws{};
    std::uint64_t generated_material_fallbacks{};
    merlin::MaterialFallback effective_material_fallback{
        merlin::MaterialFallback::None};
    std::uint64_t latest_frame_ns{};
    merlin::render::FrameTimings latest_timings;
    merlin::render::FrameTelemetry latest_telemetry;
    std::vector<merlin::MaterialDiagnostic> latest_material_diagnostics;
    merlin::viewport::DeveloperUiRendererSettings renderer_settings;
    renderer_settings.available = true;
    merlin::viewport::DeveloperUiSettingsFeedback settings_feedback;
    std::optional<merlin::viewport::DeveloperUiBenchmark> saved_benchmark;
#ifdef MERLIN_VIEWPORT_ENABLE_HYDRA2
    std::optional<std::filesystem::path> selected_usd;
#endif
    bool resized_for_test{};
    bool reference_checked{};
    const auto benchmark_start = Clock::now();
    auto comparison_start = benchmark_start;
    std::uint64_t comparison_start_frame{};
    std::uint64_t comparison_start_gpu_ns{};
    auto title_update = benchmark_start;

    std::cout << "Selected backend: "
              << merlin::render::BackendKindName(selection.selected) << " ("
              << selection.reason << ")\n"
              << "Device: " << backend->capabilities().device_name << '\n'
              << "Presentation: native GPU path, CPU readback disabled by default\n";

    while (running &&
           (arguments.frame_limit == 0 || frames < arguments.frame_limit)) {
      merlin::viewport::Event event;
      bool camera_changed{};
      while (window->PollEvent(event)) {
        const bool wants_keyboard = developer_ui->WantsKeyboard();
        const bool wants_mouse = developer_ui->WantsMouse();
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
            } else if (!wants_keyboard &&
                       event.key == merlin::viewport::Key::Screenshot) {
              screenshot_pending = true;
              screenshot_path = "merlin-viewport.ppm";
            } else if (!wants_keyboard &&
                       event.key == merlin::viewport::Key::Left) {
              scene.camera_descriptor.view.values[12] -= 0.05F;
              camera_changed = true;
            } else if (!wants_keyboard &&
                       event.key == merlin::viewport::Key::Right) {
              scene.camera_descriptor.view.values[12] += 0.05F;
              camera_changed = true;
            } else if (!wants_keyboard &&
                       event.key == merlin::viewport::Key::Up) {
              scene.camera_descriptor.view.values[13] += 0.05F;
              camera_changed = true;
            } else if (!wants_keyboard &&
                       event.key == merlin::viewport::Key::Down) {
              scene.camera_descriptor.view.values[13] -= 0.05F;
              camera_changed = true;
            }
            break;
          case merlin::viewport::EventType::PointerDown:
            if (!wants_mouse &&
                event.button == merlin::viewport::MouseButton::Left &&
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

      const auto scene_snapshot = scene.extractor.snapshot();
      merlin::viewport::DeveloperUiSnapshot ui_snapshot;
      ui_snapshot.scene_source = "native";
      ui_snapshot.selection = &selection;
      ui_snapshot.capabilities = &backend->capabilities();
      ui_snapshot.statistics = backend->statistics();
      ui_snapshot.timings = latest_timings;
      ui_snapshot.telemetry = latest_telemetry;
      ui_snapshot.material_diagnostics = &latest_material_diagnostics;
      ui_snapshot.scene = {
          true,
          scene_snapshot->geometries.size(),
          scene_snapshot->gaussians.size(),
          scene_snapshot->textures.size(),
          scene_snapshot->samplers.size(),
          scene_snapshot->materials.size(),
          scene_snapshot->instances.size(),
          scene_snapshot->lights.size(),
      };
      ui_snapshot.gaussian = SummarizeGaussians(*scene_snapshot);
      const auto benchmark_elapsed_ns = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              Clock::now() - comparison_start)
              .count());
      ui_snapshot.benchmark = MakeUiBenchmark(
          frames - comparison_start_frame, benchmark_elapsed_ns,
          gpu_ns - comparison_start_gpu_ns);
      ui_snapshot.saved_benchmark =
          saved_benchmark ? &*saved_benchmark : nullptr;
      ui_snapshot.renderer_settings = renderer_settings;
      ui_snapshot.settings_feedback = &settings_feedback;
      const auto& view = scene.camera_descriptor.view.values;
      ui_snapshot.camera.available = true;
      ui_snapshot.camera.controller = "translation";
      ui_snapshot.camera.up_axis = "Y";
      ui_snapshot.camera.position = {
          -static_cast<double>(view[12]), -static_cast<double>(view[13]),
          -static_cast<double>(view[14])};
      ui_snapshot.camera.target = {
          ui_snapshot.camera.position[0], ui_snapshot.camera.position[1],
          ui_snapshot.camera.position[2] - 1.0};
      ui_snapshot.camera.distance = 1.0;
      ui_snapshot.camera.aspect_ratio =
          static_cast<double>(width) / static_cast<double>(height);
#ifdef MERLIN_VIEWPORT_ENABLE_HYDRA2
      ui_snapshot.can_load_usd = true;
#endif
      ui_snapshot.frame_index = frames;
      ui_snapshot.host_frame_ns = latest_frame_ns;
      ui_snapshot.width = width;
      ui_snapshot.height = height;
      const auto ui_actions = developer_ui->DrawFrame(ui_snapshot);
      if (ui_actions.capture_screenshot) {
        screenshot_pending = true;
        screenshot_path = "merlin-viewport.ppm";
      }
#ifdef MERLIN_VIEWPORT_ENABLE_HYDRA2
      if (ui_actions.load_usd) {
        selected_usd = *ui_actions.load_usd;
        running = false;
        continue;
      }
#endif
      benchmark_snapshot_pending =
          benchmark_snapshot_pending || ui_actions.save_benchmark;
      if (ui_actions.apply_renderer_settings) {
        (void)merlin::viewport::ApplyDeveloperUiRendererSettings(
            *ui_actions.apply_renderer_settings, backend->capabilities(),
            renderer_settings, settings_feedback);
      }

      merlin::render::RenderRequest request;
      request.snapshot = scene_snapshot;
      request.width = width;
      request.height = height;
      request.presentation = *presentation;
      request.clear_color = {
          renderer_settings.clear_color[0], renderer_settings.clear_color[1],
          renderer_settings.clear_color[2], renderer_settings.clear_color[3]};
      const bool check_reference =
          arguments.reference_check && !reference_checked && frames != 0;
      request.products = {
          {merlin::Aov::Color,
           renderer_settings.continuous_color_readback ||
               screenshot_pending || check_reference}};
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
      latest_timings = result.timings;
      latest_telemetry = result.telemetry;
      latest_material_diagnostics = result.material_diagnostics;
      gpu_ns += result.timings.gpu_execution_ns;
      presented_readback_bytes += result.telemetry.readback_bytes;
      presentation_copy_bytes += result.telemetry.presentation_copy_bytes;
      generated_material_draws +=
          result.telemetry.generated_material_draw_count;
      generated_material_fallbacks +=
          result.telemetry.generated_material_fallback_count;
      effective_material_fallback =
          std::max(effective_material_fallback,
                   result.telemetry.material_fallbacks.effective_fallback);
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
      if (benchmark_snapshot_pending) {
        const auto benchmark_now = Clock::now();
        const auto elapsed_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                benchmark_now - benchmark_start)
                .count());
        const auto comparison_elapsed_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                benchmark_now - comparison_start)
                .count());
        const std::filesystem::path path = "merlin-viewport-benchmark.json";
        WriteBenchmark(path, selection, backend->statistics(), frames,
                       elapsed_ns, gpu_ns, presented_readback_bytes,
                       presentation_copy_bytes, zero_readback_frames,
                       generated_material_draws,
                       generated_material_fallbacks,
                       effective_material_fallback);
        saved_benchmark = MakeUiBenchmark(
            frames - comparison_start_frame, comparison_elapsed_ns,
            gpu_ns - comparison_start_gpu_ns, path);
        std::cout << "Benchmark snapshot: " << path.string() << '\n';
        benchmark_snapshot_pending = false;
        comparison_start = Clock::now();
        comparison_start_frame = frames;
        comparison_start_gpu_ns = gpu_ns;
      }

      const auto now = Clock::now();
      if (now - title_update >= std::chrono::milliseconds(250)) {
        window->SetTitle(WindowTitle(
            merlin::render::BackendKindName(selection.selected), width, height,
            latest_frame_ns));
        title_update = now;
      }
    }

#ifdef MERLIN_VIEWPORT_ENABLE_HYDRA2
    if (selected_usd) {
      backend.reset();
#ifdef MERLIN_VIEWPORT_ENABLE_VULKAN
      vulkan_factory.reset();
#endif
#ifdef MERLIN_VIEWPORT_ENABLE_METAL
      metal_factory.reset();
#endif
      developer_ui.reset();
      window.reset();
      return merlin::viewport::RunHydraViewport(MakeHydraViewportOptions(
          arguments, argv[0], std::move(*selected_usd)));
    }
#endif

    const auto elapsed_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                             benchmark_start)
            .count());
    const auto statistics = backend->statistics();
    if (!arguments.benchmark.empty()) {
      WriteBenchmark(arguments.benchmark, selection, statistics, frames,
                     elapsed_ns, gpu_ns, presented_readback_bytes,
                     presentation_copy_bytes, zero_readback_frames,
                     generated_material_draws,
                     generated_material_fallbacks,
                     effective_material_fallback);
      std::cout << "Benchmark: " << arguments.benchmark.string() << '\n';
    }
    if (arguments.reference_check &&
        (!reference_checked || zero_readback_frames == 0)) {
      throw std::runtime_error("viewport reference check did not execute");
    }
    if (arguments.reference_check &&
        (statistics.frames_presented != frames ||
         statistics.presentation_copy_bytes == 0)) {
      throw std::runtime_error(
          "viewport did not present every frame through the native GPU path");
    }
    if (arguments.resize_test && statistics.presentation_recreates == 0) {
      throw std::runtime_error(
          "viewport resize did not update the presentation target");
    }
    if (statistics.validation_messages != 0) {
      throw std::runtime_error(
          "backend validation reported viewport messages");
    }
    return 0;
  } catch (const merlin::render::RendererError& error) {
    if (allow_unavailable &&
        error.code() ==
            merlin::render::RendererErrorCode::BackendUnavailable) {
      std::cerr << "skip: " << error.what() << '\n';
      return 77;
    }
    std::cerr << "merlin-viewport: " << error.what() << '\n';
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "merlin-viewport: " << error.what() << '\n';
    return 1;
  }
}
