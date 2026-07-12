#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/vulkan/renderer.hpp>

#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using CpuClock = std::chrono::steady_clock;

struct Arguments {
  std::filesystem::path output;
  std::uint32_t width{512};
  std::uint32_t height{512};
  std::uint32_t steady_frames{30};
};

struct CpuTimings {
  std::uint64_t scene_update_ns{};
  std::uint64_t extraction_ns{};
  std::uint64_t upload_ns{};
  std::uint64_t command_recording_ns{};
  std::uint64_t readback_ns{};
  std::uint64_t total_frame_ns{};
};

struct Baseline {
  std::string name;
  std::uint32_t samples{1};
  CpuTimings timings;
  merlin::vulkan::FrameCounters counters;
};

struct SceneFixture {
  merlin::RenderWorld world;
  merlin::InstanceHandle instance;
};

std::uint64_t ElapsedNanoseconds(CpuClock::time_point start) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(CpuClock::now() - start)
          .count());
}

std::uint32_t ParseUnsigned(std::string_view text, std::string_view option) {
  std::uint32_t value{};
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      value == 0) {
    throw std::invalid_argument(std::string(option) +
                                " requires a positive integer");
  }
  return value;
}

Arguments ParseArguments(int argc, char** argv) {
  Arguments result;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    auto value = [&](std::string_view option) -> std::string_view {
      if (++i >= argc) {
        throw std::invalid_argument(std::string(option) + " requires a value");
      }
      return argv[i];
    };
    if (argument == "--output") {
      result.output = value(argument);
    } else if (argument == "--width") {
      result.width = ParseUnsigned(value(argument), argument);
    } else if (argument == "--height") {
      result.height = ParseUnsigned(value(argument), argument);
    } else if (argument == "--steady-frames") {
      result.steady_frames = ParseUnsigned(value(argument), argument);
    } else if (argument == "--help") {
      std::cout << "Usage: merlin-benchmark [--output FILE] [--width N] "
                   "[--height N] [--steady-frames N]\n";
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown option: " + std::string(argument));
    }
  }
  return result;
}

SceneFixture BuildScene() {
  SceneFixture fixture;
  merlin::MeshDescriptor mesh;
  mesh.label = "benchmark-triangle";
  mesh.positions = {{0.0F, -0.72F, 0.0F}, {0.72F, 0.62F, 0.0F},
                    {-0.72F, 0.62F, 0.0F}};
  mesh.indices = {0, 1, 2};
  const auto mesh_handle = fixture.world.CreateMesh(std::move(mesh));

  merlin::MaterialDescriptor material;
  material.label = "benchmark-material";
  material.base_color = {0.18F, 0.78F, 1.0F, 1.0F};
  const auto material_handle = fixture.world.CreateMaterial(std::move(material));

  merlin::InstanceDescriptor instance;
  instance.label = "benchmark-instance";
  instance.mesh = mesh_handle;
  instance.material = material_handle;
  fixture.instance = fixture.world.CreateInstance(std::move(instance));
  return fixture;
}

CpuTimings FromBackend(const merlin::vulkan::FrameCpuTimings& timings) {
  CpuTimings result;
  result.upload_ns = timings.upload_ns;
  result.command_recording_ns = timings.command_recording_ns;
  result.readback_ns = timings.readback_ns;
  return result;
}

std::uint64_t Median(std::vector<std::uint64_t> values) {
  const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2U);
  std::nth_element(values.begin(), middle, values.end());
  if ((values.size() & 1U) != 0U) {
    return *middle;
  }
  const auto lower = *std::max_element(values.begin(), middle);
  return lower + (*middle - lower) / 2U;
}

CpuTimings MedianTimings(const std::vector<CpuTimings>& values) {
  auto collect = [&](auto member) {
    std::vector<std::uint64_t> samples;
    samples.reserve(values.size());
    for (const auto& value : values) {
      samples.push_back(value.*member);
    }
    return Median(std::move(samples));
  };
  return {collect(&CpuTimings::scene_update_ns),
          collect(&CpuTimings::extraction_ns),
          collect(&CpuTimings::upload_ns),
          collect(&CpuTimings::command_recording_ns),
          collect(&CpuTimings::readback_ns),
          collect(&CpuTimings::total_frame_ns)};
}

