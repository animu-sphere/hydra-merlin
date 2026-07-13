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

// The fixture covers the v0.2.0 resource-granular contract: two meshes, two
// materials, and three instances where two instances share one mesh.
struct SceneFixture {
  merlin::RenderWorld world;
  merlin::MeshHandle triangle;
  merlin::MeshHandle quad;
  merlin::MaterialHandle primary_material;
  merlin::MaterialHandle secondary_material;
  merlin::InstanceHandle first_triangle;
  merlin::InstanceHandle second_triangle;
  merlin::InstanceHandle quad_instance;
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

void PopulateScene(SceneFixture& fixture) {
  merlin::MeshDescriptor triangle;
  triangle.label = "benchmark-triangle";
  triangle.positions = {{0.0F, -0.72F, 0.0F}, {0.72F, 0.62F, 0.0F},
                        {-0.72F, 0.62F, 0.0F}};
  triangle.indices = {0, 1, 2};
  fixture.triangle = fixture.world.CreateMesh(std::move(triangle));

  merlin::MeshDescriptor quad;
  quad.label = "benchmark-quad";
  quad.positions = {{-0.95F, -0.95F, 0.5F}, {-0.45F, -0.95F, 0.5F},
                    {-0.45F, -0.45F, 0.5F}, {-0.95F, -0.45F, 0.5F}};
  quad.indices = {0, 1, 2, 0, 2, 3};
  fixture.quad = fixture.world.CreateMesh(std::move(quad));

  merlin::MaterialDescriptor material;
  material.label = "benchmark-primary";
  material.parameters.base_color = {0.18F, 0.78F, 1.0F, 1.0F};
  fixture.primary_material = fixture.world.CreateMaterial(material);
  material.label = "benchmark-secondary";
  material.parameters.base_color = {1.0F, 0.55F, 0.12F, 1.0F};
  fixture.secondary_material = fixture.world.CreateMaterial(std::move(material));

  merlin::InstanceDescriptor instance;
  instance.label = "benchmark-first-triangle";
  instance.mesh = fixture.triangle;
  instance.material = fixture.primary_material;
  fixture.first_triangle = fixture.world.CreateInstance(instance);

  instance.label = "benchmark-second-triangle";
  instance.material = fixture.secondary_material;
  instance.transform.values[12] = 0.22F;
  fixture.second_triangle = fixture.world.CreateInstance(instance);

  instance.label = "benchmark-quad-instance";
  instance.mesh = fixture.quad;
  instance.material = fixture.primary_material;
  instance.transform = {};
  fixture.quad_instance = fixture.world.CreateInstance(std::move(instance));
}

CpuTimings FromBackend(const merlin::vulkan::FrameCpuTimings& timings) {
  CpuTimings result;
  result.upload_ns = timings.upload_ns;
  result.command_recording_ns = timings.command_recording_ns;
  result.readback_ns = timings.readback_ns;
  return result;
}

std::uint64_t Median(std::vector<std::uint64_t> values) {
  if (values.empty()) {
    return 0;
  }
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
         << indent << "    \"geometry_cache_hits\": " << count.geometry_cache_hits << ",\n"
         << indent << "    \"geometry_cache_misses\": " << count.geometry_cache_misses << ",\n"
         << indent << "    \"texture_cache_hits\": " << count.texture_cache_hits << ",\n"
         << indent << "    \"texture_cache_misses\": " << count.texture_cache_misses << ",\n"
         << indent << "    \"sampler_cache_hits\": " << count.sampler_cache_hits << ",\n"
         << indent << "    \"sampler_cache_misses\": " << count.sampler_cache_misses << ",\n"
         << indent << "    \"buffer_suballocation_count\": " << count.buffer_suballocation_count << ",\n"
         << indent << "    \"buffer_range_release_count\": " << count.buffer_range_release_count << ",\n"
         << indent << "    \"pipeline_cache_hits\": " << count.pipeline_cache_hits << ",\n"
         << indent << "    \"pipeline_cache_misses\": " << count.pipeline_cache_misses << ",\n"
         << indent << "    \"shader_module_cache_hits\": " << count.shader_module_cache_hits << ",\n"
         << indent << "    \"shader_module_cache_misses\": " << count.shader_module_cache_misses << ",\n"
         << indent << "    \"descriptor_layout_cache_hits\": " << count.descriptor_layout_cache_hits << ",\n"
         << indent << "    \"descriptor_layout_cache_misses\": " << count.descriptor_layout_cache_misses << ",\n"
         << indent << "    \"descriptor_update_count\": " << count.descriptor_update_count << "\n"
         << indent << "  }\n" << indent << '}';
}

void WriteJson(std::ostream& stream, const Arguments& arguments,
               const merlin::vulkan::RendererCapabilities& capabilities,
               const std::vector<Baseline>& baselines) {
  stream << "{\n  \"schema\": \"merlin-benchmark/v2\",\n"
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

    const auto render = [&] {
      merlin::vulkan::RenderRequest request;
      request.snapshot = extractor.snapshot();
      request.width = arguments.width;
      request.height = arguments.height;
      request.shaders = shaders;
      request.products = {{merlin::Aov::Color, true},
                          {merlin::Aov::Depth, true},
                          {merlin::Aov::PrimId, true},
                          {merlin::Aov::InstanceId, true}};
      return renderer.Resolve(renderer.Submit(request));
    };

    // Measures one edit scenario: the RenderWorld mutation, commit,
    // incremental extraction, and one rendered frame.
    const auto measure = [&](std::string name, merlin::RenderWorld& world,
                             const auto& edit) {
      const auto start = CpuClock::now();
      const auto update_start = CpuClock::now();
      edit();
      const auto changes = world.Commit();
      const auto update_ns = ElapsedNanoseconds(update_start);
      const auto extraction_start = CpuClock::now();
      extractor.Apply(world, changes);
      const auto extraction_ns = ElapsedNanoseconds(extraction_start);
      const auto result = render();
      auto timings = FromBackend(result.cpu_timings);
      timings.scene_update_ns = update_ns;
      timings.extraction_ns = extraction_ns;
      timings.total_frame_ns = ElapsedNanoseconds(start);
      baselines.push_back({std::move(name), 1, timings, result.counters});
      return result;
    };

    SceneFixture fixture;
    measure("first-frame", fixture.world, [&] { PopulateScene(fixture); });

    std::vector<CpuTimings> steady_timings;
    steady_timings.reserve(arguments.steady_frames);
    merlin::vulkan::FrameCounters steady_counters;
    for (std::uint32_t frame = 0; frame < arguments.steady_frames; ++frame) {
      const auto frame_start = CpuClock::now();
      const auto result = render();
      auto timing = FromBackend(result.cpu_timings);
      timing.total_frame_ns = ElapsedNanoseconds(frame_start);
      steady_timings.push_back(timing);
      if (frame == 0) {
        steady_counters = result.counters;
      } else if (result.counters != steady_counters) {
        throw std::runtime_error("steady-state structural counters changed");
      }
    }
    if (steady_counters.upload_bytes != 0 ||
        steady_counters.allocation_count != 0 ||
        steady_counters.pipeline_creation_count != 0) {
      throw std::runtime_error(
          "static steady-state frames performed upload/allocation/pipeline work");
    }
    baselines.push_back({"steady-state", arguments.steady_frames,
                         MedianTimings(steady_timings), steady_counters});

    measure("edit-transform", fixture.world, [&] {
      auto instance = fixture.world.Get(fixture.second_triangle);
      instance.transform.values[12] = -0.31F;
      fixture.world.UpdateInstance(fixture.second_triangle,
                                   std::move(instance),
                                   merlin::ChangeAspect::Transform);
    });

    measure("edit-visibility", fixture.world, [&] {
      auto instance = fixture.world.Get(fixture.quad_instance);
      instance.visible = false;
      fixture.world.UpdateInstance(fixture.quad_instance, std::move(instance),
                                   merlin::ChangeAspect::Visibility);
    });
    {
      // Unmeasured restore keeps later scenarios on the full three-draw scene.
      auto instance = fixture.world.Get(fixture.quad_instance);
      instance.visible = true;
      fixture.world.UpdateInstance(fixture.quad_instance, std::move(instance),
                                   merlin::ChangeAspect::Visibility);
      extractor.Apply(fixture.world, fixture.world.Commit());
      (void)render();
    }

    measure("edit-material", fixture.world, [&] {
      auto material = fixture.world.Get(fixture.primary_material);
      material.parameters.base_color = {0.35F, 0.92F, 0.35F, 1.0F};
      fixture.world.UpdateMaterial(fixture.primary_material,
                                   std::move(material));
    });

    measure("edit-points", fixture.world, [&] {
      auto mesh = fixture.world.Get(fixture.triangle);
      mesh.positions[0].y = -0.6F;
      fixture.world.UpdateMesh(fixture.triangle, std::move(mesh),
                               merlin::ChangeAspect::Points);
    });

    measure("remove-mesh", fixture.world, [&] {
      fixture.world.Remove(fixture.quad_instance);
      fixture.world.Remove(fixture.quad);
    });

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
