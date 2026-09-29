#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/vulkan/renderer.hpp>
#include <merlin/vulkan/shader_abi.hpp>

#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <ostream>
#include <sstream>
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
  bool validation{};
  std::uint32_t arena_blocks{4};
};

struct FrameTimings {
  std::uint64_t scene_update_ns{};
  std::uint64_t extraction_ns{};
  std::uint64_t gpu_scene_update_ns{};
  std::uint64_t gaussian_preparation_ns{};
  std::uint64_t gaussian_attribute_upload_ns{};
  std::uint64_t gaussian_prepared_upload_ns{};
  std::uint64_t gaussian_gpu_preparation_ns{};
  std::uint64_t gaussian_gpu_sort_ns{};
  std::uint64_t gaussian_gpu_tile_ns{};
  std::uint64_t gaussian_raster_ns{};
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

struct MeshVerification {
  std::uint32_t exact_aov_comparisons{};
  std::uint32_t required_submission_rejections{};
  std::string required_rejection_reason;
  bool generated_parameter_recovery{};
  bool generated_module_recovery{};
};

struct GaussianComparison {
  std::string name;
  std::uint32_t max_color_channel_error{};
  std::uint64_t depth_pixels{};
  std::uint64_t prim_id_pixels{};
  std::uint64_t instance_id_pixels{};
  bool passed{};
};

struct FixtureSummary {
  std::string name;
  std::uint64_t mesh_count{};
  std::uint64_t instance_count{};
  std::uint64_t triangle_count{};
  std::uint64_t gaussian_resource_count{};
  std::uint64_t gaussian_particle_count{};
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
  std::vector<merlin::MaterialHandle> materials;
  std::vector<merlin::MeshHandle> meshes;
  std::vector<merlin::InstanceHandle> instances;
  std::vector<merlin::GaussianHandle> gaussians;
  merlin::CameraHandle camera;
};

// Gaussian execution measured under camera motion: the CPU-sorted reference
// stream, or GPU preparation and sorting with the sorted-stream draws or
// compute tile raster.
enum class GaussianExecution {
  Cpu,
  GpuSortedStream,
  GpuTiled,
};

std::string_view GaussianExecutionName(GaussianExecution execution) {
  switch (execution) {
  case GaussianExecution::Cpu:
    return "cpu";
  case GaussianExecution::GpuSortedStream:
    return "gpu-sorted-stream";
  case GaussianExecution::GpuTiled:
    return "gpu-tiled";
  }
  return "unknown";
}

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
      std::string_view("gpu-driven-small-objects"),
      std::string_view("gpu-driven-diverse-objects"),
      std::string_view("gpu-driven-textured-objects"),
      std::string_view("gpu-driven-arena-objects"),
      std::string_view("generated-material-objects"),
      std::string_view("one-million-gaussians"),
      std::string_view("five-million-gaussians"),
      std::string_view("ten-million-gaussians"),
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
    } else if (argument == "--validate") {
      result.validation = true;
    } else if (argument == "--arena-blocks") {
      result.arena_blocks = ParseUnsigned(value(argument), argument);
      if (result.arena_blocks != 2 && result.arena_blocks != 4 &&
          result.arena_blocks != 8 && result.arena_blocks != 16) {
        throw std::invalid_argument("--arena-blocks requires 2, 4, 8, or 16");
      }
    } else if (argument == "--help") {
      std::cout
          << "Usage: merlin-benchmark [--output FILE] [--fixture NAME] "
             "[--width N] [--height N] [--steady-frames N] [--validate] "
             "[--arena-blocks 2|4|8|16]\n"
             "Fixtures: reference, million-triangles, ten-thousand-meshes, "
             "thousand-instances, gpu-driven-small-objects, "
             "gpu-driven-diverse-objects, gpu-driven-textured-objects, "
             "gpu-driven-arena-objects, generated-material-objects, "
             "one-million-gaussians, "
             "five-million-gaussians, ten-million-gaussians, "
             "aov-combinations, 4k\n";
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
  } else if (name == "one-million-gaussians" ||
             name == "five-million-gaussians" ||
             name == "ten-million-gaussians") {
    const std::size_t particle_count =
        name == "one-million-gaussians"
            ? 1'000'000U
            : (name == "five-million-gaussians" ? 5'000'000U : 10'000'000U);
    merlin::GaussianDescriptor gaussian;
    gaussian.label = std::string(name);
    gaussian.positions.resize(particle_count);
    gaussian.covariances.resize(
        particle_count,
        {0.000004F, 0.0F, 0.0F, 0.000004F, 0.0F, 0.000004F});
    gaussian.opacities.resize(particle_count, 0.7F);
    gaussian.spherical_harmonics_degree = 0;
    gaussian.spherical_harmonics_coefficients.resize(
        particle_count, {0.35F, 0.55F, 0.8F});
    for (std::size_t index = 0; index < particle_count; ++index) {
      const auto column = index % 1000U;
      const auto row = (index / 1000U) % 1000U;
      const auto layer = (index / 1'000'000U) % 10U;
      gaussian.positions[index] = {
          static_cast<float>(column) * 0.0018F - 0.9F,
          static_cast<float>(row) * 0.0018F - 0.9F,
          0.1F + static_cast<float>(layer) * 0.03F};
    }
    fixture.gaussians.push_back(
        fixture.world.CreateGaussian(std::move(gaussian)));
    summary = {std::string(name), 0, 0, 0, 1, particle_count};
  } else {
    throw std::invalid_argument("fixture is not a scale fixture");
  }
  return summary;
}

constexpr std::uint32_t kDiverseMeshCount = 16;
constexpr std::uint32_t kDiverseMaterialCount = 8;
constexpr std::uint32_t kDiverseMaterialRunLength = 256;

merlin::MaterialModule ScaleMaterialModule() {
  merlin::MaterialModule module;
  module.key = "merlin-benchmark-generated-abi-tint/v1";
  module.parameters.entries = {{"tint", merlin::MaterialValueType::Float3, 1}};
  module.requirements.results = merlin::MaterialResultField::BaseColor;
  return module;
}

merlin::vulkan::GeneratedMaterialArtifact ScaleMaterialArtifact(
    const std::filesystem::path& shader_dir) {
  const auto module = ScaleMaterialModule();
  merlin::vulkan::GeneratedMaterialArtifact artifact;
  artifact.module_key = module.key;
  artifact.fragment = shader_dir / "benchmark-generated.frag.spv";
  artifact.fragment_entry_point = "benchmark_generated_fragment";
  artifact.parameter_buffer_size = 16;
  artifact.parameter_bindings = {{"tint", merlin::MaterialValueType::Float3, 1, 0, 0}};
  artifact.reflection.target = "spirv";
  artifact.reflection.entry_points = {artifact.fragment_entry_point};
  artifact.reflection.parameters = module.parameters;
  return artifact;
}

std::uint32_t ArenaFixtureVertexCount(std::uint32_t blocks) {
  // Pad unused vertices to exercise the production 256-KiB first-fit arenas
  // without multiplying raster work. All geometries keep one/two triangles.
  return (256U * 1024U) / (kDiverseMeshCount / blocks) /
         sizeof(merlin::extraction::DrawVertex);
}

