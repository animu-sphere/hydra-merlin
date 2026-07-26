#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace merlin {

// A module identity is target-neutral: it answers "which shader source is
// this?". A target-artifact identity answers "which compiled output of that
// source is this?", and therefore adds every compiler, target, and policy
// input that can change the produced binary without changing the source.
//
// The split is what lets one module contribute a SPIR-V artifact and a Metal
// artifact without being regenerated, and it is why a parameter-only material
// edit reaches neither key.
//
// This contract is deliberately not MaterialX-shaped. A MaterialX-generated
// module supplies its topology-only module key; a handwritten Slang module
// supplies the fingerprint of its source and includes. Nothing else differs.

inline constexpr std::string_view kShaderModuleIdentitySchema =
    "merlin.shader-module-identity/v1";
inline constexpr std::string_view kShaderArtifactKeySchema =
    "merlin.shader-artifact-key/v1";

struct ShaderSourceFingerprint {
  // Logical path inside the shader package. A host-absolute path would make
  // the identity unreproducible on another machine.
  std::string path;
  std::string content_sha256;
};

// Identity of a handwritten Slang module: its own source plus every include it
// compiles with, ordered by logical path so the caller's traversal order does
// not leak into the key. A generated material supplies its own module key
// instead of calling this.
[[nodiscard]] std::string MakeShaderModuleIdentity(
    std::vector<ShaderSourceFingerprint> sources);

// Compile policy recorded verbatim from the invocation that produced the
// artifact. `target`, `profile`, and `capabilities` are the compiler's own
// argument spellings, not backend API objects; "none" means the target takes
// no capability argument.
struct ShaderTargetPolicy {
  std::string compiler{"slangc"};
  std::string compiler_version;
  std::string target;
  std::string profile;
  std::string capabilities{"none"};
  std::string matrix_layout;
  std::string optimization;
  bool debug_info{false};
  // Further target-specific generation options. Order is part of the identity
  // because it is part of the command line.
  std::vector<std::string> target_options;
};

struct ShaderArtifactKeyInputs {
  // Target-neutral identity of the compiled source: a generated material's
  // module key, or a handwritten module's source/include fingerprint.
  std::string module_identity;
  std::string entry_point;
  std::string stage;
  // Logical permutation name and the feature set it enables. The name alone is
  // a label, so the features travel with it.
  std::string permutation;
  std::string features;
  std::uint32_t abi_version{};
  ShaderTargetPolicy policy;
};

// The canonical record the key hashes. Exposed so build tooling and evidence
// consumers can show which inputs produced a key instead of only its digest.
[[nodiscard]] std::string MakeShaderArtifactKeyRecord(
    const ShaderArtifactKeyInputs& inputs);

// `sha256:<hex>` over that record.
[[nodiscard]] std::string MakeShaderArtifactKey(
    const ShaderArtifactKeyInputs& inputs);

}  // namespace merlin
