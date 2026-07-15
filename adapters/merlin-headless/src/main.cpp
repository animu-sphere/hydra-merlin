#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/vulkan/render_artifacts.hpp>
#include <merlin/vulkan/renderer.hpp>

#include <vulkan/vulkan_core.h>

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Arguments {
  std::filesystem::path output{"merlin.ppm"};
  std::filesystem::path metadata;
  std::filesystem::path report;
  std::filesystem::path artifact_dir;
  std::uint32_t width{512};
  std::uint32_t height{512};
  std::uint32_t frames{3};
  bool validation{};
  bool probe_only{};
  bool install_tree{};
};

struct RendererCheck {
  std::string id;
  std::string status;
  std::string detail;
};

enum class ReportPhase {
  Arguments,
  Core,
  Backend,
  Frame,
  Reporting,
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
  bool width_explicit{};
  bool height_explicit{};
  bool frames_explicit{};
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
    } else if (argument == "--metadata") {
      result.metadata = require_value(argument);
    } else if (argument == "--report") {
      result.report = require_value(argument);
    } else if (argument == "--artifact-dir") {
      result.artifact_dir = require_value(argument);
    } else if (argument == "--width") {
      result.width = ParseUnsigned(require_value(argument), argument);
      width_explicit = true;
    } else if (argument == "--height") {
      result.height = ParseUnsigned(require_value(argument), argument);
      height_explicit = true;
    } else if (argument == "--frames") {
      result.frames = ParseUnsigned(require_value(argument), argument);
      frames_explicit = true;
    } else if (argument == "--validate") {
      result.validation = true;
    } else if (argument == "--probe") {
      result.probe_only = true;
    } else if (argument == "--install-tree") {
      result.install_tree = true;
    } else if (argument == "--help") {
      std::cout
          << "Usage: merlin-headless [--output FILE] [--metadata FILE] "
             "[--report FILE] [--artifact-dir DIR] [--width N] [--height N] [--frames N] "
             "[--validate] [--probe] [--install-tree]\n";
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown option: " + std::string(argument));
    }
  }
  if (!result.report.empty()) {
    result.validation = true;
    if (!width_explicit) {
      result.width = 64;
    }
    if (!height_explicit) {
      result.height = 64;
    }
    if (!frames_explicit) {
      result.frames = 1000;
    }
  }
  return result;
}

std::string VersionString(std::uint32_t version) {
  return std::to_string(VK_VERSION_MAJOR(version)) + "." +
         std::to_string(VK_VERSION_MINOR(version)) + "." +
         std::to_string(VK_VERSION_PATCH(version));
}

void WriteJsonString(std::ostream& stream, std::string_view value) {
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
          stream << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                 << static_cast<unsigned int>(character) << std::dec
                 << std::setfill(' ');
        } else {
          stream << character;
        }
    }
  }
  stream << '"';
}