FixtureSummary PopulateGpuDrivenObjects(ScaleFixture& fixture,
    std::uint32_t target_count, bool diverse, bool textured = false,
    std::uint32_t arena_blocks = 0, bool generated = false) {
  if (fixture.meshes.empty()) {
    std::vector<merlin::TextureHandle> textures;
    std::vector<merlin::SamplerHandle> samplers;
    if (textured) {
      for (std::uint32_t index = 0; index < 4; ++index) {
        merlin::TextureDescriptor texture;
        texture.label = "scale-checker-" + std::to_string(index);
        texture.width = 2;
        texture.height = 2;
        const auto channel = static_cast<std::uint8_t>(64U + index * 40U);
        texture.pixels = {channel, 255, 96, 255, 255, channel, 224, 255,
            64, 128, channel, 255, 224, channel, 64, 255};
        textures.push_back(fixture.world.CreateTexture(std::move(texture)));
      }
      for (std::uint32_t index = 0; index < 2; ++index) {
        merlin::SamplerDescriptor sampler;
        sampler.label = "scale-sampler-" + std::to_string(index);
        sampler.min_filter = sampler.mag_filter = index == 0
                                                      ? merlin::FilterMode::Nearest
                                                      : merlin::FilterMode::Linear;
        samplers.push_back(fixture.world.CreateSampler(std::move(sampler)));
      }
    }
    const auto material_count = diverse ? kDiverseMaterialCount : 1U;
    for (std::uint32_t index = 0; index < material_count; ++index) {
      merlin::MaterialDescriptor material;
      material.label = "gpu-driven-material-" + std::to_string(index);
      material.parameters.base_color = {0.18F, 0.78F, 1.0F, 1.0F};
      if (diverse) {
        const auto factor = static_cast<float>(index) / material_count;
        material.parameters.base_color = {0.2F + 0.7F * factor,
            0.8F - 0.6F * factor, 0.3F + 0.4F * factor, 1.0F};
        material.parameters.roughness = 0.15F + 0.8F * factor;
        material.double_sided = index % 2U != 0;
      }
      if (textured) {
        material.features |= merlin::MaterialFeature::BaseColorTexture;
        material.base_color_texture = merlin::TextureBinding{
            textures[index % textures.size()], samplers[index % samplers.size()]};
      }
      if (generated) {
        material.module = ScaleMaterialModule();
        material.generated_resources.key = "scale-empty-resources";
        material.generated_parameters.key = "scale-tint-" + std::to_string(index);
        material.generated_parameters.entries = {{"tint",
            merlin::MaterialValueType::Float3,
            {merlin::Vec3{0.2F + 0.08F * index, 0.7F, 0.35F}}}};
      }
      fixture.materials.push_back(
          fixture.world.CreateMaterial(std::move(material)));
    }

    const auto mesh_count = diverse ? kDiverseMeshCount : 1U;
    for (std::uint32_t index = 0; index < mesh_count; ++index) {
      merlin::MeshDescriptor mesh;
      mesh.label = "gpu-driven-mesh-" + std::to_string(index);
      mesh.positions = {{-0.002F, -0.002F, 0.0F},
          {0.002F, -0.002F, 0.0F},
          {0.0F, 0.002F, 0.0F}};
      mesh.indices = {0, 1, 2};
      if (diverse) {
        const auto extent = 0.0015F + 0.00003F * static_cast<float>(index);
        mesh.positions = {{-extent, -extent, 0.0F},
            {extent, -extent, 0.0F}, {extent, extent, 0.0F},
            {-extent, extent, 0.0F}};
        if (index % 2U != 0) {
          mesh.indices = {0, 1, 2, 0, 2, 3};
        }
      }
      if (textured) {
        mesh.texcoords = {{0.0F, 0.0F}, {1.0F, 0.0F},
            {1.0F, 1.0F}, {0.0F, 1.0F}};
      }
      if (arena_blocks != 0) {
        mesh.positions.resize(ArenaFixtureVertexCount(arena_blocks), mesh.positions[0]);
        mesh.texcoords.resize(mesh.positions.size());
      }
      fixture.meshes.push_back(fixture.world.CreateMesh(std::move(mesh)));
    }
  }

  if (target_count < fixture.instances.size()) {
    throw std::invalid_argument(
        "GPU-driven scale target cannot shrink the fixture");
  }
  fixture.instances.reserve(target_count);
  for (std::uint32_t index =
           static_cast<std::uint32_t>(fixture.instances.size());
      index < target_count; ++index) {
    merlin::InstanceDescriptor instance;
    instance.label = "gpu-driven-instance-" + std::to_string(index);
    instance.mesh = fixture.meshes[index % fixture.meshes.size()];
    instance.material = fixture.materials[
        (index / kDiverseMaterialRunLength) % fixture.materials.size()];
    const auto column = index % 320U;
    const auto row = (index / 320U) % 320U;
    instance.transform.values[12] = static_cast<float>(column) * 0.0056F - 0.9F;
    instance.transform.values[13] = static_cast<float>(row) * 0.0056F - 0.9F;
    fixture.instances.push_back(
        fixture.world.CreateInstance(std::move(instance)));
  }

  return {diverse ? "gpu-driven-diverse-objects" : "gpu-driven-small-objects",
      fixture.meshes.size(), target_count,
      diverse ? target_count + target_count / 2U : target_count};
}

FrameTimings FromBackend(const merlin::vulkan::FrameCpuTimings& timings) {
  FrameTimings result;
  result.gpu_scene_update_ns = timings.upload_ns;
  result.gaussian_preparation_ns = timings.gaussian_preparation_ns;
  result.gaussian_attribute_upload_ns =
      timings.gaussian_attribute_upload_ns;
  result.gaussian_prepared_upload_ns =
      timings.gaussian_prepared_upload_ns;
  result.gaussian_gpu_preparation_ns = timings.gaussian_gpu_preparation_ns;
  result.gaussian_gpu_sort_ns = timings.gaussian_gpu_sort_ns;
  result.gaussian_gpu_tile_ns = timings.gaussian_gpu_tile_ns;
  result.gaussian_raster_ns = timings.gaussian_raster_ns;
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
    std::uint64_t FrameTimings::* member) {
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
    case '"':
      stream << "\\\"";
      break;
    case '\\':
      stream << "\\\\";
      break;
    case '\b':
      stream << "\\b";
      break;
    case '\f':
      stream << "\\f";
      break;
    case '\n':
      stream << "\\n";
      break;
    case '\r':
      stream << "\\r";
      break;
    case '\t':
      stream << "\\t";
      break;
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
         << ",\n"
         << indent << "  \"resident_bytes\": "
         << arena.resident_bytes
         << ",\n"
         << indent << "  \"peak_resident_bytes\": "
         << arena.peak_resident_bytes
         << ",\n"
         << indent << "  \"free_bytes\": " << arena.free_bytes
         << ",\n"
         << indent << "  \"largest_free_span_bytes\": "
         << arena.largest_free_span_bytes
         << ",\n"
         << indent << "  \"retiring_bytes\": "
         << arena.retiring_bytes
         << ",\n"
         << indent << "  \"allocations\": "
         << arena.allocation_count
         << ",\n"
         << indent << "  \"releases\": " << arena.release_count
         << ",\n"
         << indent << "  \"active_ranges\": "
         << arena.active_ranges
         << ",\n"
         << indent << "  \"peak_active_ranges\": "
         << arena.peak_active_ranges
         << ",\n"
         << indent << "  \"retiring_ranges\": "
         << arena.retiring_ranges
         << ",\n"
         << indent << "  \"free_spans\": " << arena.free_spans
         << ",\n"
         << indent << "  \"blocks\": " << arena.blocks
         << ",\n"
         << indent << "  \"growths\": " << arena.growth_count
         << '\n'
         << indent << '}';
}

void WriteUploadRingTelemetry(
    std::ostream& stream, const merlin::vulkan::UploadRingTelemetry& ring,
    std::string_view indent) {
  stream << "{\n"
         << indent << "  \"capacity_bytes\": " << ring.capacity_bytes
         << ",\n"
         << indent << "  \"peak_capacity_bytes\": "
         << ring.peak_capacity_bytes
         << ",\n"
         << indent << "  \"in_flight_bytes\": "
         << ring.in_flight_bytes
         << ",\n"
         << indent << "  \"peak_in_flight_bytes\": "
         << ring.peak_in_flight_bytes
         << ",\n"
         << indent << "  \"reserved_bytes\": "
         << ring.reserved_bytes
         << ",\n"
         << indent << "  \"reservations\": "
         << ring.reservation_count
         << ",\n"
         << indent << "  \"retired_bytes\": "
         << ring.retired_bytes
         << ",\n"
         << indent << "  \"active_regions\": "
         << ring.active_regions
         << ",\n"
         << indent << "  \"peak_active_regions\": "
         << ring.peak_active_regions
         << ",\n"
         << indent << "  \"wraps\": " << ring.wrap_count
         << ",\n"
         << indent << "  \"growths\": " << ring.growth_count
         << ",\n"
         << indent << "  \"retired_buffers\": "
         << ring.retired_buffers << '\n'
         << indent << '}';
}

