// The shader manifest is written by CMake and the artifact-key formula lives in
// merlin/core/shader_artifact.hpp. This test recomputes every identity in the
// emitted manifest through that header, so a change to either side that the
// other does not follow fails the build rather than silently producing keys
// that disagree about what a cached artifact is.

#include <merlin/core/identity.hpp>
#include <merlin/core/shader_artifact.hpp>

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void Require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

std::string Read(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  Require(static_cast<bool>(stream),
          "cannot read shader manifest: " + path.string());
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

// The manifest is generated, flat, and fully quoted, so locating a field by
// name inside a known region is enough; nothing here needs a JSON parser.
std::string Field(std::string_view json, std::string_view name,
                  std::size_t from = 0) {
  const std::string key = "\"" + std::string(name) + "\":\"";
  const auto start = json.find(key, from);
  Require(start != std::string_view::npos,
          "manifest has no \"" + std::string(name) + "\" field");
  const auto value = start + key.size();
  const auto end = json.find('"', value);
  Require(end != std::string_view::npos, "manifest JSON is truncated");
  return std::string(json.substr(value, end - value));
}

// "features": ["a", "b"] as the "+"-joined spelling the compile recorded.
std::string JoinedArray(std::string_view json, std::string_view name,
                        std::size_t from) {
  const std::string key = "\"" + std::string(name) + "\":[";
  const auto start = json.find(key, from);
  Require(start != std::string_view::npos,
          "manifest artifact has no \"" + std::string(name) + "\" array");
  const auto end = json.find(']', start);
  Require(end != std::string_view::npos, "manifest JSON is truncated");
  const auto body = json.substr(start + key.size(), end - start - key.size());

  std::string joined;
  std::size_t position{};
  while ((position = body.find('"', position)) != std::string_view::npos) {
    const auto value = position + 1U;
    const auto close = body.find('"', value);
    Require(close != std::string_view::npos, "manifest JSON is truncated");
    if (!joined.empty()) {
      joined.push_back('+');
    }
    joined.append(body.substr(value, close - value));
    position = close + 1U;
  }
  return joined;
}

std::vector<merlin::ShaderSourceFingerprint> ModuleSources(
    std::string_view json, std::size_t from) {
  const auto start = json.find("\"module_sources\":[", from);
  Require(start != std::string_view::npos,
          "manifest artifact has no module_sources array");
  const auto end = json.find(']', start);
  Require(end != std::string_view::npos, "manifest JSON is truncated");
  const auto body = json.substr(start, end - start);

  std::vector<merlin::ShaderSourceFingerprint> sources;
  std::size_t position{};
  while ((position = body.find("{\"path\":\"", position)) !=
         std::string_view::npos) {
    sources.push_back({Field(body, "path", position),
                       Field(body, "sha256", position)});
    position = body.find('}', position);
    Require(position != std::string_view::npos, "manifest JSON is truncated");
  }
  Require(!sources.empty(), "manifest artifact lists no module sources");
  return sources;
}

void RequireContractSemantics() {
  merlin::ShaderArtifactKeyInputs inputs;
  inputs.module_identity = merlin::MakeShaderModuleIdentity(
      {{"forward.slang", std::string(64U, 'a')}});
  inputs.entry_point = "forward_fragment";
  inputs.stage = "fragment";
  inputs.permutation = "forward-conventional";
  inputs.features = "material_constants";
  inputs.abi_version = 2;
  inputs.policy.compiler_version = "2026.8.1";
  inputs.policy.target = "spirv";
  inputs.policy.profile = "sm_6_6";
  inputs.policy.capabilities = "spirv_1_5";
  inputs.policy.matrix_layout = "column-major";
  inputs.policy.optimization = "O2";

  const auto key = merlin::MakeShaderArtifactKey(inputs);
  Require(merlin::IsIdentity(key), "artifact key is not a canonical identity");
  Require(merlin::IsIdentity(inputs.module_identity),
          "module identity is not a canonical identity");
  Require(key == merlin::MakeShaderArtifactKey(inputs),
          "artifact key is not deterministic");

  // One module, two targets: the module identity is reused unchanged and only
  // the artifact key moves. This is the property that lets the same generated
  // material serve SPIR-V and Metal without being regenerated.
  auto metal = inputs;
  metal.policy.target = "metal";
  metal.policy.profile = "metallib_2_4";
  metal.policy.capabilities = "none";
  Require(merlin::MakeShaderArtifactKey(metal) != key,
          "target policy does not reach the artifact key");
  Require(metal.module_identity == inputs.module_identity,
          "changing the target changed the module identity");

  // Every remaining policy input must move the key on its own, or a cache hit
  // could hand back an artifact built under different rules.
  const auto require_distinct = [&key](merlin::ShaderArtifactKeyInputs changed,
                                       std::string_view what) {
    Require(merlin::MakeShaderArtifactKey(changed) != key,
            std::string(what) + " does not reach the artifact key");
  };
  auto changed = inputs;
  changed.entry_point = "forward_vertex";
  require_distinct(changed, "entry point");
  changed = inputs;
  changed.stage = "vertex";
  require_distinct(changed, "stage");
  changed = inputs;
  changed.permutation = "forward-bindless";
  require_distinct(changed, "permutation");
  changed = inputs;
  changed.features = "material_constants+base_color_texture";
  require_distinct(changed, "feature set");
  changed = inputs;
  changed.abi_version = 3;
  require_distinct(changed, "ABI version");
  changed = inputs;
  changed.policy.compiler_version = "2026.8.2";
  require_distinct(changed, "compiler version");
  changed = inputs;
  changed.policy.matrix_layout = "row-major";
  require_distinct(changed, "matrix layout");
  changed = inputs;
  changed.policy.optimization = "O0";
  require_distinct(changed, "optimization policy");
  changed = inputs;
  changed.policy.debug_info = true;
  require_distinct(changed, "debug policy");
  changed = inputs;
  changed.policy.target_options = {"-fvk-use-entrypoint-name"};
  require_distinct(changed, "target-specific option");
  changed = inputs;
  changed.module_identity = merlin::MakeShaderModuleIdentity(
      {{"forward.slang", std::string(64U, 'b')}});
  require_distinct(changed, "module identity");

  // Field boundaries are explicit, so text cannot migrate between fields and
  // leave the record unchanged.
  auto shifted = inputs;
  shifted.policy.target = "spir";
  shifted.policy.profile = "vsm_6_6";
  require_distinct(shifted, "field boundary");

  // Include order is a traversal detail, not an identity.
  const merlin::ShaderSourceFingerprint common{"forward-common.slang",
                                               std::string(64U, 'c')};
  const merlin::ShaderSourceFingerprint forward{"forward.slang",
                                                std::string(64U, 'd')};
  Require(merlin::MakeShaderModuleIdentity({common, forward}) ==
              merlin::MakeShaderModuleIdentity({forward, common}),
          "module identity depends on include traversal order");
  Require(merlin::MakeShaderModuleIdentity({common, forward, common}) ==
              merlin::MakeShaderModuleIdentity({forward, common}),
          "an include reached twice changes the module identity");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Require(argc == 2, "usage: shader-artifact-key-test manifest.json");
    const auto manifest = CompactJson(Read(argv[1]));

    RequireContractSemantics();

    Require(manifest.find("\"module_identity_schema\":\"" +
                          std::string(merlin::kShaderModuleIdentitySchema) +
                          "\"") != std::string::npos,
            "manifest does not declare the module identity schema");
    Require(manifest.find("\"artifact_key_schema\":\"" +
                          std::string(merlin::kShaderArtifactKeySchema) +
                          "\"") != std::string::npos,
            "manifest does not declare the artifact key schema");

    merlin::ShaderTargetPolicy policy;
    policy.compiler = Field(manifest, "compiler");
    policy.compiler_version = Field(manifest, "compiler_version");
    policy.matrix_layout = Field(manifest, "matrix_layout");
    policy.optimization = Field(manifest, "optimization");
    Require(manifest.find("\"debug_info\":false") != std::string::npos,
            "manifest does not declare the debug policy the key assumes");

    const std::string abi_field = "\"shader_abi_version\":";
    const auto abi_position = manifest.find(abi_field);
    Require(abi_position != std::string::npos,
            "manifest has no shader ABI version");
    const auto abi = static_cast<std::uint32_t>(
        std::stoul(manifest.substr(abi_position + abi_field.size())));

    std::set<std::string> artifact_keys;
    std::set<std::string> module_identities;
    std::size_t position = manifest.find("\"artifacts\":[");
    Require(position != std::string::npos, "manifest has no artifacts array");
    std::size_t verified{};
    while ((position = manifest.find("{\"path\":\"", position)) !=
           std::string::npos) {
      merlin::ShaderArtifactKeyInputs inputs;
      inputs.module_identity =
          merlin::MakeShaderModuleIdentity(ModuleSources(manifest, position));
      inputs.entry_point = Field(manifest, "entry_point", position);
      inputs.stage = Field(manifest, "stage", position);
      inputs.permutation = Field(manifest, "permutation", position);
      inputs.features = JoinedArray(manifest, "features", position);
      inputs.abi_version = abi;
      inputs.policy = policy;
      inputs.policy.target = Field(manifest, "target", position);
      inputs.policy.profile = Field(manifest, "profile", position);
      inputs.policy.capabilities = Field(manifest, "capabilities", position);

      const auto recorded_module = Field(manifest, "module_identity", position);
      Require(recorded_module == inputs.module_identity,
              "recorded module identity disagrees with the sources it lists: " +
                  recorded_module);
      const auto recorded_key = Field(manifest, "artifact_key", position);
      Require(recorded_key == merlin::MakeShaderArtifactKey(inputs),
              "recorded artifact key disagrees with merlin/core/"
              "shader_artifact.hpp for " +
                  Field(manifest, "path", position));

      artifact_keys.insert(recorded_key);
      module_identities.insert(recorded_module);
      ++verified;
      position = manifest.find("\"artifact_key\":\"", position);
      Require(position != std::string::npos, "manifest JSON is truncated");
      ++position;
    }

    Require(verified == 6, "manifest does not describe six artifacts");
    Require(artifact_keys.size() == verified,
            "two artifacts share an artifact key");
    // The conventional SPIR-V and Metal artifacts compile the same module, so
    // fewer identities than artifacts is the expected, load-bearing result.
    Require(module_identities.size() == 2U,
            "module identities do not follow the two shader modules");
  } catch (const std::exception& error) {
    std::cerr << "shader artifact key test failed: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