void WriteMetadata(const std::filesystem::path& path,
                   const merlin::vulkan::RendererCapabilities& capabilities) {
  if (path.empty()) {
    return;
  }
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("could not create metadata: " + path.string());
  }
  const auto& descriptor_features = capabilities.descriptor_indexing_features;
  const auto& descriptor_limits = capabilities.descriptor_indexing_limits;
  const auto& descriptor_selection = capabilities.descriptor_indexing_selection;
  const auto fallback_category =
      merlin::vulkan::DescriptorFallbackReasonCategory(
          descriptor_selection.fallback_reason);
  stream << "{\n  \"schema_version\": 2,\n  \"vulkan\": {\n"
         << "    \"sdk_version\": ";
  WriteJsonString(stream, capabilities.sdk_version);
  stream << ",\n    \"header_version\": ";
  WriteJsonString(stream, VersionString(capabilities.header_version));
  stream << ",\n    \"loader_api_version\": ";
  WriteJsonString(stream, VersionString(capabilities.loader_api_version));
  stream << ",\n    \"device_api_version\": ";
  WriteJsonString(stream, VersionString(capabilities.api_version));
  stream << ",\n    \"device_name\": ";
  WriteJsonString(stream, capabilities.device_name);
  stream << ",\n    \"vendor_id\": " << capabilities.vendor_id
         << ",\n    \"device_id\": " << capabilities.device_id
         << ",\n    \"driver_version\": " << capabilities.driver_version
         << ",\n    \"driver_name\": ";
  WriteJsonString(stream, capabilities.driver_name);
  stream << ",\n    \"driver_info\": ";
  WriteJsonString(stream, capabilities.driver_info);
  stream << ",\n    \"timeline_semaphore\": "
         << (capabilities.timeline_semaphore ? "true" : "false")
         << ",\n    \"validation_enabled\": "
         << (capabilities.validation_enabled ? "true" : "false")
         << ",\n    \"descriptor_indexing\": {\n"
         << "      \"schema_version\": " << descriptor_selection.schema_version
         << ",\n      \"requested_backend\": ";
  WriteJsonString(stream, merlin::vulkan::DescriptorBackendRequestName(
                              descriptor_selection.requested_backend));
  stream << ",\n      \"selected_backend\": ";
  WriteJsonString(stream, merlin::vulkan::DescriptorBackendName(
                              descriptor_selection.selected_backend));
  stream << ",\n      \"fallback_category\": ";
  WriteJsonString(
      stream, merlin::vulkan::DescriptorFallbackCategoryName(fallback_category));
  stream << ",\n      \"fallback_reason\": ";
  WriteJsonString(stream, merlin::vulkan::DescriptorFallbackReasonName(
                              descriptor_selection.fallback_reason));
  stream << ",\n      \"requirements_satisfied\": "
         << (descriptor_selection.requirements_satisfied ? "true" : "false")
         << ",\n      \"texture_capacity\": "
         << descriptor_selection.texture_capacity
         << ",\n      \"sampler_capacity\": "
         << descriptor_selection.sampler_capacity
         << ",\n      \"additional_sampler_allocation_count\": "
         << descriptor_selection.additional_sampler_allocation_count
         << ",\n      \"additional_per_stage_resource_count\": "
         << descriptor_selection.additional_per_stage_resource_count
         << ",\n      \"features\": {\n"
         << "        \"shader_sampled_image_array_non_uniform_indexing\": "
         << (descriptor_features
                     .shader_sampled_image_array_non_uniform_indexing
                 ? "true"
                 : "false")
         << ",\n        \"descriptor_binding_sampled_image_update_after_bind\": "
         << (descriptor_features
                     .descriptor_binding_sampled_image_update_after_bind
                 ? "true"
                 : "false")
         << ",\n        \"descriptor_binding_partially_bound\": "
         << (descriptor_features.descriptor_binding_partially_bound ? "true"
                                                                    : "false")
         << ",\n        \"descriptor_binding_variable_descriptor_count\": "
         << (descriptor_features.descriptor_binding_variable_descriptor_count
                 ? "true"
                 : "false")
         << ",\n        \"runtime_descriptor_array\": "
         << (descriptor_features.runtime_descriptor_array ? "true" : "false")
         << "\n      },\n      \"limits\": {\n"
         << "        \"max_update_after_bind_descriptors_in_all_pools\": "
         << descriptor_limits.max_update_after_bind_descriptors_in_all_pools
         << ",\n        \"max_per_stage_descriptor_update_after_bind_samplers\": "
         << descriptor_limits
                .max_per_stage_descriptor_update_after_bind_samplers
         << ",\n        \"max_per_stage_descriptor_update_after_bind_sampled_images\": "
         << descriptor_limits
                .max_per_stage_descriptor_update_after_bind_sampled_images
         << ",\n        \"max_per_stage_update_after_bind_resources\": "
         << descriptor_limits.max_per_stage_update_after_bind_resources
         << ",\n        \"max_descriptor_set_update_after_bind_samplers\": "
         << descriptor_limits.max_descriptor_set_update_after_bind_samplers
         << ",\n        \"max_descriptor_set_update_after_bind_sampled_images\": "
         << descriptor_limits.max_descriptor_set_update_after_bind_sampled_images
         << ",\n        \"max_sampler_allocation_count\": "
         << descriptor_limits.max_sampler_allocation_count
         << "\n      }\n    }\n  }\n}\n";
  if (!stream) {
    throw std::runtime_error("could not write metadata: " + path.string());
  }
}

