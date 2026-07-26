#include <merlin/core/shader_artifact.hpp>

#include <merlin/core/identity.hpp>

#include <algorithm>
#include <tuple>

namespace merlin {

std::string MakeShaderModuleIdentity(
    std::vector<ShaderSourceFingerprint> sources) {
  const auto order = [](const ShaderSourceFingerprint& lhs,
                        const ShaderSourceFingerprint& rhs) {
    return std::tie(lhs.path, lhs.content_sha256) <
           std::tie(rhs.path, rhs.content_sha256);
  };
  std::sort(sources.begin(), sources.end(), order);
  // A path repeated with the same content is the same include reached twice.
  // A path repeated with different content is a real conflict the caller was
  // supposed to reject: both entries stay in the record, so the identity
  // differs from either resolution and therefore matches no artifact built from
  // one of them. Resolving it here would instead hand out a key claiming a
  // content the module never compiled.
  sources.erase(std::unique(sources.begin(), sources.end(),
                            [](const ShaderSourceFingerprint& lhs,
                               const ShaderSourceFingerprint& rhs) {
                              return lhs.path == rhs.path &&
                                     lhs.content_sha256 == rhs.content_sha256;
                            }),
                sources.end());

  std::string record;
  AppendIdentityField(record, "schema", kShaderModuleIdentitySchema);
  for (const auto& source : sources) {
    AppendIdentityField(record, "path", source.path);
    AppendIdentityField(record, "content-sha256", source.content_sha256);
  }
  return MakeIdentity(record);
}

std::string MakeShaderArtifactKeyRecord(
    const ShaderArtifactKeyInputs& inputs) {
  std::string record;
  AppendIdentityField(record, "schema", kShaderArtifactKeySchema);
  AppendIdentityField(record, "module", inputs.module_identity);
  AppendIdentityField(record, "abi", std::to_string(inputs.abi_version));
  AppendIdentityField(record, "entry", inputs.entry_point);
  AppendIdentityField(record, "stage", inputs.stage);
  AppendIdentityField(record, "permutation", inputs.permutation);
  AppendIdentityField(record, "features", inputs.features);
  AppendIdentityField(record, "compiler", inputs.policy.compiler);
  AppendIdentityField(record, "compiler-version",
                      inputs.policy.compiler_version);
  AppendIdentityField(record, "target", inputs.policy.target);
  AppendIdentityField(record, "profile", inputs.policy.profile);
  AppendIdentityField(record, "capabilities", inputs.policy.capabilities);
  AppendIdentityField(record, "matrix-layout", inputs.policy.matrix_layout);
  AppendIdentityField(record, "optimization", inputs.policy.optimization);
  AppendIdentityField(record, "debug-info",
                      inputs.policy.debug_info ? "true" : "false");
  for (const auto& option : inputs.policy.target_options) {
    AppendIdentityField(record, "target-option", option);
  }
  return record;
}

std::string MakeShaderArtifactKey(const ShaderArtifactKeyInputs& inputs) {
  return MakeIdentity(MakeShaderArtifactKeyRecord(inputs));
}

}  // namespace merlin
