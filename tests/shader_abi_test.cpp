#include <merlin/vulkan/shader_abi.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

std::string Read(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("cannot read shader contract input: " +
                             path.string());
  }
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

std::string CompactJson(std::string_view text) {
  std::string compact;
  compact.reserve(text.size());
  bool in_string{};
  bool escaped{};
  for (const char character : text) {
    if (in_string) {
      compact.push_back(character);
      if (escaped) {
        escaped = false;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '"') {
        in_string = false;
      }
    } else if (character == '"') {
      in_string = true;
      compact.push_back(character);
    } else if (!std::isspace(static_cast<unsigned char>(character))) {
      compact.push_back(character);
    }
  }
  return compact;
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

void RequireContains(std::string_view text, std::string_view expected,
                     std::string_view diagnostic) {
  Require(text.find(expected) != std::string_view::npos, diagnostic);
}

void RequireField(std::string_view json, std::string_view name,
                  std::size_t offset, std::size_t size) {
  const std::string field = "\"name\":\"" + std::string(name) + "\"";
  const std::string binding =
      "\"binding\":{\"kind\":\"uniform\",\"offset\":" +
      std::to_string(offset) + ",\"size\":" + std::to_string(size);
  auto position = json.find(field);
  while (position != std::string_view::npos) {
    const auto binding_position = json.find(binding, position);
    if (binding_position != std::string_view::npos &&
        binding_position - position < 700) {
      return;
    }
    position = json.find(field, position + field.size());
  }
  throw std::runtime_error("Slang reflection mismatch for field " +
                           std::string(name) + " at offset " +
                           std::to_string(offset));
}

void RequireCommonAbi(std::string_view json) {
  RequireContains(json, "\"name\":\"DrawConstants\"",
                  "DrawConstants is absent from Slang reflection");
  RequireContains(json,
                  "\"binding\":{\"kind\":\"pushConstantBuffer\",\"index\":0}",
                  "DrawConstants is not a push constant buffer");
  RequireField(json, "model_view_projection", 0, 64);
  RequireField(json, "normal_matrix_column0", 64, 16);
  RequireField(json, "normal_matrix_column1", 80, 16);
  RequireField(json, "normal_matrix_column2", 96, 16);
  RequireField(json, "feature_mask", 112, 4);
  RequireField(json, "prim_id", 116, 4);
  RequireField(json, "instance_id", 120, 4);
  RequireField(json, "texture_index", 124, 4);
  RequireField(json, "base_color", 0, 16);
  RequireField(json, "light_direction_intensity", 16, 16);
  RequireField(json, "light_color_alpha_cutoff", 32, 16);
  RequireField(json, "diffuse_environment", 48, 144);
}

void RequireBinding(std::string_view json, std::string_view name,
                    std::string_view binding) {
  const auto parameter =
      json.find("\"name\":\"" + std::string(name) + "\"");
  Require(parameter != std::string_view::npos,
          "resource is absent from Slang reflection");
  const auto reflected = json.find(binding, parameter);
  Require(reflected != std::string_view::npos && reflected - parameter < 180,
          "Slang resource set/binding mismatch");
}

// Every manifest reference is resolved relative to the package directory, so a
// value carrying a separator or a drive letter means a build path leaked.
void RequireBareFilenames(const std::string& json, std::string_view field) {
  const std::string key = "\"" + std::string(field) + "\":\"";
  std::size_t position = json.find(key);
  std::size_t seen{};
  while (position != std::string::npos) {
    const auto start = position + key.size();
    const auto end = json.find('"', start);
    Require(end != std::string::npos, "manifest JSON is truncated");
    const auto value = json.substr(start, end - start);
    Require(value.find('/') == std::string::npos &&
                value.find('\\') == std::string::npos &&
                value.find(':') == std::string::npos,
            "manifest field " + std::string(field) +
                " is not a bare filename: " + value);
    ++seen;
    position = json.find(key, end);
  }
  Require(seen > 0, "manifest has no \"" + std::string(field) + "\" field");
}

std::size_t CountRegex(const std::string& text, const std::regex& pattern) {
  return static_cast<std::size_t>(
      std::distance(std::sregex_iterator(text.begin(), text.end(), pattern),
                    std::sregex_iterator()));
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 17) {
      throw std::runtime_error(
          "usage: shader-abi-test conventional.vert.json conventional.frag.json "
          "bindless.vert.json bindless.frag.json gpu-scene.vert.json "
          "gpu-scene.frag.json gpu-driven.comp.json gpu-driven.vert.json "
          "gpu-driven.frag.json gaussian.vert.json "
          "gaussian-id.vert.json gaussian.frag.json gaussian-id.frag.json "
          "metal.vert.json "
          "metal.frag.json manifest.json");
    }

    const auto conventional_vertex = CompactJson(Read(argv[1]));
    const auto conventional_fragment = CompactJson(Read(argv[2]));
    const auto bindless_vertex = CompactJson(Read(argv[3]));
    const auto bindless_fragment = CompactJson(Read(argv[4]));
    const auto gpu_scene_vertex = CompactJson(Read(argv[5]));
    const auto gpu_scene_fragment = CompactJson(Read(argv[6]));
    const auto gpu_driven_compute = CompactJson(Read(argv[7]));
    const auto gpu_driven_vertex = CompactJson(Read(argv[8]));
    const auto gpu_driven_fragment = CompactJson(Read(argv[9]));
    const auto gaussian_vertex = CompactJson(Read(argv[10]));
    const auto gaussian_id_vertex = CompactJson(Read(argv[11]));
    const auto gaussian_fragment = CompactJson(Read(argv[12]));
    const auto gaussian_id_fragment = CompactJson(Read(argv[13]));
    const auto metal_vertex = CompactJson(Read(argv[14]));
    const auto metal_fragment = CompactJson(Read(argv[15]));
    const auto manifest = CompactJson(Read(argv[16]));

    RequireCommonAbi(conventional_vertex);
    RequireCommonAbi(conventional_fragment);
    RequireCommonAbi(bindless_vertex);
    RequireCommonAbi(bindless_fragment);

    RequireContains(gpu_scene_vertex, "\"name\":\"GpuSceneDrawConstants\"",
                    "GPU Scene draw constants are absent from reflection");
    RequireContains(
        gpu_scene_vertex,
        "\"binding\":{\"kind\":\"pushConstantBuffer\",\"index\":0}",
        "GPU Scene draw constants are not a push constant buffer");
    RequireField(gpu_scene_vertex, "view_projection", 0, 64);
    RequireField(gpu_scene_vertex, "draw_slot", 64, 4);

    RequireBinding(conventional_fragment, "base_color_texture",
                   "\"binding\":{\"kind\":\"descriptorTableSlot\",\"index\":0}");
    RequireContains(conventional_fragment, "\"combined\":true",
                    "base color texture is not a combined image sampler");
    RequireBinding(conventional_fragment, "material_constants",
                   "\"binding\":{\"kind\":\"descriptorTableSlot\",\"index\":31}");
    RequireBinding(bindless_fragment, "bindless_samplers",
                   "\"binding\":{\"kind\":\"descriptorTableSlot\",\"index\":0}");
    RequireBinding(bindless_fragment, "bindless_textures",
                   "\"binding\":{\"kind\":\"descriptorTableSlot\",\"index\":1}");
    RequireBinding(bindless_fragment, "material_constants",
                   "\"binding\":{\"kind\":\"descriptorTableSlot\",\"space\":1,\"index\":0}");
    RequireBinding(gpu_scene_vertex, "gpu_geometries",
                   "\"binding\":{\"kind\":\"descriptorTableSlot\",\"space\":1,\"index\":1}");
    RequireBinding(gpu_scene_vertex, "gpu_instances",
                   "\"binding\":{\"kind\":\"descriptorTableSlot\",\"space\":1,\"index\":2}");
    RequireBinding(gpu_scene_vertex, "gpu_materials",
                   "\"binding\":{\"kind\":\"descriptorTableSlot\",\"space\":1,\"index\":3}");
    RequireBinding(gpu_scene_vertex, "gpu_draws",
                   "\"binding\":{\"kind\":\"descriptorTableSlot\",\"space\":1,\"index\":4}");
    RequireBinding(gpu_scene_fragment, "bindless_textures",
                   "\"binding\":{\"kind\":\"descriptorTableSlot\",\"index\":1}");
    RequireContains(gpu_driven_compute,
                    "\"name\":\"GpuDrivenIndexedConstants\"",
                    "GPU-driven indexed constants are absent from reflection");
    RequireContains(
        gpu_driven_compute,
        "\"binding\":{\"kind\":\"pushConstantBuffer\",\"index\":0}",
        "GPU-driven indexed constants are not a push constant buffer");
    RequireField(gpu_driven_compute, "view_projection", 0, 64);
    RequireField(gpu_driven_compute, "visibility_mask", 64, 4);
    RequireField(gpu_driven_compute, "candidate_count", 68, 4);
    RequireField(gpu_driven_compute, "flags", 72, 4);
    RequireField(gpu_driven_compute, "vertex_stride", 76, 4);
    RequireBinding(gpu_driven_compute, "gpu_geometries",
                   "\"binding\":{\"kind\":\"descriptorTableSlot\",\"space\":1,\"index\":1}");
    RequireBinding(gpu_driven_compute, "gpu_instances",
                   "\"binding\":{\"kind\":\"descriptorTableSlot\",\"space\":1,\"index\":2}");
    RequireBinding(gpu_driven_compute, "gpu_draws",
                   "\"binding\":{\"kind\":\"descriptorTableSlot\",\"space\":1,\"index\":4}");
    RequireBinding(gpu_driven_compute, "candidate_draw_slots",
                   "\"binding\":{\"kind\":\"descriptorTableSlot\",\"space\":2,\"index\":0}");
    RequireBinding(gpu_driven_compute, "candidate_results",
                   "\"binding\":{\"kind\":\"descriptorTableSlot\",\"space\":2,\"index\":1}");
    RequireBinding(gpu_driven_compute, "indirect_commands",
                   "\"binding\":{\"kind\":\"descriptorTableSlot\",\"space\":2,\"index\":2}");
    RequireBinding(gpu_driven_compute, "dispatch_counters",
                   "\"binding\":{\"kind\":\"descriptorTableSlot\",\"space\":2,\"index\":3}");
    RequireField(gpu_driven_compute, "index_count", 0, 4);
    RequireField(gpu_driven_compute, "instance_count", 4, 4);
    RequireField(gpu_driven_compute, "first_index", 8, 4);
    RequireField(gpu_driven_compute, "vertex_offset", 12, 4);
    RequireField(gpu_driven_compute, "first_instance", 16, 4);
    RequireField(gpu_driven_compute, "visibility_mask_culled_count", 8, 4);
    RequireField(gpu_driven_compute, "frustum_culled_count", 12, 4);
    RequireContains(gpu_driven_vertex,
                    "\"name\":\"GpuDrivenForwardConstants\"",
                    "GPU-driven Forward constants are absent");
    RequireField(gpu_driven_vertex, "view_projection", 0, 64);
    RequireContains(gpu_driven_vertex,
                    "\"semanticName\":\"SV_INSTANCEID\"",
                    "GPU-driven Forward does not consume the local instance ID");
    RequireContains(gpu_driven_vertex,
                    "\"semanticName\":\"SV_STARTINSTANCELOCATION\"",
                    "GPU-driven Forward does not consume firstInstance");
    RequireBinding(gpu_driven_vertex, "gpu_geometries",
                   "\"binding\":{\"kind\":\"descriptorTableSlot\",\"space\":1,\"index\":1}");
    RequireBinding(gpu_driven_vertex, "gpu_instances",
                   "\"binding\":{\"kind\":\"descriptorTableSlot\",\"space\":1,\"index\":2}");
    RequireBinding(gpu_driven_fragment, "gpu_materials",
                   "\"binding\":{\"kind\":\"descriptorTableSlot\",\"space\":1,\"index\":3}");
    RequireBinding(gpu_driven_fragment, "gpu_draws",
                   "\"binding\":{\"kind\":\"descriptorTableSlot\",\"space\":1,\"index\":4}");
    RequireContains(bindless_fragment, "\"elementCount\":0",
                    "bindless descriptors are not reflected as runtime arrays");

    RequireContains(conventional_vertex,
                    "\"name\":\"forward_vertex\",\"stage\":\"vertex\"",
                    "conventional vertex entry point mismatch");
    RequireContains(conventional_fragment,
                    "\"name\":\"forward_fragment\",\"stage\":\"fragment\"",
                    "conventional fragment entry point mismatch");
    RequireContains(bindless_vertex,
                    "\"name\":\"forward_bindless_vertex\",\"stage\":\"vertex\"",
                    "bindless vertex entry point mismatch");
    RequireContains(bindless_fragment,
                    "\"name\":\"forward_bindless_fragment\",\"stage\":\"fragment\"",
                    "bindless fragment entry point mismatch");
    RequireContains(gpu_scene_vertex,
                    "\"name\":\"forward_gpu_scene_vertex\",\"stage\":\"vertex\"",
                    "GPU Scene vertex entry point mismatch");
    RequireContains(gpu_scene_fragment,
                    "\"name\":\"forward_gpu_scene_fragment\",\"stage\":\"fragment\"",
                    "GPU Scene fragment entry point mismatch");
    RequireContains(gpu_driven_compute,
                    "\"name\":\"gpu_driven_indexed_compact\",\"stage\":\"compute\"",
                    "GPU-driven indexed compute entry point mismatch");
    RequireContains(gaussian_vertex,
                    "\"name\":\"gaussian_vertex\",\"stage\":\"vertex\"",
                    "Gaussian vertex entry point mismatch");
    RequireContains(gaussian_fragment,
                    "\"name\":\"gaussian_fragment\",\"stage\":\"fragment\"",
                    "Gaussian fragment entry point mismatch");
    RequireContains(
        gaussian_id_vertex,
        "\"name\":\"gaussian_id_vertex\",\"stage\":\"vertex\"",
        "Gaussian ID vertex entry point mismatch");
    RequireContains(
        gaussian_id_fragment,
        "\"name\":\"gaussian_id_fragment\",\"stage\":\"fragment\"",
        "Gaussian ID fragment entry point mismatch");
    RequireContains(gaussian_vertex, "\"name\":\"GaussianConstants\"",
                    "Gaussian push constants are absent from reflection");
    RequireContains(
        gaussian_vertex,
        "\"binding\":{\"kind\":\"pushConstantBuffer\",\"index\":0}",
        "Gaussian constants are not a push constant buffer");
    RequireField(gaussian_vertex, "inverse_viewport_size", 0, 8);
    RequireContains(metal_vertex,
                    "\"name\":\"forward_vertex\",\"stage\":\"vertex\"",
                    "Metal vertex compile gate reflection mismatch");
    RequireContains(metal_fragment,
                    "\"name\":\"forward_fragment\",\"stage\":\"fragment\"",
                    "Metal fragment compile gate reflection mismatch");

    RequireContains(manifest, "\"schema_version\":2",
                    "shader artifact manifest schema mismatch");
    RequireContains(manifest, "\"shader_abi_version\":5",
                    "shader ABI manifest version mismatch");
    RequireContains(manifest, "\"required_series\":\"2026.8\"",
                    "Slang toolchain series is not pinned");
    RequireContains(
        manifest,
        "\"environment\":{\"path\":\"environment.hdr\",\"sha256\":\"4897697c757edc524dc9b7bcc692e8e05a7f02dbede3e30d2291dc0831dece17\",\"representation\":\"diffuse-sh-l2\"}",
        "environment artifact identity is absent");
    RequireContains(manifest,
                    "\"feature\":\"non_uniform_resource_indexing\"",
                    "Metal unsupported feature diagnostic is absent");
    RequireContains(manifest, "\"fallback\":\"forward-conventional\"",
                    "Metal fallback declaration is absent");
    // merlin-shader-artifact-key recomputes these; here they only have to be
    // present, canonical, and one per artifact.
    Require(CountRegex(manifest, std::regex(
                "\\\"artifact_key\\\":\\\"sha256:[0-9a-f]{64}\\\"")) == 15,
            "manifest does not contain one deterministic key per artifact");
    RequireBareFilenames(manifest, "path");
    RequireBareFilenames(manifest, "reflection");
    RequireBareFilenames(manifest, "source");

    using namespace merlin::vulkan::shader_abi;
    static_assert(kVersion == 5);
    static_assert(kArtifactSchemaVersion == 2);
    static_assert(kConventionalBaseColorTexture.set == 0);
    static_assert(kConventionalBaseColorTexture.binding == 0);
    static_assert(kConventionalMaterialConstants.binding == 31);
    static_assert(kBindlessSamplers.binding == 0);
    static_assert(kBindlessTextures.binding == 1);
    static_assert(kBindlessMaterialConstants.set == 1);
    static_assert(kBindlessMaterialConstants.binding == 0);
    static_assert(kGpuSceneGeometries.binding == 1);
    static_assert(kGpuSceneInstances.binding == 2);
    static_assert(kGpuSceneMaterials.binding == 3);
    static_assert(kGpuSceneDraws.binding == 4);
    static_assert(kGpuDrivenCandidateDrawSlots.set == 2);
    static_assert(kGpuDrivenCandidateDrawSlots.binding == 0);
    static_assert(kGpuDrivenCandidateResults.binding == 1);
    static_assert(kGpuDrivenIndirectCommands.binding == 2);
    static_assert(kGpuDrivenDispatchCounters.binding == 3);
  } catch (const std::exception& error) {
    std::cerr << "shader ABI contract failure: " << error.what() << '\n';
    return 1;
  }
}