void WriteRendererReport(
    const std::filesystem::path& path,
    const std::optional<merlin::vulkan::RendererCapabilities>& capabilities,
    const std::vector<RendererCheck>& checks) {
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("could not create renderer report: " + path.string());
  }
  stream << "{\n  \"schema\": \"openstrata.renderer-report/v1alpha1\",\n"
         << "  \"renderer\": {\"name\": \"hdMerlin\"}";
  if (capabilities && !capabilities->device_name.empty()) {
    stream << ",\n  \"device\": {\n    \"backend\": \"vulkan\",\n"
           << "    \"name\": ";
    WriteJsonString(stream, capabilities->device_name);
    stream << ",\n    \"api_version\": ";
    WriteJsonString(stream, VersionString(capabilities->api_version));
    stream << ",\n    \"driver_version\": ";
    WriteJsonString(stream, std::to_string(capabilities->driver_version));
    stream << ",\n    \"vendor_id\": " << capabilities->vendor_id
           << ",\n    \"device_id\": " << capabilities->device_id << "\n  }";
  }
  stream << ",\n  \"checks\": [\n";
  for (std::size_t index = 0; index < checks.size(); ++index) {
    const auto& check = checks[index];
    stream << "    {\"id\":";
    WriteJsonString(stream, check.id);
    stream << ",\"status\":";
    WriteJsonString(stream, check.status);
    if (!check.detail.empty()) {
      stream << ",\"detail\":";
      WriteJsonString(stream, check.detail);
    }
    stream << '}' << (index + 1U == checks.size() ? "\n" : ",\n");
  }
  stream << "  ]\n}\n";
  if (!stream) {
    throw std::runtime_error("could not write renderer report: " + path.string());
  }
}

void AppendHydraSkips(std::vector<RendererCheck>& checks) {
  constexpr std::string_view detail =
      "Hydra 2 evidence requires the optional installed host test";
  checks.push_back({"renderer.plugin.discovery", "skip", std::string(detail)});
  checks.push_back({"renderer.delegate.creation", "skip", std::string(detail)});
  checks.push_back({"renderer.render_buffer.cpu", "skip", std::string(detail)});
  checks.push_back({"renderer.host.first_frame", "skip", std::string(detail)});
  checks.push_back({"renderer.host.stable_update", "skip", std::string(detail)});
}

bool ReportPassed(const std::vector<RendererCheck>& checks) {
  for (const auto& check : checks) {
    if (check.status == "fail") {
      return false;
    }
  }
  return true;
}

bool IsUnavailableCapability(std::string_view detail) {
  const auto incompatible_driver =
      std::string("create Vulkan instance failed with VkResult ") +
      std::to_string(VK_ERROR_INCOMPATIBLE_DRIVER);
  return detail == incompatible_driver ||
         detail.find("no Vulkan physical device is available") !=
             std::string_view::npos ||
         detail.find("physical device with a graphics queue is available") !=
             std::string_view::npos ||
         detail.find("D32 depth attachment readback is unsupported") !=
             std::string_view::npos ||
         detail.find("Vulkan 1.4 loader is required") != std::string_view::npos;
}