void WriteBaseline(std::ostream& stream, const Baseline& baseline,
    std::string_view indent) {
  stream << indent << "{\n"
         << indent << "  \"name\": ";
  JsonString(stream, baseline.name);
  stream << ",\n"
         << indent << "  \"samples\": " << baseline.timings.size()
         << ",\n"
         << indent << "  \"stages_ns\": {\n";
  constexpr std::array stages{
      std::pair{"render_world_update", &FrameTimings::scene_update_ns},
      std::pair{"snapshot_extraction", &FrameTimings::extraction_ns},
      std::pair{"gpu_scene_update", &FrameTimings::gpu_scene_update_ns},
      std::pair{"gaussian_preparation", &FrameTimings::gaussian_preparation_ns},
      std::pair{"gaussian_attribute_upload",
          &FrameTimings::gaussian_attribute_upload_ns},
      std::pair{"gaussian_prepared_upload",
          &FrameTimings::gaussian_prepared_upload_ns},
      std::pair{"gaussian_gpu_preparation", &FrameTimings::gaussian_gpu_preparation_ns},
      std::pair{"gaussian_gpu_sort", &FrameTimings::gaussian_gpu_sort_ns},
      std::pair{"gaussian_gpu_tile", &FrameTimings::gaussian_gpu_tile_ns},
      std::pair{"gaussian_raster", &FrameTimings::gaussian_raster_ns},
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
  stream << indent << "  },\n"
         << indent << "  \"frame_hitches\": {"
         << "\"threshold_ns\": " << hitch_threshold << ", \"count\": "
         << hitch_count << "},\n"
         << indent << "  \"counters\": {\n";
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
  WriteCounter(stream, counter_indent, "gaussian_candidate_count",
      count.gaussian_candidate_count);
  WriteCounter(stream, counter_indent, "gaussian_visible_count",
      count.gaussian_visible_count);
  WriteCounter(stream, counter_indent, "gaussian_hidden_count",
      count.gaussian_hidden_count);
  WriteCounter(stream, counter_indent, "gaussian_opacity_culled_count",
      count.gaussian_opacity_culled_count);
  WriteCounter(stream, counter_indent, "gaussian_frustum_culled_count",
      count.gaussian_frustum_culled_count);
  WriteCounter(stream, counter_indent, "gaussian_invalid_culled_count",
      count.gaussian_invalid_culled_count);
  WriteCounter(stream, counter_indent, "gaussian_sorted_count",
      count.gaussian_sorted_count);
  WriteCounter(stream, counter_indent,
      "gaussian_sorting_policy_fallback_count",
      count.gaussian_sorting_policy_fallback_count);
  WriteCounter(stream, counter_indent, "gaussian_preparation_cache_hits",
      count.gaussian_preparation_cache_hits);
  WriteCounter(stream, counter_indent, "gaussian_preparation_cache_misses",
      count.gaussian_preparation_cache_misses);
  WriteCounter(stream, counter_indent,
      "gaussian_gpu_preparation_dispatch_count",
      count.gaussian_gpu_preparation_dispatch_count);
  WriteCounter(stream, counter_indent,
      "gaussian_gpu_preparation_candidate_count",
      count.gaussian_gpu_preparation_candidate_count);
  WriteCounter(stream, counter_indent,
      "gaussian_gpu_preparation_visible_count",
      count.gaussian_gpu_preparation_visible_count);
  WriteCounter(stream, counter_indent,
      "gaussian_gpu_preparation_opacity_culled_count",
      count.gaussian_gpu_preparation_opacity_culled_count);
  WriteCounter(stream, counter_indent,
      "gaussian_gpu_preparation_frustum_culled_count",
      count.gaussian_gpu_preparation_frustum_culled_count);
  WriteCounter(stream, counter_indent,
      "gaussian_gpu_preparation_invalid_culled_count",
      count.gaussian_gpu_preparation_invalid_culled_count);
  WriteCounter(stream, counter_indent,
      "gaussian_gpu_preparation_fallback_count",
      count.gaussian_gpu_preparation_fallback_count);
  WriteCounter(stream, counter_indent, "gaussian_gpu_sort_dispatch_count",
      count.gaussian_gpu_sort_dispatch_count);
  WriteCounter(stream, counter_indent, "gaussian_gpu_sort_pass_count",
      count.gaussian_gpu_sort_pass_count);
  WriteCounter(stream, counter_indent, "gaussian_gpu_sort_key_count",
      count.gaussian_gpu_sort_key_count);
  WriteCounter(stream, counter_indent, "gaussian_gpu_sorted_count",
      count.gaussian_gpu_sorted_count);
  WriteCounter(stream, counter_indent,
      "gaussian_gpu_sort_reference_divergence_count",
      count.gaussian_gpu_sort_reference_divergence_count);
  WriteCounter(stream, counter_indent, "gaussian_gpu_sort_fallback_count",
      count.gaussian_gpu_sort_fallback_count);
  WriteCounter(stream, counter_indent, "gaussian_gpu_raster_dispatch_count",
      count.gaussian_gpu_raster_dispatch_count);
  WriteCounter(stream, counter_indent, "gaussian_gpu_raster_instance_count",
      count.gaussian_gpu_raster_instance_count);
  WriteCounter(stream, counter_indent,
      "gaussian_gpu_raster_indirect_draw_count",
      count.gaussian_gpu_raster_indirect_draw_count);
  WriteCounter(stream, counter_indent, "gaussian_gpu_raster_fallback_count",
      count.gaussian_gpu_raster_fallback_count);
  WriteCounter(stream, counter_indent, "gaussian_gpu_tile_dispatch_count",
      count.gaussian_gpu_tile_dispatch_count);
  WriteCounter(stream, counter_indent, "gaussian_gpu_tile_sort_pass_count",
      count.gaussian_gpu_tile_sort_pass_count);
  WriteCounter(stream, counter_indent, "gaussian_gpu_tile_count",
      count.gaussian_gpu_tile_count);
  WriteCounter(stream, counter_indent, "gaussian_gpu_tile_occupied_count",
      count.gaussian_gpu_tile_occupied_count);
  WriteCounter(stream, counter_indent, "gaussian_gpu_tile_max_pair_count",
      count.gaussian_gpu_tile_max_pair_count);
  WriteCounter(stream, counter_indent, "gaussian_gpu_tile_pair_capacity",
      count.gaussian_gpu_tile_pair_capacity);
  WriteCounter(stream, counter_indent, "gaussian_gpu_tile_requested_pair_count",
      count.gaussian_gpu_tile_requested_pair_count);
  WriteCounter(stream, counter_indent, "gaussian_gpu_tile_pair_count",
      count.gaussian_gpu_tile_pair_count);
  WriteCounter(stream, counter_indent, "gaussian_gpu_tile_pair_overflow_count",
      count.gaussian_gpu_tile_pair_overflow_count);
  WriteCounter(stream, counter_indent, "gaussian_gpu_tile_clamped_record_count",
      count.gaussian_gpu_tile_clamped_record_count);
  WriteCounter(stream, counter_indent, "gaussian_gpu_tile_reference_divergence_count",
      count.gaussian_gpu_tile_reference_divergence_count);
  WriteCounter(stream, counter_indent, "gaussian_gpu_tile_fallback_count",
      count.gaussian_gpu_tile_fallback_count);
  WriteCounter(stream, counter_indent,
      "gaussian_cpu_preparation_skipped_count",
      count.gaussian_cpu_preparation_skipped_count);
  WriteCounter(stream, counter_indent,
      "gaussian_gpu_tile_raster_dispatch_count",
      count.gaussian_gpu_tile_raster_dispatch_count);
  WriteCounter(stream, counter_indent, "gaussian_gpu_tile_raster_frame_count",
      count.gaussian_gpu_tile_raster_frame_count);
  WriteCounter(stream, counter_indent,
      "gaussian_gpu_tile_raster_overflow_fallback_count",
      count.gaussian_gpu_tile_raster_overflow_fallback_count);
  WriteCounter(stream, counter_indent,
      "gaussian_gpu_tile_raster_fallback_count",
      count.gaussian_gpu_tile_raster_fallback_count);
  WriteCounter(stream, counter_indent, "gaussian_draw_count",
      count.gaussian_draw_count);
  WriteCounter(stream, counter_indent, "gaussian_attribute_upload_bytes",
      count.gaussian_attribute_upload_bytes);
  WriteCounter(stream, counter_indent, "gaussian_attribute_copy_range_count",
      count.gaussian_attribute_copy_range_count);
  WriteCounter(stream, counter_indent, "gaussian_attribute_generation_count",
      count.gaussian_attribute_generation_count);
  WriteCounter(stream, counter_indent, "gaussian_upload_bytes",
      count.gaussian_upload_bytes);
  WriteCounter(stream, counter_indent, "upload_bytes", count.upload_bytes);
  WriteCounter(stream, counter_indent, "vertex_upload_bytes",
      count.vertex_upload_bytes);
  WriteCounter(stream, counter_indent, "index_upload_bytes",
      count.index_upload_bytes);
  WriteCounter(stream, counter_indent, "texture_upload_bytes",
      count.texture_upload_bytes);
  WriteCounter(stream, counter_indent, "gpu_scene_upload_bytes",
      count.gpu_scene_upload_bytes);
  WriteCounter(stream, counter_indent, "gpu_scene_copy_range_count",
      count.gpu_scene_copy_range_count);
  WriteCounter(stream, counter_indent,
      "gpu_scene_upload_ring_reserved_bytes",
      count.gpu_scene_upload_ring_reserved_bytes);
  WriteCounter(stream, counter_indent, "gpu_scene_upload_ring_growth_count",
      count.gpu_scene_upload_ring_growth_count);
  WriteCounter(stream, counter_indent, "gpu_scene_upload_ring_growth_bytes",
      count.gpu_scene_upload_ring_growth_bytes);
  WriteCounter(stream, counter_indent, "gpu_scene_draw_count",
      count.gpu_scene_draw_count);
  WriteCounter(stream, counter_indent, "gpu_driven_candidate_draw_count",
      count.gpu_driven_candidate_draw_count);
  WriteCounter(stream, counter_indent, "gpu_driven_visible_draw_count",
      count.gpu_driven_visible_draw_count);
  WriteCounter(stream, counter_indent,
      "gpu_driven_visibility_mask_culled_count",
      count.gpu_driven_visibility_mask_culled_count);
  WriteCounter(stream, counter_indent,
      "gpu_driven_frustum_culled_count",
      count.gpu_driven_frustum_culled_count);
  WriteCounter(stream, counter_indent, "gpu_driven_indirect_draw_count",
      count.gpu_driven_indirect_draw_count);
  WriteCounter(stream, counter_indent, "gpu_driven_candidate_upload_bytes",
      count.gpu_driven_candidate_upload_bytes);
  WriteCounter(stream, counter_indent, "mesh_cpu_draw_visit_count",
      count.mesh_cpu_draw_visit_count);
  WriteCounter(stream, counter_indent, "gpu_driven_fallback_count",
      count.gpu_driven_fallback_count);
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
  WriteCounter(stream, counter_indent, "generated_material_draw_count",
      count.generated_material_draw_count);
  WriteCounter(stream, counter_indent, "generated_material_fallback_count",
      count.generated_material_fallback_count);
  WriteCounter(stream, counter_indent, "material_fallback_recorded_count",
      count.material_fallbacks.recorded_count);
  WriteCounter(stream, counter_indent, "material_simplification_count",
      count.material_fallbacks.simplification_count);
  WriteCounter(stream, counter_indent, "material_basic_fallback_count",
      count.material_fallbacks.basic_material_count);
  WriteCounter(stream, counter_indent, "material_error_fallback_count",
      count.material_fallbacks.error_material_count);
  stream << counter_indent << "\"material_effective_fallback\": ";
  JsonString(stream, merlin::MaterialFallbackName(
                         count.material_fallbacks.effective_fallback));
  stream << ",\n";
  WriteCounter(stream, counter_indent,
      "bindless_sampled_image_descriptor_update_count",
      count.bindless_sampled_image_descriptor_update_count);
  WriteCounter(stream, counter_indent,
      "bindless_sampler_descriptor_update_count",
      count.bindless_sampler_descriptor_update_count);
  WriteCounter(stream, counter_indent, "transfer_submission_count",
      count.transfer_submission_count);
  WriteCounter(stream, counter_indent, "queue_ownership_transfer_count",
      count.queue_ownership_transfer_count, true);
  stream << indent << "  }\n"
         << indent << '}';
}

void WriteJson(std::ostream& stream, const Arguments& arguments,
    const FixtureSummary& fixture,
    const merlin::vulkan::RendererCapabilities& capabilities,
    const merlin::vulkan::RendererStatistics& statistics,
    const std::vector<Baseline>& baselines, const MeshVerification& verification,
    const std::vector<GaussianComparison>& gaussian_comparisons) {
  const auto& textures = statistics.bindless_texture_slots;
  const auto& samplers = statistics.bindless_samplers;
  const auto& memory = statistics.memory_budget;
  const auto& transfer = statistics.transfer_queue;
  stream << "{\n  \"schema\": \"merlin-benchmark/v3\",\n"
         << "  \"mesh_verification\": {\n    \"exact_aov_comparisons\": "
         << verification.exact_aov_comparisons
         << ",\n    \"required_submission_rejections\": " << verification.required_submission_rejections
         << ",\n    \"generated_parameter_recovery\": " << (verification.generated_parameter_recovery ? "true" : "false")
         << ",\n    \"generated_module_recovery\": " << (verification.generated_module_recovery ? "true" : "false")
         << ",\n    \"required_rejection_reason\": ";
  JsonString(stream, verification.required_rejection_reason);
  stream << "\n  },\n  \"gaussian_verification\": {\n"
         << "    \"camera_sequence\": \"translation-sine-reset-per-path/v1\",\n"
         << "    \"comparisons\": [";
  for (std::size_t i = 0; i < gaussian_comparisons.size(); ++i) {
    const auto& comparison = gaussian_comparisons[i];
    stream << (i == 0 ? "\n" : ",\n") << "      {\"name\": ";
    JsonString(stream, comparison.name);
    stream << ", \"max_color_channel_error\": " << comparison.max_color_channel_error
           << ", \"depth_pixels\": " << comparison.depth_pixels
           << ", \"prim_id_pixels\": " << comparison.prim_id_pixels
           << ", \"instance_id_pixels\": " << comparison.instance_id_pixels
           << ", \"passed\": " << (comparison.passed ? "true" : "false") << '}';
  }
  stream << "\n    ]\n  },\n  \"environment\": {\n    \"commit\": ";
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
         << ",\n    \"draw_indirect_first_instance\": "
         << (capabilities.draw_indirect_first_instance ? "true" : "false")
         << ",\n    \"draw_indirect_count\": "
         << (capabilities.draw_indirect_count ? "true" : "false")
         << ",\n    \"shader_draw_parameters\": "
         << (capabilities.shader_draw_parameters ? "true" : "false")
         << ",\n    \"generated_materials\": "
         << (capabilities.generated_materials ? "true" : "false")
         << ",\n    \"validation_enabled\": "
         << (capabilities.validation_enabled ? "true" : "false")
         << ",\n    \"validation_messages\": " << statistics.validation_messages
         << ",\n    \"async_transfer_queue\": "
         << (capabilities.async_transfer_queue ? "true" : "false")
         << ",\n    \"memory_budget_extension\": "
         << (capabilities.memory_budget_extension ? "true" : "false")
         << "\n  },\n  \"fixture\": {\n    \"name\": ";
  JsonString(stream, fixture.name);
  stream << ",\n    \"arena_vertex_blocks\": "
         << (arguments.fixture == "gpu-driven-arena-objects" ? arguments.arena_blocks : 0U)
         << ",\n    \"mesh_count\": " << fixture.mesh_count
         << ",\n    \"instance_count\": " << fixture.instance_count
         << ",\n    \"triangle_count\": " << fixture.triangle_count
         << ",\n    \"gaussian_resource_count\": "
         << fixture.gaussian_resource_count
         << ",\n    \"gaussian_particle_count\": "
         << fixture.gaussian_particle_count
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
  stream << "\n    },\n    \"gaussian_attributes\": {\n"
         << "      \"resources\": "
         << statistics.gaussian_attribute_resources
         << ",\n      \"pending_range_retirements\": "
         << statistics.pending_gaussian_attribute_retirements
         << ",\n      \"range_retirement_collections\": "
         << statistics.gaussian_attribute_range_retirements
         << ",\n      \"arena\": ";
  WriteArenaTelemetry(stream, statistics.gaussian_attribute_arena, "      ");
  stream << ",\n      \"upload_ring\": ";
  WriteUploadRingTelemetry(stream, statistics.gaussian_attribute_upload_ring,
      "      ");
  stream << "\n    },\n    \"upload_ring\": ";
  WriteUploadRingTelemetry(stream, statistics.upload_ring, "    ");
  stream << ",\n    \"gpu_scene\": {\n"
         << "      \"buffers\": "
         << (statistics.gpu_scene_buffers ? "true" : "false")
         << ",\n      \"capacity_bytes\": "
         << statistics.gpu_scene_capacity_bytes
         << ",\n      \"upload_ring\": ";
  WriteUploadRingTelemetry(stream, statistics.gpu_scene_upload_ring, "      ");
  stream << "\n    }";
  stream << ",\n    \"transfer_queue\": {\n"
         << "      \"asynchronous\": "
         << (transfer.asynchronous ? "true" : "false")
         << ",\n      \"graphics_family\": " << transfer.graphics_family
         << ",\n      \"transfer_family\": " << transfer.transfer_family
         << ",\n      \"submissions\": " << transfer.submission_count
         << ",\n      \"uploaded_bytes\": " << transfer.uploaded_bytes
         << ",\n      \"ownership_transfers\": "
         << transfer.ownership_transfer_count
         << ",\n      \"latest_timeline_value\": "
         << transfer.latest_timeline_value
         << "\n    },\n    \"memory_budget\": {\n"
         << "      \"extension_available\": "
         << (memory.extension_available ? "true" : "false")
         << ",\n      \"heap_capacity_bytes\": "
         << memory.heap_capacity_bytes
         << ",\n      \"heap_budget_bytes\": " << memory.heap_budget_bytes
         << ",\n      \"heap_usage_bytes\": " << memory.heap_usage_bytes
         << ",\n      \"heap_available_bytes\": "
         << memory.heap_available_bytes
         << ",\n      \"configured_limit_bytes\": "
         << memory.configured_limit_bytes
         << ",\n      \"effective_limit_bytes\": "
         << memory.effective_limit_bytes
         << ",\n      \"renderer_allocated_bytes\": "
         << memory.renderer_allocated_bytes
         << ",\n      \"renderer_peak_allocated_bytes\": "
         << memory.renderer_peak_allocated_bytes
         << ",\n      \"allocations\": " << memory.allocation_count
         << ",\n      \"releases\": " << memory.release_count
         << ",\n      \"exhaustions\": " << memory.exhaustion_count
         << ",\n      \"queries\": " << memory.query_count
         << "\n    },\n    \"textures\": {\n"
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
    const Products& products,
    std::shared_ptr<const merlin::render::GpuScenePackedFrameUpdate>
        gpu_scene_update = {},
    merlin::vulkan::GpuDrivenIndexedMode gpu_driven_mode =
        merlin::vulkan::GpuDrivenIndexedMode::Disabled,
    GaussianExecution gaussian_execution = GaussianExecution::Cpu) {
  merlin::vulkan::RenderRequest request;
  request.snapshot = extractor.snapshot();
  request.width = arguments.width;
  request.height = arguments.height;
  request.shaders = shaders;
  request.products = products;
  request.gpu_scene_update = std::move(gpu_scene_update);
  request.gpu_driven_indexed.mode = gpu_driven_mode;
  if (gaussian_execution != GaussianExecution::Cpu) {
    request.gpu_driven_gaussian_preparation =
        merlin::vulkan::GpuDrivenGaussianPreparationMode::Require;
    request.gpu_driven_gaussian_sort =
        merlin::vulkan::GpuDrivenGaussianSortMode::Require;
    request.gpu_driven_gaussian_raster =
        merlin::vulkan::GpuDrivenGaussianRasterMode::Require;
  }
  if (gaussian_execution == GaussianExecution::GpuTiled) {
    request.gpu_driven_gaussian_tiles =
        merlin::vulkan::GpuDrivenGaussianTileMode::Require;
    request.gpu_driven_gaussian_tile_raster =
        merlin::vulkan::GpuDrivenGaussianTileRasterMode::Require;
  }
  return renderer.Resolve(renderer.Submit(request));
}

void AssertStatic(const merlin::vulkan::FrameCounters& counters,
    std::string_view baseline = {}) {
  if (counters.upload_bytes != 0 || counters.allocation_count != 0 ||
      counters.pipeline_creation_count != 0 ||
      counters.shader_module_cache_misses != 0 ||
      counters.geometry_cache_misses != 0 ||
      counters.descriptor_pool_creation_count != 0 ||
      counters.descriptor_allocation_count != 0 ||
      counters.descriptor_update_count != 0 ||
      counters.bindless_sampled_image_descriptor_update_count != 0 ||
      counters.bindless_sampler_descriptor_update_count != 0) {
    std::ostringstream message;
    message << "static frame";
    if (!baseline.empty()) {
      message << " '" << baseline << '\'';
    }
    message << " performed forbidden work: upload=" << counters.upload_bytes
            << " allocation=" << counters.allocation_count
            << " pipeline=" << counters.pipeline_creation_count
            << " shader_miss=" << counters.shader_module_cache_misses
            << " geometry_miss=" << counters.geometry_cache_misses
            << " descriptor_pool="
            << counters.descriptor_pool_creation_count
            << " descriptor_allocation="
            << counters.descriptor_allocation_count
            << " descriptor_update=" << counters.descriptor_update_count
            << " bindless_image_update="
            << counters.bindless_sampled_image_descriptor_update_count
            << " bindless_sampler_update="
            << counters.bindless_sampler_descriptor_update_count;
    throw std::runtime_error(message.str());
  }
}

template <typename Image>
void RequireSameImage(const Image& conventional, const Image& gpu_driven,
    std::string_view name, std::uint32_t draw_count) {
  if (conventional.product != gpu_driven.product ||
      conventional.row_pitch_bytes != gpu_driven.row_pitch_bytes ||
      conventional.pixels != gpu_driven.pixels) {
    throw std::runtime_error(
        "GPU-driven " + std::string(name) + " output differs from " +
        "conventional submission at " + std::to_string(draw_count) +
        " draws");
  }
}

void RequireSameOutput(const merlin::vulkan::RenderResult& conventional,
    const merlin::vulkan::RenderResult& gpu_driven,
    std::uint32_t draw_count, bool same_revision = true) {
  if (conventional.rendered_aovs != gpu_driven.rendered_aovs ||
      conventional.cpu_readback_aovs != gpu_driven.cpu_readback_aovs ||
      (same_revision && conventional.scene_revision != gpu_driven.scene_revision)) {
    throw std::runtime_error(
        "GPU-driven render-product metadata differs from conventional "
        "submission at " +
        std::to_string(draw_count) + " draws");
  }
  if (std::none_of(conventional.instance_id.pixels.begin(),
          conventional.instance_id.pixels.end(),
          [](std::uint32_t id) { return id != ~std::uint32_t{}; })) {
    throw std::runtime_error("Mesh parity comparison has no rasterized foreground");
  }
  RequireSameImage(conventional.color, gpu_driven.color, "color", draw_count);
  RequireSameImage(conventional.depth, gpu_driven.depth, "depth", draw_count);
  RequireSameImage(conventional.prim_id, gpu_driven.prim_id, "primId",
      draw_count);
  RequireSameImage(conventional.instance_id, gpu_driven.instance_id,
      "instanceId", draw_count);
}

GaussianComparison CompareGaussianOutput(std::string name,
    const merlin::vulkan::RenderResult& reference,
    const merlin::vulkan::RenderResult& candidate, GaussianExecution execution) {
  if (reference.color.product != candidate.color.product ||
      reference.color.row_pitch_bytes != candidate.color.row_pitch_bytes ||
      reference.color.pixels.size() != candidate.color.pixels.size() ||
      reference.depth.product != candidate.depth.product ||
      reference.depth.pixels.size() != candidate.depth.pixels.size() ||
      reference.prim_id.product != candidate.prim_id.product ||
      reference.prim_id.pixels.size() != candidate.prim_id.pixels.size() ||
      reference.instance_id.product != candidate.instance_id.product ||
      reference.instance_id.pixels.size() != candidate.instance_id.pixels.size() ||
      std::none_of(reference.instance_id.pixels.begin(), reference.instance_id.pixels.end(),
          [](std::uint32_t id) { return id != ~std::uint32_t{}; })) {
    throw std::runtime_error("Gaussian comparison has different products or no foreground");
  }
  GaussianComparison result{std::move(name)};
  for (std::size_t i = 0; i < reference.color.pixels.size(); ++i) {
    const auto error = std::abs(static_cast<int>(reference.color.pixels[i]) -
                                static_cast<int>(candidate.color.pixels[i]));
    result.max_color_channel_error = std::max(result.max_color_channel_error,
        static_cast<std::uint32_t>(error));
  }
  for (std::size_t i = 0; i < reference.depth.pixels.size(); ++i) {
    result.depth_pixels += reference.depth.pixels[i] != candidate.depth.pixels[i];
    result.prim_id_pixels += reference.prim_id.pixels[i] != candidate.prim_id.pixels[i];
    result.instance_id_pixels += reference.instance_id.pixels[i] != candidate.instance_id.pixels[i];
  }
  // Existing raster tolerances: UNorm sorted-stream rounding and float tile
  // composition. Rare depth-tied particle IDs can change at the cutoff rim.
  const auto tolerance = execution == GaussianExecution::GpuTiled ? 6U : 2U;
  result.passed = !(result.max_color_channel_error > tolerance || result.depth_pixels != 0 ||
      result.prim_id_pixels != 0 ||
      (execution == GaussianExecution::GpuSortedStream && result.instance_id_pixels != 0) ||
      result.instance_id_pixels * 100U > reference.depth.pixels.size());
  // Keep the full capture for diagnosing a failure. The executable returns
  // failure after writing the report, so this cannot silently pass a gate.
  return result;
}

void RequireGaussianGpuFrame(const merlin::vulkan::RenderResult& result,
    GaussianExecution execution) {
  const auto& c = result.counters;
  if (c.upload_bytes != 0 || c.gaussian_upload_bytes != 0 ||
      c.gaussian_attribute_upload_bytes != 0 || c.allocation_count != 0 ||
      c.pipeline_creation_count != 0 || c.gaussian_cpu_preparation_skipped_count != 1 ||
      result.cpu_timings.gaussian_preparation_ns != 0 ||
      c.gaussian_gpu_sorted_count == 0 || c.gaussian_gpu_sorted_count != c.gaussian_visible_count ||
      c.gaussian_gpu_preparation_fallback_count != 0 || c.gaussian_gpu_sort_fallback_count != 0 ||
      c.gaussian_gpu_raster_fallback_count != 0 || c.gaussian_gpu_tile_fallback_count != 0 ||
      c.gaussian_gpu_tile_raster_fallback_count != 0 ||
      c.gaussian_gpu_tile_raster_overflow_fallback_count != 0 ||
      c.gaussian_gpu_sort_reference_divergence_count != 0 ||
      c.gaussian_gpu_tile_reference_divergence_count != 0 ||
      (execution == GaussianExecution::GpuTiled && c.gaussian_gpu_tile_raster_frame_count != 1) ||
      (execution == GaussianExecution::GpuSortedStream && c.gaussian_gpu_raster_indirect_draw_count != 2)) {
    throw std::runtime_error("Gaussian GPU sample uploaded, traversed, allocated, or fell back");
  }
}

} // namespace