bool SameCounters(const merlin::vulkan::FrameCounters& lhs,
                  const merlin::vulkan::FrameCounters& rhs) {
  return lhs.draw_count == rhs.draw_count &&
         lhs.triangle_count == rhs.triangle_count &&
         lhs.upload_bytes == rhs.upload_bytes &&
         lhs.readback_bytes == rhs.readback_bytes &&
         lhs.allocation_count == rhs.allocation_count &&
         lhs.buffer_allocation_count == rhs.buffer_allocation_count &&
         lhs.image_allocation_count == rhs.image_allocation_count &&
         lhs.pipeline_creation_count == rhs.pipeline_creation_count &&
         lhs.scene_cache_hits == rhs.scene_cache_hits &&
         lhs.scene_cache_misses == rhs.scene_cache_misses &&
         lhs.pipeline_cache_hits == rhs.pipeline_cache_hits &&
         lhs.pipeline_cache_misses == rhs.pipeline_cache_misses;
}

std::string VersionString(std::uint32_t version) {
  return std::to_string(VK_VERSION_MAJOR(version)) + "." +
         std::to_string(VK_VERSION_MINOR(version)) + "." +
         std::to_string(VK_VERSION_PATCH(version));
}

void JsonString(std::ostream& stream, std::string_view value) {
  stream << '"';
  for (const unsigned char character : value) {
    switch (character) {
      case '"': stream << "\\\""; break;
      case '\\': stream << "\\\\"; break;
      case '\b': stream << "\\b"; break;
      case '\f': stream << "\\f"; break;
      case '\n': stream << "\\n"; break;
      case '\r': stream << "\\r"; break;
      case '\t': stream << "\\t"; break;
      default:
        if (character < 0x20U) {
          constexpr char digits[] = "0123456789abcdef";
          stream << "\\u00" << digits[character >> 4U]
                 << digits[character & 0x0fU];
        } else {
          stream << character;
        }
    }
  }
  stream << '"';
}

void WriteBaseline(std::ostream& stream, const Baseline& baseline,
                   std::string_view indent) {
  stream << indent << "{\n" << indent << "  \"name\": ";
  JsonString(stream, baseline.name);
  const auto& timing = baseline.timings;
  const auto& count = baseline.counters;
  stream << ",\n" << indent << "  \"samples\": " << baseline.samples
         << ",\n" << indent << "  \"cpu_ns\": {\n"
         << indent << "    \"scene_update\": " << timing.scene_update_ns << ",\n"
         << indent << "    \"extraction\": " << timing.extraction_ns << ",\n"
         << indent << "    \"upload\": " << timing.upload_ns << ",\n"
         << indent << "    \"command_recording\": " << timing.command_recording_ns << ",\n"
         << indent << "    \"readback\": " << timing.readback_ns << ",\n"
         << indent << "    \"total_frame\": " << timing.total_frame_ns << "\n"
         << indent << "  },\n" << indent << "  \"counters\": {\n"
         << indent << "    \"draw_count\": " << count.draw_count << ",\n"
         << indent << "    \"triangle_count\": " << count.triangle_count << ",\n"
         << indent << "    \"upload_bytes\": " << count.upload_bytes << ",\n"
         << indent << "    \"readback_bytes\": " << count.readback_bytes << ",\n"
         << indent << "    \"allocation_count\": " << count.allocation_count << ",\n"
         << indent << "    \"buffer_allocation_count\": " << count.buffer_allocation_count << ",\n"
         << indent << "    \"image_allocation_count\": " << count.image_allocation_count << ",\n"
         << indent << "    \"pipeline_creation_count\": " << count.pipeline_creation_count << ",\n"
         << indent << "    \"scene_cache_hits\": " << count.scene_cache_hits << ",\n"
         << indent << "    \"scene_cache_misses\": " << count.scene_cache_misses << ",\n"
         << indent << "    \"pipeline_cache_hits\": " << count.pipeline_cache_hits << ",\n"
         << indent << "    \"pipeline_cache_misses\": " << count.pipeline_cache_misses << "\n"
         << indent << "  }\n" << indent << '}';
}