std::vector<RendererCheck> FailureChecks(ReportPhase phase,
                                         std::string_view detail,
                                         bool unavailable,
                                         bool install_tree) {
  std::vector<RendererCheck> checks;
  if (phase > ReportPhase::Core) {
    checks.push_back({"renderer.core.boundary", "pass", {}});
  } else {
    checks.push_back({"renderer.core.boundary", "fail", std::string(detail)});
  }

  if (phase > ReportPhase::Backend) {
    checks.push_back({"renderer.backend.capability", "pass", {}});
  } else if (phase == ReportPhase::Backend) {
    checks.push_back({"renderer.backend.capability",
                      unavailable ? "skip" : "fail", std::string(detail)});
  } else {
    checks.push_back({"renderer.backend.capability", "skip",
                      "core validation did not complete"});
  }

  if (phase >= ReportPhase::Frame) {
    checks.push_back({"renderer.gpu.frame", "fail", std::string(detail)});
  } else {
    checks.push_back({"renderer.gpu.frame", "skip",
                      unavailable ? std::string(detail)
                                  : "renderer backend was not available"});
  }
  checks.push_back({"renderer.validation.messages", "skip",
                    "no validated GPU frame completed"});
  checks.push_back({"renderer.render_product.color", "skip",
                    "no validated GPU frame completed"});
  checks.push_back({"renderer.render_product.depth", "skip",
                    "no validated GPU frame completed"});
  checks.push_back({"renderer.frame.persistence", "skip",
                    "no validated GPU frame completed"});
  checks.push_back(
      {"renderer.install_tree", install_tree ? "pass" : "skip",
       install_tree ? "" : "install-tree validation was not exercised"});
  AppendHydraSkips(checks);
  return checks;
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
  stream << "P6\n" << image.product.width << ' ' << image.product.height
         << "\n255\n";
  for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
    stream.write(reinterpret_cast<const char*>(image.pixels.data() + i), 3);
  }
  if (!stream) {
    throw std::runtime_error("could not write output image: " + path.string());
  }
}

void ValidateSmokeResult(const merlin::vulkan::RenderResult& result) {
  merlin::vulkan::ValidateRenderResult(result);
  const auto& image = result.color;
  const auto width = image.product.width;
  const auto height = image.product.height;
  const auto expected_size =
      static_cast<std::size_t>(image.row_pitch_bytes) * height;
  if (image.pixels.size() != expected_size) {
    throw std::runtime_error("Vulkan readback returned an invalid byte count");
  }
  const std::size_t center =
      (static_cast<std::size_t>(height / 2U) * width + width / 2U) * 4U;
  if (image.pixels[0] == image.pixels[center] &&
      image.pixels[1] == image.pixels[center + 1U] &&
      image.pixels[2] == image.pixels[center + 2U]) {
    throw std::runtime_error("offscreen smoke image does not contain the triangle");
  }
  const auto depth_center = result.depth.pixels[
      static_cast<std::size_t>(height / 2U) * width + width / 2U];
  const auto depth_corner = result.depth.pixels.front();
  if (!(depth_center < depth_corner)) {
    throw std::runtime_error("depth AOV does not contain triangle depth");
  }

  std::size_t top_coverage{};
  std::size_t bottom_coverage{};
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto depth = result.depth.pixels[
          static_cast<std::size_t>(y) * width + x];
      if (depth < 1.0F) {
        (y < height / 2U ? top_coverage : bottom_coverage)++;
      }
    }
  }
  if (top_coverage >= bottom_coverage) {
    throw std::runtime_error("CPU readback does not use top-left image origin");
  }
}