int main(int argc, char** argv) {
  try {
    const auto arguments = ParseArguments(argc, argv);
    const auto executable_dir = std::filesystem::absolute(argv[0]).parent_path();
    const auto shader_dir =
        executable_dir / merlin::vulkan::shader_abi::ArtifactDirectory();
    const merlin::vulkan::ShaderPaths shaders{
        shader_dir / "triangle.vert.spv",
        shader_dir / "triangle.frag.spv",
        shader_dir / "triangle.bindless.vert.spv",
        shader_dir / "triangle.bindless.frag.spv",
        shader_dir / "environment.hdr",
        shader_dir / "gaussian.vert.spv",
        shader_dir / "gaussian.frag.spv"};
    const bool generated_fixture = arguments.fixture == "generated-material-objects";
    const bool arena_fixture = arguments.fixture == "gpu-driven-arena-objects";
    const bool textured_fixture = arena_fixture ||
                                  arguments.fixture == "gpu-driven-textured-objects";
    const bool diverse_fixture = generated_fixture || textured_fixture ||
                                 arguments.fixture == "gpu-driven-diverse-objects";
    const bool gpu_driven_fixture = diverse_fixture ||
        arguments.fixture == "gpu-driven-small-objects";
    constexpr std::uint32_t kGpuDrivenMaximumDrawCount = 100'000;
    const merlin::render::GpuScenePackingCapacities
        gpu_driven_capacities{diverse_fixture ? kDiverseMeshCount : 1U,
            kGpuDrivenMaximumDrawCount,
            diverse_fixture ? kDiverseMaterialCount : 1U,
            kGpuDrivenMaximumDrawCount};
    merlin::vulkan::RendererOptions renderer_options;
    renderer_options.enable_validation = arguments.validation;
    if (generated_fixture) {
      renderer_options.descriptor_backend = merlin::vulkan::DescriptorBackendRequest::Conventional;
      renderer_options.generated_material_artifacts.push_back(ScaleMaterialArtifact(shader_dir));
    }
    if (gpu_driven_fixture && !generated_fixture) {
      renderer_options.gpu_scene_capacities = gpu_driven_capacities;
    }
    merlin::vulkan::Renderer renderer(renderer_options);
    merlin::extraction::SceneExtractor extractor;
    std::vector<Baseline> baselines;
    MeshVerification verification;
    std::vector<GaussianComparison> gaussian_comparisons;
    FixtureSummary fixture_summary;
    std::unique_ptr<merlin::render::GpuScenePackingState> gpu_scene_packing;
    std::vector<merlin::render::GpuGeometryPlacement> gpu_geometry_placements;
    std::vector<merlin::render::GpuInstanceIdentity> gpu_instance_identities;
    std::vector<merlin::render::GpuMaterialBinding> gpu_material_bindings;
    std::uint64_t last_completion_value{};
    auto gpu_driven_mode = merlin::vulkan::GpuDrivenIndexedMode::Disabled;
    auto gaussian_execution = GaussianExecution::Cpu;
    if (gpu_driven_fixture && !generated_fixture) {
      gpu_scene_packing =
          std::make_unique<merlin::render::GpuScenePackingState>(
              gpu_driven_capacities);
      // These immutable fixtures are the first allocations in both arenas.
      // Vulkan reserves geometry ranges at 16-byte boundaries; four packed
      // vertices already meet that alignment. Exact Forward parity below
      // guards these fixture-specific placement assumptions.
      static_assert((4U * sizeof(merlin::extraction::DrawVertex)) % 16U == 0);
      std::uint64_t index_offset{};
      for (std::uint32_t index = 0;
          index < gpu_driven_capacities.geometries; ++index) {
        gpu_geometry_placements.push_back(
            {arena_fixture
                    ? (index % (kDiverseMeshCount / arguments.arena_blocks)) *
                          ArenaFixtureVertexCount(arguments.arena_blocks) *
                          sizeof(merlin::extraction::DrawVertex)
                    : index * 4U * sizeof(merlin::extraction::DrawVertex),
                index_offset});
        index_offset += index % 2U == 0 ? 16U : 32U;
      }
      gpu_material_bindings.resize(gpu_driven_capacities.materials);
      if (textured_fixture) {
        // Fresh tables allocate ascending physical slots after reserved entries.
        // Exact comparison with a renderer without GPU Scene below guards this.
        for (std::uint32_t index = 0; index < gpu_driven_capacities.materials; ++index) {
          gpu_material_bindings[index] = {
              merlin::vulkan::kReservedBindlessTextureSlots + index % 4U,
              index % 2U};
        }
      }
    }

    const auto render = [&](const Products& products = AllProducts()) {
      std::shared_ptr<const merlin::render::GpuScenePackedFrameUpdate> update;
      if (gpu_scene_packing) {
        update =
            std::make_shared<const merlin::render::GpuScenePackedFrameUpdate>(
                gpu_scene_packing->Apply(
                    *extractor.snapshot(), last_completion_value,
                    last_completion_value,
                    {gpu_geometry_placements, gpu_instance_identities,
                        gpu_material_bindings}));
      }
      auto result = Render(renderer, extractor, shaders, arguments, products,
          std::move(update), gpu_driven_mode, gaussian_execution);
      last_completion_value = result.completion_value;
      return result;
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
      merlin::vulkan::RenderResult last_result;
      for (std::uint32_t frame = 0; frame < arguments.steady_frames; ++frame) {
        const auto start = CpuClock::now();
        auto result = render();
        if (gaussian_execution != GaussianExecution::Cpu) {
          RequireGaussianGpuFrame(result, gaussian_execution);
        }
        auto timing = FromBackend(result.cpu_timings);
        timing.total_frame_ns = ElapsedNanoseconds(start);
        samples.push_back(timing);
        if (frame == 0) {
          counters = result.counters;
        } else if (result.counters != counters) {
          throw std::runtime_error("steady-state structural counters changed");
        }
        last_result = std::move(result);
      }
      if (gaussian_execution == GaussianExecution::Cpu) {
        AssertStatic(counters, name);
      }
      baselines.push_back(
          {std::move(name), std::move(samples), counters, {}});
      return last_result;
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
              (std::uint64_t{1} << static_cast<std::uint32_t>(merlin::Aov::Color))) {
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
    } else if (gpu_driven_fixture) {
      ScaleFixture fixture;
      merlin::CameraDescriptor camera;
      camera.label = "mesh-motion-camera";
      fixture.camera = fixture.world.CreateCamera(std::move(camera));
      extractor.Apply(fixture.world, fixture.world.Commit());
      extractor.SetActiveCamera(fixture.camera);
      const auto warm_path = [&](merlin::vulkan::GpuDrivenIndexedMode mode) {
        gpu_driven_mode = mode;
        for (std::uint32_t frame = 0;
            frame < renderer.statistics().frame_context_count; ++frame) {
          (void)render();
        }
      };
      constexpr std::array draw_counts{1'000U, 10'000U, 100'000U};
      for (const auto draw_count : draw_counts) {
        gpu_instance_identities.reserve(draw_count);
        while (gpu_instance_identities.size() < draw_count) {
          const auto index = static_cast<std::uint32_t>(
              gpu_instance_identities.size());
          gpu_instance_identities.push_back(
              {index + 1U, index + 1U, ~std::uint32_t{}, 0U});
        }

        gpu_driven_mode = merlin::vulkan::GpuDrivenIndexedMode::Disabled;
        measure("update-" + std::to_string(draw_count), fixture.world, [&] {
          auto descriptor = fixture.world.Get(fixture.camera);
          descriptor.view.values[12] = 0.0F;
          fixture.world.UpdateCamera(fixture.camera, std::move(descriptor),
              merlin::ChangeAspect::Camera);
          fixture_summary =
              PopulateGpuDrivenObjects(fixture, draw_count, diverse_fixture,
                  textured_fixture, arena_fixture ? arguments.arena_blocks : 0,
                  generated_fixture);
          fixture_summary.name = arguments.fixture;
          for (std::size_t index = 0; index < fixture.instances.size(); ++index) {
            const auto handle = fixture.instances[index];
            gpu_instance_identities[index] = {
                static_cast<std::uint32_t>(fixture.world.Get(handle).mesh.value()),
                static_cast<std::uint32_t>(handle.value()), ~std::uint32_t{}, 0U};
          }
        });

        warm_path(merlin::vulkan::GpuDrivenIndexedMode::Disabled);
        const auto conventional =
            steady("conventional-" + std::to_string(draw_count));
        if (baselines.back().counters.gpu_driven_candidate_draw_count != 0 ||
            baselines.back().counters.gpu_driven_indirect_draw_count != 0) {
          throw std::runtime_error(
              "conventional scale baseline selected GPU-driven submission");
        }

        if (generated_fixture) {
          if (conventional.counters.generated_material_draw_count != draw_count ||
              conventional.counters.generated_material_fallback_count != 0 ||
              !conventional.material_diagnostics.empty()) {
            throw std::runtime_error("generated ABI fixture did not execute its material artifact");
          }
          gpu_driven_mode = merlin::vulkan::GpuDrivenIndexedMode::Require;
          bool rejected{};
          try {
            (void)render();
          } catch (const merlin::vulkan::RendererError& error) {
            if (error.code() != merlin::vulkan::RendererErrorCode::Unsupported ||
                std::string(error.what()).find("persistent bindless GPU Scene state is unavailable") == std::string::npos) {
              throw;
            }
            rejected = true;
            ++verification.required_submission_rejections;
            verification.required_rejection_reason = error.what();
          }
          if (!rejected) {
            throw std::runtime_error("generated conventional fixture unexpectedly accepted required GPU submission");
          }
        }
        warm_path(generated_fixture ? merlin::vulkan::GpuDrivenIndexedMode::Prefer
                                    : merlin::vulkan::GpuDrivenIndexedMode::Require);
        const auto gpu_driven =
            steady(std::string(generated_fixture ? "prefer-fallback-" : "gpu-driven-") +
                   std::to_string(draw_count));
        const auto& counters = baselines.back().counters;
        // Extraction orders draws by material/mesh sort key. Repeated
        // instances of each pair stay contiguous, so resource diversity
        // bounds batch count independently of the number of instances.
        const auto maximum_batches = diverse_fixture
            ? kDiverseMeshCount * kDiverseMaterialCount : 1U;
        if (generated_fixture) {
          if (counters.gpu_driven_candidate_draw_count != 0 ||
              counters.gpu_driven_indirect_draw_count != 0 ||
              counters.gpu_driven_fallback_count != 1 ||
              counters.generated_material_draw_count != draw_count ||
              counters.generated_material_fallback_count != 0 ||
              counters.mesh_cpu_draw_visit_count != 0) {
            throw std::runtime_error("generated material fallback violated its explicit submission contract");
          }
        } else if (counters.gpu_driven_candidate_draw_count != draw_count ||
                   counters.gpu_driven_visible_draw_count != draw_count ||
                   counters.gpu_driven_indirect_draw_count > maximum_batches ||
                   counters.gpu_driven_indirect_draw_count < (diverse_fixture ? 2U : 1U) ||
                   counters.mesh_cpu_draw_visit_count != 0 ||
                   counters.gpu_driven_candidate_upload_bytes != 0 ||
                   counters.gpu_driven_fallback_count != 0) {
          throw std::runtime_error(
              "GPU-driven scale baseline violated bounded steady-state "
              "submission at " + std::to_string(draw_count) + " draws: " +
              "batches=" + std::to_string(counters.gpu_driven_indirect_draw_count) +
              ", CPU visits=" + std::to_string(counters.mesh_cpu_draw_visit_count));
        }
        const auto expected_batches = counters.gpu_driven_indirect_draw_count;
        if (arena_fixture && (renderer.statistics().vertex_arena.blocks != arguments.arena_blocks ||
                                 renderer.statistics().index_arena.blocks != 1)) {
          throw std::runtime_error("arena fixture did not create the requested vertex blocks and one index block");
        }
        if (textured_fixture) {
          merlin::vulkan::RendererOptions reference_options;
          reference_options.enable_validation = arguments.validation;
          merlin::vulkan::Renderer reference_renderer(reference_options);
          RequireSameOutput(Render(reference_renderer, extractor, shaders,
                                arguments, AllProducts()),
              gpu_driven, draw_count);
          ++verification.exact_aov_comparisons;
          if (reference_renderer.statistics().validation_messages != 0) {
            throw std::runtime_error("independent Forward reference reported validation diagnostics");
          }
        }
        RequireSameOutput(conventional, gpu_driven, draw_count);
        ++verification.exact_aov_comparisons;

        std::vector<FrameTimings> motion_samples;
        motion_samples.reserve(arguments.steady_frames);
        merlin::vulkan::RenderResult motion_result;
        for (std::uint32_t frame = 0; frame < arguments.steady_frames; ++frame) {
          const auto start = CpuClock::now();
          const auto extraction_start = CpuClock::now();
          auto descriptor = fixture.world.Get(fixture.camera);
          descriptor.view.values[12] =
              0.05F * std::sin(0.1F * static_cast<float>(frame + 1));
          fixture.world.UpdateCamera(fixture.camera, std::move(descriptor),
              merlin::ChangeAspect::Camera);
          extractor.Apply(fixture.world, fixture.world.Commit());
          const auto extraction_ns = ElapsedNanoseconds(extraction_start);
          motion_result = render();
          auto timing = FromBackend(motion_result.cpu_timings);
          timing.extraction_ns = extraction_ns;
          timing.total_frame_ns = ElapsedNanoseconds(start);
          motion_samples.push_back(timing);
          AssertStatic(motion_result.counters);
          if (motion_result.counters.mesh_cpu_draw_visit_count != 0 ||
              motion_result.counters.gpu_driven_candidate_draw_count != (generated_fixture ? 0U : draw_count) ||
              motion_result.counters.gpu_driven_indirect_draw_count != expected_batches ||
              motion_result.counters.gpu_driven_fallback_count != (generated_fixture ? 1U : 0U) ||
              motion_result.counters.gpu_driven_candidate_upload_bytes != 0 ||
              (generated_fixture &&
                  (motion_result.counters.generated_material_draw_count != draw_count ||
                      motion_result.counters.generated_material_fallback_count != 0))) {
            throw std::runtime_error(
                "camera motion rebuilt GPU-driven Mesh submission");
          }
        }
        baselines.push_back({std::string(generated_fixture
                                             ? "camera-motion-prefer-fallback-"
                                             : "camera-motion-gpu-driven-") +
                                 std::to_string(draw_count),
            std::move(motion_samples), motion_result.counters,
            extractor.snapshot()->build_counters});
        gpu_driven_mode = merlin::vulkan::GpuDrivenIndexedMode::Disabled;
        RequireSameOutput(render(), motion_result, draw_count);
        ++verification.exact_aov_comparisons;
        if (textured_fixture) {
          merlin::vulkan::Renderer reference_renderer(merlin::vulkan::RendererOptions{.enable_validation = arguments.validation});
          RequireSameOutput(Render(reference_renderer, extractor, shaders,
                                arguments, AllProducts()),
              motion_result, draw_count);
          ++verification.exact_aov_comparisons;
          if (reference_renderer.statistics().validation_messages != 0) {
            throw std::runtime_error("moving Forward reference reported validation diagnostics");
          }
        }
        if (generated_fixture && draw_count == 1'000U) {
          // Edits must invalidate both the preflight plan and completed frame
          // descriptor caches. Restoring a missing module must restore execution.
          const auto material_handle = fixture.materials.at(1);
          const auto original = fixture.world.Get(material_handle);
          const auto apply_material = [&](const merlin::MaterialDescriptor& material) {
            fixture.world.UpdateMaterial(material_handle, material);
            extractor.Apply(fixture.world, fixture.world.Commit());
            return render();
          };
          const auto require_reused = [&] {
            warm_path(merlin::vulkan::GpuDrivenIndexedMode::Disabled);
            auto result = render();
            AssertStatic(result.counters);
            if (result.counters.mesh_cpu_draw_visit_count != 0 ||
                result.counters.generated_material_draw_count != draw_count ||
                result.counters.generated_material_fallback_count != 0) {
              throw std::runtime_error("generated material cache did not recover after edit");
            }
            return result;
          };
          auto edited = original;
          edited.generated_parameters.key += "-edited";
          edited.generated_parameters.entries[0].values[0] = merlin::Vec3{0.95F, 0.1F, 0.15F};
          const auto changed = apply_material(edited);
          if (changed.counters.mesh_cpu_draw_visit_count == 0 ||
              changed.color.pixels == motion_result.color.pixels) {
            throw std::runtime_error("generated parameter edit failed to invalidate/render");
          }
          (void)require_reused();
          (void)apply_material(original);
          RequireSameOutput(require_reused(), motion_result, draw_count, false);
          verification.generated_parameter_recovery = true;

          edited = original;
          edited.module->key = "missing-scale-artifact";
          const auto missing = apply_material(edited);
          if (missing.counters.generated_material_fallback_count == 0 ||
              missing.material_diagnostics.empty() ||
              missing.material_diagnostics.front().category != merlin::MaterialDiagnosticCategory::CacheIncompatible) {
            throw std::runtime_error("missing module did not take explicit material fallback");
          }
          (void)apply_material(original);
          RequireSameOutput(require_reused(), motion_result, draw_count, false);
          verification.generated_module_recovery = true;
        }
      }
    } else {
      ScaleFixture fixture;
      measure("first-frame", fixture.world, [&] {
        fixture_summary =
            PopulateScaleFixture(arguments.fixture, fixture);
      });
      steady("steady-state");
      if (fixture_summary.gaussian_particle_count != 0) {
        merlin::CameraDescriptor camera;
        camera.label = "gaussian-motion-camera";
        fixture.camera = fixture.world.CreateCamera(std::move(camera));
        extractor.Apply(fixture.world, fixture.world.Commit());
        extractor.SetActiveCamera(fixture.camera);
        const auto move_camera = [&](std::uint32_t step) {
          auto descriptor = fixture.world.Get(fixture.camera);
          descriptor.view.values[12] =
              0.05F * std::sin(0.1F * static_cast<float>(step));
          fixture.world.UpdateCamera(fixture.camera, std::move(descriptor),
              merlin::ChangeAspect::Camera);
          extractor.Apply(fixture.world, fixture.world.Commit());
        };
        merlin::vulkan::RenderResult static_reference;
        merlin::vulkan::RenderResult motion_reference;
        for (const auto execution : {GaussianExecution::Cpu,
                 GaussianExecution::GpuSortedStream, GaussianExecution::GpuTiled}) {
          gaussian_execution = execution;
          const auto path = std::string(GaussianExecutionName(execution));
          move_camera(0);
          // Warm every reusable context before checking zero-upload static work.
          for (std::uint32_t i = 0; i < renderer.statistics().frame_context_count; ++i) {
            (void)render();
          }
          auto static_result = steady("static-" + path);
          if (execution == GaussianExecution::Cpu) {
            static_reference = std::move(static_result);
          } else {
            RequireGaussianGpuFrame(static_result, execution);
            gaussian_comparisons.push_back(CompareGaussianOutput(
                "static-" + path, static_reference, static_result, execution));
          }
          move_camera(1);
          (void)render();
          std::vector<FrameTimings> samples;
          samples.reserve(arguments.steady_frames);
          merlin::vulkan::RenderResult last_result;
          for (std::uint32_t frame = 0; frame < arguments.steady_frames; ++frame) {
            const auto start = CpuClock::now();
            const auto extraction_start = CpuClock::now();
            // Reset the sequence per path; all policies see identical cameras.
            move_camera(frame + 2U);
            const auto extraction_ns = ElapsedNanoseconds(extraction_start);
            last_result = render();
            auto timing = FromBackend(last_result.cpu_timings);
            timing.extraction_ns = extraction_ns;
            timing.total_frame_ns = ElapsedNanoseconds(start);
            samples.push_back(timing);
            if (execution != GaussianExecution::Cpu) {
              RequireGaussianGpuFrame(last_result, execution);
            }
          }
          baselines.push_back({"camera-motion-" + path, std::move(samples),
              last_result.counters, extractor.snapshot()->build_counters});
          if (execution == GaussianExecution::Cpu) {
            motion_reference = std::move(last_result);
          } else {
            gaussian_comparisons.push_back(CompareGaussianOutput(
                "camera-motion-" + path, motion_reference, last_result, execution));
          }
        }
        gaussian_execution = GaussianExecution::Cpu;
      }
    }

    if (renderer.statistics().validation_messages != 0) {
      throw std::runtime_error("renderer validation diagnostics were reported");
    }
    if (arguments.output.empty()) {
      WriteJson(std::cout, arguments, fixture_summary, renderer.capabilities(),
          renderer.statistics(), baselines, verification, gaussian_comparisons);
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
          renderer.statistics(), baselines, verification, gaussian_comparisons);
      if (!stream) {
        throw std::runtime_error("could not write output: " +
                                 arguments.output.string());
      }
    }
    if (std::any_of(gaussian_comparisons.begin(), gaussian_comparisons.end(),
            [](const auto& comparison) { return !comparison.passed; })) {
      std::cerr << "merlin-benchmark: Gaussian image tolerance exceeded; see gaussian_verification in the completed report\n";
      return 1;
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "merlin-benchmark: " << error.what() << '\n';
    return 1;
  }
}
