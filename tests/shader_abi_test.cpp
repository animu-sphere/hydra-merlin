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
    if (argc != 8) {
      throw std::runtime_error(
          "usage: shader-abi-test conventional.vert.json conventional.frag.json "
          "bindless.vert.json bindless.frag.json metal.vert.json "
          "metal.frag.json manifest.json");
    }

    const auto conventional_vertex = CompactJson(Read(argv[1]));
    const auto conventional_fragment = CompactJson(Read(argv[2]));
    const auto bindless_vertex = CompactJson(Read(argv[3]));
    const auto bindless_fragment = CompactJson(Read(argv[4]));
    const auto metal_vertex = CompactJson(Read(argv[5]));
    const auto metal_fragment = CompactJson(Read(argv[6]));
    const auto manifest = CompactJson(Read(argv[7]));

    RequireCommonAbi(conventional_vertex);
    RequireCommonAbi(conventional_fragment);
    RequireCommonAbi(bindless_vertex);
    RequireCommonAbi(bindless_fragment);

    RequireBinding(conventional_fragment, "base_color_texture",
                   "\"binding\":{\"kind\":\"descriptorTableSlot\",\"index\":0}");
    RequireContains(conventional_fragment, "\"combined\":true",
                    "base color texture is not a combined image sampler");
    RequireBinding(conventional_fragment, "material_constants",
                   "\"binding\":{\"kind\":\"descriptorTableSlot\",\"index\":1}");
    RequireBinding(bindless_fragment, "bindless_samplers",
                   "\"binding\":{\"kind\":\"descriptorTableSlot\",\"index\":0}");
    RequireBinding(bindless_fragment, "bindless_textures",
                   "\"binding\":{\"kind\":\"descriptorTableSlot\",\"index\":1}");
    RequireBinding(bindless_fragment, "material_constants",
                   "\"binding\":{\"kind\":\"descriptorTableSlot\",\"space\":1,\"index\":0}");
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
    RequireContains(metal_vertex,
                    "\"name\":\"forward_vertex\",\"stage\":\"vertex\"",
                    "Metal vertex compile gate reflection mismatch");
    RequireContains(metal_fragment,
                    "\"name\":\"forward_fragment\",\"stage\":\"fragment\"",
                    "Metal fragment compile gate reflection mismatch");

    RequireContains(manifest, "\"schema_version\":1",
                    "shader artifact manifest schema mismatch");
    RequireContains(manifest, "\"shader_abi_version\":1",
                    "shader ABI manifest version mismatch");
    RequireContains(manifest, "\"required_series\":\"2026.8\"",
                    "Slang toolchain series is not pinned");
    RequireContains(manifest,
                    "\"feature\":\"non_uniform_resource_indexing\"",
                    "Metal unsupported feature diagnostic is absent");
    RequireContains(manifest, "\"fallback\":\"forward-conventional\"",
                    "Metal fallback declaration is absent");
    Require(CountRegex(manifest,
                       std::regex("\\\"cache_key\\\":\\\"[0-9a-f]{64}\\\"")) == 6,
            "manifest does not contain one deterministic key per artifact");
    RequireBareFilenames(manifest, "path");
    RequireBareFilenames(manifest, "reflection");
    RequireBareFilenames(manifest, "source");

    using namespace merlin::vulkan::shader_abi;
    static_assert(kVersion == 1);
    static_assert(kArtifactSchemaVersion == 1);
    static_assert(kConventionalBaseColorTexture.set == 0);
    static_assert(kConventionalBaseColorTexture.binding == 0);
    static_assert(kConventionalMaterialConstants.binding == 1);
    static_assert(kBindlessSamplers.binding == 0);
    static_assert(kBindlessTextures.binding == 1);
    static_assert(kBindlessMaterialConstants.set == 1);
    static_assert(kBindlessMaterialConstants.binding == 0);
  } catch (const std::exception& error) {
    std::cerr << "shader ABI contract failure: " << error.what() << '\n';
    return 1;
  }
}
