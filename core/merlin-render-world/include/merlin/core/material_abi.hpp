#pragma once

#include <merlin/core/material_diagnostic.hpp>
#include <merlin/core/types.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace merlin {

// `types.hpp` declares what a generated material module *is*: its entry point,
// its ABI and reflection schema versions, its logical parameter and resource
// layout, and the geometry inputs it needs and result fields it produces.
//
// This contract answers the two questions a consumer of one has to ask before
// it can draw with it:
//
//   * can this consumer use the module? Its declared ABI against what the
//     consumer is able to supply and willing to read.
//   * did the compiler produce what the module declared? A target artifact's
//     reflected interface against that same logical layout.
//
// Both answers are `MaterialDiagnostic` records, so an ABI failure classifies,
// bridges onto `merlin-diagnostic/v1`, and descends the fallback ladder exactly
// like a generation failure does. Neither carries a MaterialX, Slang, Vulkan, or
// Metal type: a target names itself with its compiler's own spelling and is
// never interpreted, and each backend derives its native descriptor or argument
// layout from the logical one.
//
// The contract belongs to Core for the same reason the diagnostic categories do.
// A MaterialX-generated module and a handwritten one are checked by one rule, so
// a renderer cannot end up trusting a module that only one producer's private
// check ever looked at.

inline constexpr std::string_view kMaterialAbiSchema = "merlin.material-abi/v1";

// Stable spellings for the enumerators that appear in ABI messages. They are
// contract surface: a host reads them out of a diagnostic, so they are spelled
// once here rather than formatted at each call site.
//
// The result and input helpers describe a single flag. Passing a combination
// returns "unknown", because no one name is honest about two of them.
[[nodiscard]] std::string_view MaterialValueTypeName(
    MaterialValueType type) noexcept;
[[nodiscard]] std::string_view MaterialResultFieldName(
    MaterialResultField field) noexcept;
[[nodiscard]] std::string_view MaterialInputRequirementName(
    MaterialInputRequirement input) noexcept;

// What a consumer of a generated material declares about itself.
//
// This is the renderer's half of the ABI. A module is not "valid" or "invalid"
// in isolation: a module producing only a base color is perfectly good for a
// consumer that reads only a base color, and unusable for one that shades with
// roughness. Stating the consumer's side explicitly is what lets the same module
// pass for a Forward pass and fail for a later one without changing the module.
struct MaterialAbiExpectation {
  std::uint32_t abi_version{kMaterialAbiVersion};
  std::uint32_t reflection_schema_version{kMaterialReflectionSchemaVersion};
  std::string entry_point{"evaluateMaterial"};
  // Result fields the consumer reads. A module that produces fewer cannot drive
  // it; one that produces more is fine, because the extra fields are ignored.
  MaterialResultField required_results{MaterialResultField::BaseColor};
  // Geometry inputs the consumer can construct. A module needing one the
  // consumer cannot build has no way to be evaluated at all.
  //
  // The default is every input the contract defines, because hdMerlin owns
  // geometry construction and supplies all of it; a consumer that cannot should
  // narrow this rather than discover the gap at draw time. It is spelled as the
  // shared mask so an input added to the enum widens this default with it,
  // rather than leaving a consumer that supplies everything looking as though
  // it does not.
  MaterialInputRequirement available_inputs{kAllMaterialInputRequirements};
};

