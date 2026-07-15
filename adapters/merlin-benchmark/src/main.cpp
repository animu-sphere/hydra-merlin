#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/vulkan/renderer.hpp>

#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using CpuClock = std::chrono::steady_clock;

constexpr std::uint64_t kHitchFloorNanoseconds = 2'000'000;

struct Arguments {
  std::filesystem::path output;
  std::string fixture{"reference"};
  std::uint32_t width{512};
  std::uint32_t height{512};
  std::uint32_t steady_frames{30};
  bool resolution_overridden{};
};

struct FrameTimings {
  std::uint64_t scene_update_ns{};
  std::uint64_t extraction_ns{};
  std::uint64_t gpu_scene_update_ns{};
  std::uint64_t command_recording_ns{};
  std::uint64_t queue_submission_ns{};
  std::uint64_t completion_wait_ns{};
  std::uint64_t readback_ns{};
  std::uint64_t gpu_execution_ns{};
  std::uint64_t total_frame_ns{};
};

struct Distribution {
  std::uint64_t median{};
  std::uint64_t p95{};
  std::uint64_t p99{};
  std::uint64_t maximum{};
};

struct Baseline {
  std::string name;
  std::vector<FrameTimings> timings;
  merlin::vulkan::FrameCounters counters;
  merlin::extraction::SnapshotBuildCounters snapshot_build_counters;
};

struct FixtureSummary {
  std::string name;
  std::uint64_t mesh_count{};
  std::uint64_t instance_count{};
  std::uint64_t triangle_count{};
};

// The reference fixture covers shared geometry, independent materials, a
// camera-only update, every resource edit class, and AOV readback combinations.
struct SceneFixture {
  merlin::RenderWorld world;
  merlin::MeshHandle triangle;
  merlin::MeshHandle quad;
  merlin::MaterialHandle primary_material;
  merlin::MaterialHandle secondary_material;
  merlin::InstanceHandle first_triangle;
  merlin::InstanceHandle second_triangle;
  merlin::InstanceHandle quad_instance;
  merlin::CameraHandle camera;
};

struct ScaleFixture {
  merlin::RenderWorld world;
  merlin::MaterialHandle material;
  std::vector<merlin::MeshHandle> meshes;
  std::vector<merlin::InstanceHandle> instances;
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

bool IsFixture(std::string_view value) {
  constexpr std::array fixtures{
      std::string_view("reference"), std::string_view("million-triangles"),
      std::string_view("ten-thousand-meshes"),
      std::string_view("thousand-instances"),
      std::string_view("aov-combinations"), std::string_view("4k")};
  return std::find(fixtures.begin(), fixtures.end(), value) != fixtures.end();
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
    } else if (argument == "--fixture") {
      const auto selected = value(argument);
      if (!IsFixture(selected)) {
        throw std::invalid_argument("unsupported fixture: " +
                                    std::string(selected));
      }
      result.fixture = selected;
    } else if (argument == "--width") {
      result.width = ParseUnsigned(value(argument), argument);
      result.resolution_overridden = true;
    } else if (argument == "--height") {
      result.height = ParseUnsigned(value(argument), argument);
      result.resolution_overridden = true;
    } else if (argument == "--steady-frames") {
      result.steady_frames = ParseUnsigned(value(argument), argument);
    } else if (argument == "--help") {
      std::cout
          << "Usage: merlin-benchmark [--output FILE] [--fixture NAME] "
             "[--width N] [--height N] [--steady-frames N]\n"
             "Fixtures: reference, million-triangles, ten-thousand-meshes, "
             "thousand-instances, aov-combinations, 4k\n";
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown option: " + std::string(argument));
    }
  }
  if (result.fixture == "4k" && !result.resolution_overridden) {
    result.width = 3840;
    result.height = 2160;
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

  merlin::CameraDescriptor camera;
  camera.label = "benchmark-camera";
  fixture.camera = fixture.world.CreateCamera(std::move(camera));
}