merlin::ChangeSet BuildSmokeWorld(merlin::RenderWorld& world) {
  merlin::MeshDescriptor mesh;
  mesh.label = "headless-triangle";
  mesh.positions = {{0.0F, -0.72F, 0.0F}, {0.72F, 0.62F, 0.0F},
                    {-0.72F, 0.62F, 0.0F}};
  mesh.normals = {{0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 1.0F},
                  {0.0F, 0.0F, 1.0F}};
  mesh.texcoords = {{0.5F, 0.0F}, {1.0F, 1.0F}, {0.0F, 1.0F}};
  mesh.indices = {0, 1, 2};
  const auto mesh_handle = world.CreateMesh(std::move(mesh));

  merlin::TextureDescriptor texture;
  texture.label = "headless-checker";
  texture.width = 2;
  texture.height = 2;
  texture.pixels = {255, 255, 255, 255, 40, 180, 255, 255,
                    40, 180, 255, 255, 255, 255, 255, 255};
  const auto texture_handle = world.CreateTexture(std::move(texture));
  merlin::SamplerDescriptor sampler;
  sampler.label = "headless-nearest";
  sampler.min_filter = merlin::FilterMode::Nearest;
  sampler.mag_filter = merlin::FilterMode::Nearest;
  const auto sampler_handle = world.CreateSampler(std::move(sampler));

  merlin::MaterialDescriptor material;
  material.label = "fallback";
  material.parameters.base_color = {0.18F, 0.78F, 1.0F, 1.0F};
  material.base_color_texture =
      merlin::TextureBinding{texture_handle, sampler_handle, 0};
  material.features |= merlin::MaterialFeature::BaseColorTexture;
  const auto material_handle = world.CreateMaterial(std::move(material));

  merlin::InstanceDescriptor instance;
  instance.label = "triangle-instance";
  instance.mesh = mesh_handle;
  instance.material = material_handle;
  world.CreateInstance(std::move(instance));

  merlin::LightDescriptor light;
  light.label = "headless-key";
  world.CreateLight(std::move(light));
  return world.Commit();
}

}  // namespace