void WriteJson(std::ostream& stream, const Arguments& arguments,
               const merlin::vulkan::RendererCapabilities& capabilities,
               const std::vector<Baseline>& baselines) {
  stream << "{\n  \"schema\": \"merlin-benchmark/v1\",\n"
         << "  \"environment\": {\n    \"commit\": ";
  JsonString(stream, MERLIN_BENCHMARK_COMMIT);
  stream << ",\n    \"build_type\": ";
  JsonString(stream, MERLIN_BENCHMARK_BUILD_TYPE);
  stream << ",\n    \"compiler\": ";
  JsonString(stream, MERLIN_BENCHMARK_COMPILER);
  stream << ",\n    \"os\": ";
  JsonString(stream, MERLIN_BENCHMARK_OS);
  stream << ",\n    \"architecture\": ";
  JsonString(stream, MERLIN_BENCHMARK_ARCHITECTURE);
  stream << ",\n    \"gpu\": ";
  JsonString(stream, capabilities.device_name);
  stream << ",\n    \"driver\": {\n      \"name\": ";
  JsonString(stream, capabilities.driver_name);
  stream << ",\n      \"info\": ";
  JsonString(stream, capabilities.driver_info);
  stream << ",\n      \"version\": " << capabilities.driver_version
         << "\n    },\n    \"vulkan_api\": ";
  JsonString(stream, VersionString(capabilities.api_version));
  stream << ",\n    \"resolution\": {\n      \"width\": " << arguments.width
         << ",\n      \"height\": " << arguments.height
         << "\n    }\n  },\n  \"baselines\": [\n";
  for (std::size_t index = 0; index < baselines.size(); ++index) {
    WriteBaseline(stream, baselines[index], "    ");
    stream << (index + 1U == baselines.size() ? "\n" : ",\n");
  }
  stream << "  ]\n}\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto arguments = ParseArguments(argc, argv);
    const auto executable_dir = std::filesystem::absolute(argv[0]).parent_path();
    const merlin::vulkan::ShaderPaths shaders{
        executable_dir / "shaders" / "triangle.vert.spv",
        executable_dir / "shaders" / "triangle.frag.spv"};
    merlin::vulkan::Renderer renderer;
    merlin::extraction::SceneExtractor extractor;
    std::vector<Baseline> baselines;

    const auto first_start = CpuClock::now();
    const auto update_start = CpuClock::now();
    auto fixture = BuildScene();
    const auto changes = fixture.world.Commit();
    const auto first_update_ns = ElapsedNanoseconds(update_start);
    const auto extraction_start = CpuClock::now();
    extractor.Apply(fixture.world, changes);
    const auto first_extraction_ns = ElapsedNanoseconds(extraction_start);
    const auto first_result = renderer.Render(extractor.scene(), arguments.width,
                                              arguments.height, shaders);
    auto first_timings = FromBackend(first_result.cpu_timings);
    first_timings.scene_update_ns = first_update_ns;
    first_timings.extraction_ns = first_extraction_ns;
    first_timings.total_frame_ns = ElapsedNanoseconds(first_start);
    baselines.push_back({"first-frame", 1, first_timings,
                         first_result.counters});

    std::vector<CpuTimings> steady_timings;
    steady_timings.reserve(arguments.steady_frames);
    merlin::vulkan::FrameCounters steady_counters;
    for (std::uint32_t frame = 0; frame < arguments.steady_frames; ++frame) {
      const auto frame_start = CpuClock::now();
      const auto result = renderer.Render(extractor.scene(), arguments.width,
                                          arguments.height, shaders);
      auto timing = FromBackend(result.cpu_timings);
      timing.total_frame_ns = ElapsedNanoseconds(frame_start);
      steady_timings.push_back(timing);
      if (frame == 0) {
        steady_counters = result.counters;
      } else if (!SameCounters(steady_counters, result.counters)) {
        throw std::runtime_error("steady-state structural counters changed");
      }
    }
    baselines.push_back({"steady-state", arguments.steady_frames,
                         MedianTimings(steady_timings), steady_counters});

    const auto edit_start = CpuClock::now();
    const auto edit_update_start = CpuClock::now();
    auto instance = fixture.world.Get(fixture.instance);
    instance.transform.values[12] = 0.125F;
    fixture.world.UpdateInstance(fixture.instance, std::move(instance),
                                 merlin::ChangeAspect::Transform);
    const auto edit_changes = fixture.world.Commit();
    const auto edit_update_ns = ElapsedNanoseconds(edit_update_start);
    const auto edit_extraction_start = CpuClock::now();
    extractor.Apply(fixture.world, edit_changes);
    const auto edit_extraction_ns = ElapsedNanoseconds(edit_extraction_start);
    const auto edit_result = renderer.Render(extractor.scene(), arguments.width,
                                             arguments.height, shaders);
    auto edit_timings = FromBackend(edit_result.cpu_timings);
    edit_timings.scene_update_ns = edit_update_ns;
    edit_timings.extraction_ns = edit_extraction_ns;
    edit_timings.total_frame_ns = ElapsedNanoseconds(edit_start);
    baselines.push_back({"scene-edit", 1, edit_timings, edit_result.counters});

    if (arguments.output.empty()) {
      WriteJson(std::cout, arguments, renderer.capabilities(), baselines);
    } else {
      if (arguments.output.has_parent_path()) {
        std::filesystem::create_directories(arguments.output.parent_path());
      }
      std::ofstream stream(arguments.output, std::ios::binary);
      if (!stream) {
        throw std::runtime_error("could not create output: " +
                                 arguments.output.string());
      }
      WriteJson(stream, arguments, renderer.capabilities(), baselines);
      if (!stream) {
        throw std::runtime_error("could not write output: " +
                                 arguments.output.string());
      }
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "merlin-benchmark: " << error.what() << '\n';
    return 1;
  }
}