FixtureSummary PopulateScaleFixture(std::string_view name,
                                    ScaleFixture& fixture) {
  merlin::MaterialDescriptor material;
  material.label = "scale-material";
  material.parameters.base_color = {0.18F, 0.78F, 1.0F, 1.0F};
  fixture.material = fixture.world.CreateMaterial(std::move(material));

  auto make_mesh = [](std::string label) {
    merlin::MeshDescriptor mesh;
    mesh.label = std::move(label);
    mesh.positions = {{-0.01F, -0.01F, 0.0F}, {0.01F, -0.01F, 0.0F},
                      {0.0F, 0.01F, 0.0F}};
    mesh.indices = {0, 1, 2};
    return mesh;
  };
  auto add_instance = [&](merlin::MeshHandle mesh, std::uint32_t index) {
    merlin::InstanceDescriptor instance;
    instance.label = "scale-instance-" + std::to_string(index);
    instance.mesh = mesh;
    instance.material = fixture.material;
    const auto column = index % 100U;
    const auto row = (index / 100U) % 100U;
    instance.transform.values[12] = static_cast<float>(column) * 0.018F - 0.9F;
    instance.transform.values[13] = static_cast<float>(row) * 0.018F - 0.9F;
    fixture.instances.push_back(fixture.world.CreateInstance(std::move(instance)));
  };

  FixtureSummary summary;
  summary.name = name;
  if (name == "million-triangles") {
    auto mesh = make_mesh("million-triangle-mesh");
    mesh.indices.resize(3'000'000);
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
      mesh.indices[i] = 0;
      mesh.indices[i + 1] = 1;
      mesh.indices[i + 2] = 2;
    }
    fixture.meshes.push_back(fixture.world.CreateMesh(std::move(mesh)));
    add_instance(fixture.meshes.front(), 0);
    summary = {std::string(name), 1, 1, 1'000'000};
  } else if (name == "ten-thousand-meshes") {
    fixture.meshes.reserve(10'000);
    fixture.instances.reserve(10'000);
    for (std::uint32_t i = 0; i < 10'000; ++i) {
      fixture.meshes.push_back(
          fixture.world.CreateMesh(make_mesh("scale-mesh-" +
                                             std::to_string(i))));
      add_instance(fixture.meshes.back(), i);
    }
    summary = {std::string(name), 10'000, 10'000, 10'000};
  } else if (name == "thousand-instances") {
    fixture.meshes.push_back(
        fixture.world.CreateMesh(make_mesh("shared-instance-mesh")));
    fixture.instances.reserve(1'000);
    for (std::uint32_t i = 0; i < 1'000; ++i) {
      add_instance(fixture.meshes.front(), i);
    }
    summary = {std::string(name), 1, 1'000, 1'000};
  } else {
    throw std::invalid_argument("fixture is not a scale fixture");
  }
  return summary;
}

FrameTimings FromBackend(const merlin::vulkan::FrameCpuTimings& timings) {
  FrameTimings result;
  result.gpu_scene_update_ns = timings.upload_ns;
  result.command_recording_ns = timings.command_recording_ns;
  result.queue_submission_ns = timings.queue_submission_ns;
  result.completion_wait_ns = timings.completion_wait_ns;
  result.readback_ns = timings.readback_ns;
  result.gpu_execution_ns = timings.gpu_execution_ns;
  return result;
}

std::uint64_t Percentile(const std::vector<std::uint64_t>& sorted,
                         std::uint32_t percentile) {
  if (sorted.empty()) {
    return 0;
  }
  const auto rank = (static_cast<std::uint64_t>(percentile) * sorted.size() +
                     99U) /
                    100U;
  return sorted[std::max<std::size_t>(1, static_cast<std::size_t>(rank)) - 1U];
}

Distribution Summarize(const std::vector<FrameTimings>& values,
                       std::uint64_t FrameTimings::*member) {
  std::vector<std::uint64_t> samples;
  samples.reserve(values.size());
  for (const auto& value : values) {
    samples.push_back(value.*member);
  }
  std::sort(samples.begin(), samples.end());
  if (samples.empty()) {
    return {};
  }
  const auto middle = samples.size() / 2U;
  const auto median = (samples.size() & 1U) != 0U
                          ? samples[middle]
                          : samples[middle - 1U] +
                                (samples[middle] - samples[middle - 1U]) / 2U;
  return {median, Percentile(samples, 95), Percentile(samples, 99),
          samples.back()};
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

void WriteDistribution(std::ostream& stream, const Distribution& value) {
  stream << "{\"median\": " << value.median << ", \"p95\": " << value.p95
         << ", \"p99\": " << value.p99 << ", \"max\": " << value.maximum
         << '}';
}

void WriteCounter(std::ostream& stream, std::string_view indent,
                  std::string_view name, std::uint64_t value, bool last = false) {
  stream << indent << "\"" << name << "\": " << value
         << (last ? "\n" : ",\n");
}

void WriteArenaTelemetry(
    std::ostream& stream, const merlin::vulkan::ArenaTelemetry& arena,
    std::string_view indent) {
  stream << "{\n"
         << indent << "  \"capacity_bytes\": " << arena.capacity_bytes
         << ",\n" << indent << "  \"resident_bytes\": "
         << arena.resident_bytes
         << ",\n" << indent << "  \"peak_resident_bytes\": "
         << arena.peak_resident_bytes
         << ",\n" << indent << "  \"free_bytes\": " << arena.free_bytes
         << ",\n" << indent << "  \"largest_free_span_bytes\": "
         << arena.largest_free_span_bytes
         << ",\n" << indent << "  \"retiring_bytes\": "
         << arena.retiring_bytes
         << ",\n" << indent << "  \"allocations\": "
         << arena.allocation_count
         << ",\n" << indent << "  \"releases\": " << arena.release_count
         << ",\n" << indent << "  \"active_ranges\": "
         << arena.active_ranges
         << ",\n" << indent << "  \"peak_active_ranges\": "
         << arena.peak_active_ranges
         << ",\n" << indent << "  \"retiring_ranges\": "
         << arena.retiring_ranges
         << ",\n" << indent << "  \"free_spans\": " << arena.free_spans
         << ",\n" << indent << "  \"blocks\": " << arena.blocks
         << ",\n" << indent << "  \"growths\": " << arena.growth_count
         << '\n' << indent << '}';
}

void WriteUploadRingTelemetry(
    std::ostream& stream, const merlin::vulkan::UploadRingTelemetry& ring,
    std::string_view indent) {
  stream << "{\n"
         << indent << "  \"capacity_bytes\": " << ring.capacity_bytes
         << ",\n" << indent << "  \"peak_capacity_bytes\": "
         << ring.peak_capacity_bytes
         << ",\n" << indent << "  \"in_flight_bytes\": "
         << ring.in_flight_bytes
         << ",\n" << indent << "  \"peak_in_flight_bytes\": "
         << ring.peak_in_flight_bytes
         << ",\n" << indent << "  \"reserved_bytes\": "
         << ring.reserved_bytes
         << ",\n" << indent << "  \"reservations\": "
         << ring.reservation_count
         << ",\n" << indent << "  \"retired_bytes\": "
         << ring.retired_bytes
         << ",\n" << indent << "  \"active_regions\": "
         << ring.active_regions
         << ",\n" << indent << "  \"peak_active_regions\": "
         << ring.peak_active_regions
         << ",\n" << indent << "  \"wraps\": " << ring.wrap_count
         << ",\n" << indent << "  \"growths\": " << ring.growth_count
         << ",\n" << indent << "  \"retired_buffers\": "
         << ring.retired_buffers << '\n' << indent << '}';
}

void WriteBaseline(std::ostream& stream, const Baseline& baseline,
                   std::string_view indent) {
  stream << indent << "{\n" << indent << "  \"name\": ";
  JsonString(stream, baseline.name);
  stream << ",\n" << indent << "  \"samples\": " << baseline.timings.size()
         << ",\n" << indent << "  \"stages_ns\": {\n";
  constexpr std::array stages{
      std::pair{"render_world_update", &FrameTimings::scene_update_ns},
      std::pair{"snapshot_extraction", &FrameTimings::extraction_ns},
      std::pair{"gpu_scene_update", &FrameTimings::gpu_scene_update_ns},
      std::pair{"command_recording", &FrameTimings::command_recording_ns},
      std::pair{"queue_submission", &FrameTimings::queue_submission_ns},
      std::pair{"completion_wait", &FrameTimings::completion_wait_ns},
      std::pair{"readback", &FrameTimings::readback_ns},
      std::pair{"gpu_execution", &FrameTimings::gpu_execution_ns},
      std::pair{"total_frame", &FrameTimings::total_frame_ns}};
  for (std::size_t i = 0; i < stages.size(); ++i) {
    stream << indent << "    \"" << stages[i].first << "\": ";
    WriteDistribution(stream, Summarize(baseline.timings, stages[i].second));
    stream << (i + 1U == stages.size() ? "\n" : ",\n");
  }
  const auto total = Summarize(baseline.timings, &FrameTimings::total_frame_ns);
  const auto hitch_threshold =
      std::max(total.median * 2U, total.median + kHitchFloorNanoseconds);
  const auto hitch_count = static_cast<std::uint64_t>(std::count_if(
      baseline.timings.begin(), baseline.timings.end(),
      [&](const FrameTimings& timing) {
        return timing.total_frame_ns > hitch_threshold;
      }));
  stream << indent << "  },\n" << indent << "  \"frame_hitches\": {"
         << "\"threshold_ns\": " << hitch_threshold << ", \"count\": "
         << hitch_count << "},\n" << indent << "  \"counters\": {\n";
  const auto& count = baseline.counters;
  const auto& snapshot = baseline.snapshot_build_counters;
  const auto counter_indent = std::string(indent) + "    ";
  WriteCounter(stream, counter_indent, "snapshot_visited_records",
               snapshot.visited_records);
  WriteCounter(stream, counter_indent, "snapshot_copied_records",
               snapshot.copied_records);
  WriteCounter(stream, counter_indent, "snapshot_rebuilt_draws",
               snapshot.rebuilt_draws);
  WriteCounter(stream, counter_indent, "snapshot_fully_rebuilt_tables",
               snapshot.fully_rebuilt_tables);
  WriteCounter(stream, counter_indent, "draw_count", count.draw_count);
  WriteCounter(stream, counter_indent, "visible_primitive_count",
               count.visible_primitive_count);
  WriteCounter(stream, counter_indent, "triangle_count", count.triangle_count);
  WriteCounter(stream, counter_indent, "upload_bytes", count.upload_bytes);
  WriteCounter(stream, counter_indent, "vertex_upload_bytes",
               count.vertex_upload_bytes);
  WriteCounter(stream, counter_indent, "index_upload_bytes",
               count.index_upload_bytes);
  WriteCounter(stream, counter_indent, "texture_upload_bytes",
               count.texture_upload_bytes);
  WriteCounter(stream, counter_indent, "upload_ring_reserved_bytes",
               count.upload_ring_reserved_bytes);
  WriteCounter(stream, counter_indent, "readback_bytes", count.readback_bytes);
  WriteCounter(stream, counter_indent, "requested_aov_mask",
               count.requested_aov_mask);
  WriteCounter(stream, counter_indent, "rendered_aov_mask",
               count.rendered_aov_mask);
  WriteCounter(stream, counter_indent, "cpu_readback_aov_mask",
               count.cpu_readback_aov_mask);
  WriteCounter(stream, counter_indent, "requested_aov_count",
               count.requested_aov_count);
  WriteCounter(stream, counter_indent, "rendered_aov_count",
               count.rendered_aov_count);
  WriteCounter(stream, counter_indent, "cpu_readback_aov_count",
               count.cpu_readback_aov_count);
  WriteCounter(stream, counter_indent, "wait_count", count.wait_count);
  WriteCounter(stream, counter_indent, "resolve_count", count.resolve_count);
  WriteCounter(stream, counter_indent, "map_count", count.map_count);
  WriteCounter(stream, counter_indent, "allocation_count",
               count.allocation_count);
  WriteCounter(stream, counter_indent, "buffer_allocation_count",
               count.buffer_allocation_count);
  WriteCounter(stream, counter_indent, "image_allocation_count",
               count.image_allocation_count);
  WriteCounter(stream, counter_indent, "buffer_allocation_bytes",
               count.buffer_allocation_bytes);
  WriteCounter(stream, counter_indent, "image_allocation_bytes",
               count.image_allocation_bytes);
  WriteCounter(stream, counter_indent, "pipeline_creation_count",
               count.pipeline_creation_count);
  WriteCounter(stream, counter_indent, "scene_cache_hits",
               count.scene_cache_hits);
  WriteCounter(stream, counter_indent, "scene_cache_misses",
               count.scene_cache_misses);
  WriteCounter(stream, counter_indent, "geometry_cache_hits",
               count.geometry_cache_hits);
  WriteCounter(stream, counter_indent, "geometry_cache_misses",
               count.geometry_cache_misses);
  WriteCounter(stream, counter_indent, "texture_cache_hits",
               count.texture_cache_hits);
  WriteCounter(stream, counter_indent, "texture_cache_misses",
               count.texture_cache_misses);
  WriteCounter(stream, counter_indent, "sampler_cache_hits",
               count.sampler_cache_hits);
  WriteCounter(stream, counter_indent, "sampler_cache_misses",
               count.sampler_cache_misses);
  WriteCounter(stream, counter_indent, "geometry_reconcile_count",
               count.geometry_reconcile_count);
  WriteCounter(stream, counter_indent, "texture_reconcile_count",
               count.texture_reconcile_count);
  WriteCounter(stream, counter_indent, "sampler_reconcile_count",
               count.sampler_reconcile_count);
  WriteCounter(stream, counter_indent, "buffer_suballocation_count",
               count.buffer_suballocation_count);
  WriteCounter(stream, counter_indent, "buffer_range_release_count",
               count.buffer_range_release_count);
  WriteCounter(stream, counter_indent, "geometry_range_reuse_count",
               count.geometry_range_reuse_count);
  WriteCounter(stream, counter_indent, "geometry_arena_growth_count",
               count.geometry_arena_growth_count);
  WriteCounter(stream, counter_indent, "geometry_arena_growth_bytes",
               count.geometry_arena_growth_bytes);
  WriteCounter(stream, counter_indent, "upload_ring_growth_count",
               count.upload_ring_growth_count);
  WriteCounter(stream, counter_indent, "upload_ring_growth_bytes",
               count.upload_ring_growth_bytes);
  WriteCounter(stream, counter_indent, "pipeline_cache_hits",
               count.pipeline_cache_hits);
  WriteCounter(stream, counter_indent, "pipeline_cache_misses",
               count.pipeline_cache_misses);
  WriteCounter(stream, counter_indent, "shader_module_cache_hits",
               count.shader_module_cache_hits);
  WriteCounter(stream, counter_indent, "shader_module_cache_misses",
               count.shader_module_cache_misses);
  WriteCounter(stream, counter_indent, "descriptor_layout_cache_hits",
               count.descriptor_layout_cache_hits);
  WriteCounter(stream, counter_indent, "descriptor_layout_cache_misses",
               count.descriptor_layout_cache_misses);
  WriteCounter(stream, counter_indent, "descriptor_pool_creation_count",
               count.descriptor_pool_creation_count);
  WriteCounter(stream, counter_indent, "descriptor_allocation_count",
               count.descriptor_allocation_count);
  WriteCounter(stream, counter_indent, "descriptor_update_count",
               count.descriptor_update_count);
  WriteCounter(stream, counter_indent,
               "bindless_sampled_image_descriptor_update_count",
               count.bindless_sampled_image_descriptor_update_count);
  WriteCounter(stream, counter_indent,
               "bindless_sampler_descriptor_update_count",
               count.bindless_sampler_descriptor_update_count, true);
  stream << indent << "  }\n" << indent << '}';
}

void WriteJson(std::ostream& stream, const Arguments& arguments,
               const FixtureSummary& fixture,
               const merlin::vulkan::RendererCapabilities& capabilities,
               const merlin::vulkan::RendererStatistics& statistics,
               const std::vector<Baseline>& baselines) {
  const auto& textures = statistics.bindless_texture_slots;
  const auto& samplers = statistics.bindless_samplers;
  stream << "{\n  \"schema\": \"merlin-benchmark/v3\",\n"
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
  stream << ",\n    \"timestamp_queries\": "
         << (capabilities.timestamp_queries ? "true" : "false")
         << "\n  },\n  \"fixture\": {\n    \"name\": ";
  JsonString(stream, fixture.name);
  stream << ",\n    \"mesh_count\": " << fixture.mesh_count
         << ",\n    \"instance_count\": " << fixture.instance_count
         << ",\n    \"triangle_count\": " << fixture.triangle_count
         << ",\n    \"resolution\": {\n      \"width\": " << arguments.width
         << ",\n      \"height\": " << arguments.height
         << "\n    }\n  },\n  \"residency\": {\n"
         << "    \"descriptor_backend\": ";
  JsonString(stream, merlin::vulkan::DescriptorBackendName(
                         capabilities.descriptor_indexing_selection
                             .selected_backend));
  stream << ",\n    \"descriptor_fallback_reason\": ";
  JsonString(stream, merlin::vulkan::DescriptorFallbackReasonName(
                         capabilities.descriptor_indexing_selection
                             .fallback_reason));
  stream << ",\n    \"bindless_resource_tables\": "
         << (statistics.bindless_resource_tables ? "true" : "false")
         << ",\n    \"geometry\": {\n"
         << "      \"pending_range_retirements\": "
         << statistics.pending_geometry_retirements
         << ",\n      \"range_retirement_collections\": "
         << statistics.geometry_range_retirements
         << ",\n      \"vertex_arena\": ";
  WriteArenaTelemetry(stream, statistics.vertex_arena, "      ");
  stream << ",\n      \"index_arena\": ";
  WriteArenaTelemetry(stream, statistics.index_arena, "      ");
  stream << "\n    },\n    \"upload_ring\": ";
  WriteUploadRingTelemetry(stream, statistics.upload_ring, "    ");
  stream << ",\n    \"textures\": {\n"
         << "      \"capacity\": " << textures.capacity
         << ",\n      \"reserved\": " << textures.reserved_slots
         << ",\n      \"current\": " << textures.current_use
         << ",\n      \"peak\": " << textures.peak_use
         << ",\n      \"retiring\": " << textures.retiring_slots
         << ",\n      \"available\": " << textures.available_slots
         << ",\n      \"allocations\": " << textures.allocation_count
         << ",\n      \"reuses\": " << textures.reuse_count
         << ",\n      \"retirements\": " << textures.retirement_count
         << ",\n      \"retirement_collections\": "
         << textures.retirement_collection_count
         << ",\n      \"descriptor_updates\": "
         << textures.descriptor_update_count
         << ",\n      \"exhaustions\": " << textures.exhaustion_count
         << ",\n      \"generation_mismatches\": "
         << textures.generation_mismatch_count
         << "\n    },\n    \"samplers\": {\n"
         << "      \"capacity\": " << samplers.slots.capacity
         << ",\n      \"current\": " << samplers.slots.current_use
         << ",\n      \"peak\": " << samplers.slots.peak_use
         << ",\n      \"retiring\": " << samplers.slots.retiring_slots
         << ",\n      \"available\": " << samplers.slots.available_slots
         << ",\n      \"allocations\": "
         << samplers.slots.allocation_count
         << ",\n      \"reuses\": " << samplers.slots.reuse_count
         << ",\n      \"retirements\": "
         << samplers.slots.retirement_count
         << ",\n      \"retirement_collections\": "
         << samplers.slots.retirement_collection_count
         << ",\n      \"descriptor_updates\": "
         << samplers.slots.descriptor_update_count
         << ",\n      \"exhaustions\": "
         << samplers.slots.exhaustion_count
         << ",\n      \"generation_mismatches\": "
         << samplers.slots.generation_mismatch_count
         << ",\n      \"unique\": " << samplers.unique_sampler_count
         << ",\n      \"current_references\": "
         << samplers.current_reference_count
         << ",\n      \"peak_references\": "
         << samplers.peak_reference_count
         << ",\n      \"deduplication_hits\": "
         << samplers.deduplication_hit_count
         << "\n    }\n  },\n  \"baselines\": [\n";
  for (std::size_t index = 0; index < baselines.size(); ++index) {
    WriteBaseline(stream, baselines[index], "    ");
    stream << (index + 1U == baselines.size() ? "\n" : ",\n");
  }
  stream << "  ]\n}\n";
}

using Products = std::vector<merlin::vulkan::RenderProductRequest>;

const Products& AllProducts() {
  static const Products products{{merlin::Aov::Color, true},
                                 {merlin::Aov::Depth, true},
                                 {merlin::Aov::PrimId, true},
                                 {merlin::Aov::InstanceId, true}};
  return products;
}

merlin::vulkan::RenderResult Render(
    merlin::vulkan::Renderer& renderer,
    const merlin::extraction::SceneExtractor& extractor,
    const merlin::vulkan::ShaderPaths& shaders, const Arguments& arguments,
    const Products& products) {
  merlin::vulkan::RenderRequest request;
  request.snapshot = extractor.snapshot();
  request.width = arguments.width;
  request.height = arguments.height;
  request.shaders = shaders;
  request.products = products;
  return renderer.Resolve(renderer.Submit(request));
}

void AssertStatic(const merlin::vulkan::FrameCounters& counters) {
  if (counters.upload_bytes != 0 || counters.allocation_count != 0 ||
      counters.pipeline_creation_count != 0 ||
      counters.shader_module_cache_misses != 0 ||
      counters.geometry_cache_misses != 0 ||
      counters.descriptor_pool_creation_count != 0 ||
      counters.descriptor_allocation_count != 0 ||
      counters.descriptor_update_count != 0 ||
      counters.bindless_sampled_image_descriptor_update_count != 0 ||
      counters.bindless_sampler_descriptor_update_count != 0) {
    throw std::runtime_error(
        "static frame performed upload/allocation/shader/pipeline/geometry/"
        "descriptor work");
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto arguments = ParseArguments(argc, argv);
    const auto executable_dir = std::filesystem::absolute(argv[0]).parent_path();
    const merlin::vulkan::ShaderPaths shaders{
        executable_dir / "shaders" / "triangle.vert.spv",
        executable_dir / "shaders" / "triangle.frag.spv",
        executable_dir / "shaders" / "triangle.bindless.vert.spv",
        executable_dir / "shaders" / "triangle.bindless.frag.spv"};
    merlin::vulkan::Renderer renderer;
    merlin::extraction::SceneExtractor extractor;
    std::vector<Baseline> baselines;
    FixtureSummary fixture_summary;

    const auto render = [&](const Products& products = AllProducts()) {
      return Render(renderer, extractor, shaders, arguments, products);
    };
    const auto record_render = [&](std::string name, const Products& products) {
      const auto start = CpuClock::now();
      const auto result = render(products);
      auto timings = FromBackend(result.cpu_timings);
      timings.total_frame_ns = ElapsedNanoseconds(start);
      baselines.push_back(
          {std::move(name), {timings}, result.counters, {}});
      return result;
    };
    const auto measure = [&](std::string name, merlin::RenderWorld& world,
                             const auto& edit,
                             const Products& products = AllProducts()) {
      const auto start = CpuClock::now();
      const auto update_start = CpuClock::now();
      edit();
      const auto changes = world.Commit();
      const auto update_ns = ElapsedNanoseconds(update_start);
      const auto extraction_start = CpuClock::now();
      extractor.Apply(world, changes);
      const auto extraction_ns = ElapsedNanoseconds(extraction_start);
      const auto result = render(products);
      auto timings = FromBackend(result.cpu_timings);
      timings.scene_update_ns = update_ns;
      timings.extraction_ns = extraction_ns;
      timings.total_frame_ns = ElapsedNanoseconds(start);
      baselines.push_back(
          {std::move(name), {timings}, result.counters,
           extractor.snapshot()->build_counters});
      return result;
    };
    const auto steady = [&](std::string name) {
      std::vector<FrameTimings> samples;
      samples.reserve(arguments.steady_frames);
      merlin::vulkan::FrameCounters counters;
      for (std::uint32_t frame = 0; frame < arguments.steady_frames; ++frame) {
        const auto start = CpuClock::now();
        const auto result = render();
        auto timing = FromBackend(result.cpu_timings);
        timing.total_frame_ns = ElapsedNanoseconds(start);
        samples.push_back(timing);
        if (frame == 0) {
          counters = result.counters;
        } else if (result.counters != counters) {
          throw std::runtime_error("steady-state structural counters changed");
        }
      }
      AssertStatic(counters);
      baselines.push_back(
          {std::move(name), std::move(samples), counters, {}});
    };

    const bool reference_fixture =
        arguments.fixture == "reference" ||
        arguments.fixture == "aov-combinations" || arguments.fixture == "4k";
    if (reference_fixture) {
      SceneFixture fixture;
      measure("first-frame", fixture.world, [&] {
        PopulateScene(fixture);
        extractor.SetActiveCamera(fixture.camera);
      });
      fixture_summary = {arguments.fixture, 2, 3, 4};
      steady("steady-state");

      const auto camera = measure("camera-only", fixture.world, [&] {
        auto descriptor = fixture.world.Get(fixture.camera);
        descriptor.view.values[12] = 0.125F;
        fixture.world.UpdateCamera(fixture.camera, std::move(descriptor),
                                   merlin::ChangeAspect::Camera);
      });
      AssertStatic(camera.counters);

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
      measure("edit-topology", fixture.world, [&] {
        auto mesh = fixture.world.Get(fixture.triangle);
        mesh.indices = {1, 2, 0};
        fixture.world.UpdateMesh(fixture.triangle, std::move(mesh),
                                 merlin::ChangeAspect::Topology);
      });

      const auto warm = [&](const Products& products) {
        for (std::uint32_t i = 0; i < renderer.statistics().frame_context_count;
             ++i) {
          (void)render(products);
        }
      };
      const Products color_only{{merlin::Aov::Color, true}};
      warm(color_only);
      const auto color = record_render("aov-color-only", color_only);
      const auto expected_color_bytes =
          static_cast<std::uint64_t>(arguments.width) * arguments.height * 4U;
      if (color.counters.readback_bytes != expected_color_bytes ||
          color.counters.cpu_readback_aov_count != 1 ||
          color.counters.cpu_readback_aov_mask !=
              (std::uint64_t{1} <<
               static_cast<std::uint32_t>(merlin::Aov::Color))) {
        throw std::runtime_error(
            "color-only fixture performed non-color CPU readback");
      }
      const Products color_depth{{merlin::Aov::Color, true},
                                 {merlin::Aov::Depth, true}};
      warm(color_depth);
      (void)record_render("aov-color-depth", color_depth);
      warm(AllProducts());
      (void)record_render("aov-all", AllProducts());

      measure("remove-mesh", fixture.world, [&] {
        fixture.world.Remove(fixture.quad_instance);
        fixture.world.Remove(fixture.quad);
      });
    } else {
      ScaleFixture fixture;
      measure("first-frame", fixture.world, [&] {
        fixture_summary =
            PopulateScaleFixture(arguments.fixture, fixture);
      });
      steady("steady-state");
    }

    if (arguments.output.empty()) {
      WriteJson(std::cout, arguments, fixture_summary, renderer.capabilities(),
                renderer.statistics(), baselines);
    } else {
      if (arguments.output.has_parent_path()) {
        std::filesystem::create_directories(arguments.output.parent_path());
      }
      std::ofstream stream(arguments.output, std::ios::binary);
      if (!stream) {
        throw std::runtime_error("could not create output: " +
                                 arguments.output.string());
      }
      WriteJson(stream, arguments, fixture_summary, renderer.capabilities(),
                renderer.statistics(), baselines);
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