int main(int argc, char** argv) {
  Arguments arguments;
  ReportPhase report_phase = ReportPhase::Arguments;
  std::optional<merlin::vulkan::RendererCapabilities> report_capabilities;
  try {
    arguments = ParseArguments(argc, argv);
    report_phase = ReportPhase::Core;
    merlin::RenderWorld world;
    const auto changes = BuildSmokeWorld(world);
    merlin::extraction::SceneExtractor extractor;
    extractor.Apply(world, changes);

    report_phase = ReportPhase::Backend;
    merlin::vulkan::Renderer renderer({arguments.validation});
    const auto& capabilities = renderer.capabilities();
    report_capabilities = capabilities;
    std::cout << "Merlin Vulkan device: " << capabilities.device_name << '\n'
              << "Loader API version: "
              << VersionString(capabilities.loader_api_version) << '\n'
              << "API version: " << VK_VERSION_MAJOR(capabilities.api_version) << '.'
              << VK_VERSION_MINOR(capabilities.api_version) << '.'
              << VK_VERSION_PATCH(capabilities.api_version) << '\n'
              << "Timeline semaphore: "
              << (capabilities.timeline_semaphore ? "yes" : "no") << '\n'
              << "Descriptor backend: "
              << merlin::vulkan::DescriptorBackendName(
                     capabilities.descriptor_indexing_selection.selected_backend)
              << " ("
              << merlin::vulkan::DescriptorFallbackReasonName(
                     capabilities.descriptor_indexing_selection.fallback_reason)
              << ")\n"
              << "Validation: " << (capabilities.validation_enabled ? "on" : "off") << '\n'
              << "RenderWorld revision: " << changes.revision << " ("
              << changes.changes.size() << " changes)\n";
    WriteMetadata(arguments.metadata, capabilities);

    if (arguments.validation && !capabilities.validation_enabled &&
        arguments.report.empty()) {
      std::cerr << "merlin-headless: Vulkan validation layer is unavailable\n";
      return 77;
    }

    report_phase = ReportPhase::Frame;
    merlin::vulkan::RenderResult result;
    std::optional<merlin::vulkan::RenderResult> expected_result;
    if (!arguments.probe_only) {
      const auto executable_dir = std::filesystem::absolute(argv[0]).parent_path();
      const merlin::vulkan::ShaderPaths shaders{
          executable_dir / "shaders" / "triangle.vert.spv",
          executable_dir / "shaders" / "triangle.frag.spv",
          executable_dir / "shaders" / "triangle.bindless.vert.spv",
          executable_dir / "shaders" / "triangle.bindless.frag.spv"};
      merlin::vulkan::RenderRequest request;
      request.snapshot = extractor.snapshot();
      request.width = arguments.width;
      request.height = arguments.height;
      request.shaders = shaders;
      request.products = {{merlin::Aov::Color, true},
                          {merlin::Aov::Depth, true}};
      if (!arguments.artifact_dir.empty()) {
        request.products.push_back({merlin::Aov::PrimId, true});
      }
      for (std::uint32_t frame = 0; frame < arguments.frames; ++frame) {
        const auto completion = renderer.Submit(request);
        result = renderer.Resolve(completion);
        if (!expected_result) {
          expected_result = result;
        }
      }
      ValidateSmokeResult(result);
      WritePpm(arguments.output, result.color);
      if (!arguments.artifact_dir.empty()) {
        const auto artifacts = merlin::vulkan::SaveComparisonArtifacts(
            *expected_result, result, arguments.artifact_dir);
        if (!artifacts.matches) {
          throw std::runtime_error(
              "unchanged-frame comparison artifacts do not match");
        }
      }
      const auto statistics = renderer.statistics();
      if (statistics.scene_uploads != 1 && arguments.report.empty()) {
        throw std::runtime_error("unchanged scene was uploaded more than once");
      }
      if (arguments.validation && statistics.validation_messages != 0 &&
          arguments.report.empty()) {
        throw std::runtime_error("Vulkan validation reported warnings or errors");
      }
      std::cout << "Wrote " << arguments.output.string() << " ("
                << result.color.product.width << 'x'
                << result.color.product.height << ", "
                << statistics.frames_submitted << " frames, completion "
                << result.completion_value << ")\n";
    }

    if (!arguments.report.empty()) {
      const auto statistics = renderer.statistics();
      const bool persistence_ok =
          !arguments.probe_only &&
          statistics.frames_submitted == arguments.frames &&
          result.completion_value == arguments.frames &&
          statistics.scene_uploads == 1 && statistics.frame_context_count == 3;
      std::vector<RendererCheck> checks{
          {"renderer.core.boundary", "pass", {}},
          {"renderer.backend.capability", "pass", {}},
          {"renderer.gpu.frame", arguments.probe_only ? "skip" : "pass",
           arguments.probe_only ? "probe-only mode did not render a frame" : ""},
          {"renderer.validation.messages",
           capabilities.validation_enabled
               ? (statistics.validation_messages == 0 ? "pass" : "fail")
               : "skip",
           capabilities.validation_enabled
               ? (statistics.validation_messages == 0
                      ? ""
                      : "Vulkan validation reported warnings or errors")
               : "VK_LAYER_KHRONOS_validation is unavailable"},
          {"renderer.render_product.color",
           arguments.probe_only ? "skip" : "pass",
           arguments.probe_only ? "probe-only mode did not render color" : ""},
          {"renderer.render_product.depth",
           arguments.probe_only ? "skip" : "pass",
           arguments.probe_only ? "probe-only mode did not render depth" : ""},
          {"renderer.frame.persistence", persistence_ok ? "pass" : "fail",
           persistence_ok
               ? ""
               : "frame count, completion, context, or unchanged-upload contract failed"},
          {"renderer.install_tree", arguments.install_tree ? "pass" : "skip",
           arguments.install_tree ? ""
                                  : "install-tree validation was not exercised"},
      };
      AppendHydraSkips(checks);
      report_phase = ReportPhase::Reporting;
      WriteRendererReport(arguments.report, report_capabilities, checks);
      return ReportPassed(checks) ? 0 : 1;
    }
    return 0;
  } catch (const std::exception& error) {
    if (!arguments.report.empty() && report_phase != ReportPhase::Reporting) {
      const bool unavailable =
          report_phase == ReportPhase::Backend &&
          IsUnavailableCapability(error.what());
      const auto checks = FailureChecks(report_phase, error.what(), unavailable,
                                        arguments.install_tree);
      try {
        WriteRendererReport(arguments.report, report_capabilities, checks);
      } catch (const std::exception& report_error) {
        std::cerr << "merlin-headless: " << report_error.what() << '\n';
        return 1;
      }
      std::cerr << "merlin-headless: " << error.what() << '\n';
      return unavailable ? 0 : 1;
    }
    std::cerr << "merlin-headless: " << error.what() << '\n';
    return 1;
  }
}