// A compiled artifact's interface, as its target compiler reported it, restated
// in the logical vocabulary.
//
// Whichever layer invoked the compiler translates that compiler's reflection
// format into this; Core never reads one. That is the whole point of the split:
// SPIR-V reports a descriptor slot and Metal reports a buffer index for the same
// parameter, and neither spelling is part of the agreement being checked.
struct MaterialTargetReflection {
  // The compiler's own spelling of the target, e.g. "spirv" or "metal". It is
  // echoed into diagnostics so a host can tell which artifact disagreed, and is
  // never interpreted.
  std::string target;
  // Entry points the artifact reports.
  //
  // A generated material declares none of its own: it is composed into a
  // renderer-owned entry point and inlined, so these are the renderer's. The
  // module's own entry-point name appearing here means the material became the
  // pass instead of being called by it, which is the source-level contamination
  // rule observed on the compiled artifact rather than on the text.
  std::vector<std::string> entry_points;
  // The material's own parameter and resource block, as the target laid it out.
  //
  // A composed artifact also reports the renderer's uniforms and resources.
  // Those are outside the material block and are not this contract's business;
  // the producer that reads the compiler's reflection separates them. One of
  // them appearing *inside* the material block is exactly the failure this
  // check exists to find, which is why an undeclared entry is a mismatch rather
  // than an allowance.
  MaterialParameterLayout parameters;
  MaterialResourceLayout resources;
};

// Can this consumer use this module?
//
// Every returned record is an `AbiMismatch`. Most describe a disagreement with
// the consumer; a module that declares one parameter or resource name twice is
// reported here as well, because a name nothing downstream can resolve to a
// single declaration is unusable for every consumer rather than for this one.
// An empty result means the module satisfies the expectation.
[[nodiscard]] std::vector<MaterialDiagnostic> VerifyMaterialAbi(
    const MaterialModule& module,
    const MaterialAbiExpectation& expectation = {},
    const MaterialFallbackPolicy& policy = {});

// Did the compiler produce the interface the module declared?
//
// Agreement is semantic, not positional: entries are matched by name and
// compared on type and array size, because a target is free to lay the block out
// however its ABI requires. A parameter the module declared and the target did
// not report, one the target reported and the module never declared, a type that
// changed across the boundary, or a name reported twice are all
// `ReflectionMismatch`. An artifact that reports no entry point at all, or that
// reports the module's own, is an `AbiMismatch`: that is about which code owns
// the pass rather than about the interface.
//
// A name the *module* declared twice is also an `AbiMismatch`, and the entries
// under it are then not compared against the target at all: no reported entry
// can be attributed to one of two declarations, so comparing would report the
// target as disagreeing with whichever declaration happened to be listed last.
//
// Two targets that both agree with one module therefore agree with each other;
// there is no separate cross-target check to run.
[[nodiscard]] std::vector<MaterialDiagnostic> VerifyMaterialTargetReflection(
    const MaterialModule& module, const MaterialTargetReflection& reflection,
    const MaterialFallbackPolicy& policy = {});

// Does this generated source declare only a material function?
//
// MaterialXGenSlang owns graph evaluation; hdMerlin owns the render pass. A
// generated module that declares an entry point, binds a system value, names a
// binding slot or a pass mode, allocates group-shared storage, or discards a
// fragment has taken over part of the pass, and composing it into a
// renderer-owned shader would silently hand pass ownership across the boundary.
// Every such record is a `GenerationFailure`: the generator produced something
// outside the boundary it was given.
//
// The scan names Slang/HLSL declaration forms because Slang is this
// repository's one shading language across backends; nothing about it is
// MaterialX-shaped, and a handwritten generated module is checked identically.
// Whole families are rejected rather than the members of one seen so far: any
// `[[vk::...]]` attribute is a binding decision, so the prefix is what the scan
// matches. A material function needs none of them, so the cost of rejecting one
// that would have been harmless is a generator fix, while the cost of missing
// one is a renderer that silently lost part of its pass.
//
// What this proves is structural: the module declares no pass. It does not
// prove the module's arithmetic is free of lighting, which no text scan can
// show and which is the generator's own contract to keep.
//
// `context` is echoed onto every record, so a caller supplies the material
// identity and source document it already knows; the offending construct and
// its line are named in the message, since a record has no field for a source
// position and a byte offset would not survive a reformat anyway.
[[nodiscard]] std::vector<MaterialDiagnostic> VerifyMaterialSourcePassNeutral(
    std::string_view source, const MaterialDiagnosticContext& context = {},
    const MaterialFallbackPolicy& policy = {});

}  // namespace merlin
