#pragma once

#include <merlin/core/diagnostic.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace merlin {

// Material generation and compilation fail in ways a generic renderer
// diagnostic cannot describe: the interesting part is which element of which
// document was rejected, and which rung of the fallback ladder the renderer
// dropped to as a result.
//
// This contract owns that vocabulary in Core so a MaterialX integration, a
// Hydra network translator, and a future material source all classify the same
// failure the same way. It carries no MaterialX, Slang, Vulkan, or host type;
// producers describe their own versions and targets as opaque strings.
//
// A record here is not a replacement for `merlin-diagnostic/v1`. It is the
// richer producer-side record that `ToDiagnostic` flattens onto that sink, so
// hosts keep one diagnostic stream while evidence consumers keep the structure.

inline constexpr std::string_view kMaterialDiagnosticSchema =
    "merlin.material-diagnostic/v1";
inline constexpr std::uint32_t kMaterialDiagnosticSchemaVersion = 1;

// The failure categories the material boundary is required to distinguish.
// They are grouped by the layer that detects them, because that is what
// determines which recovery is even available.
enum class MaterialDiagnosticCategory {
  // Source document: unreadable, invalid, or without a usable renderable.
  InvalidDocument,
  RenderableNotFound,
  AmbiguousRenderable,
  // Feature slice: the document is valid but asks for something outside it.
  UnsupportedNode,
  UnsupportedInput,
  UnsupportedConversion,
  // Dependencies the generator needed and did not get.
  MissingLibrary,
  MissingInclude,
  MissingTexture,
  // Generation and target compilation.
  GenerationFailure,
  CompileFailure,
  TargetFailure,
  // Interface agreement between a generated module and its consumer.
  ReflectionMismatch,
  AbiMismatch,
  // Persisted artifacts that cannot be trusted or reused.
  CacheCorrupt,
  CacheIncompatible,
};

// The fallback ladder from the MaterialXGenSlang boundary design, ordered by
// how much of the authored material survives. `None` means the material
// generated successfully and nothing was substituted.
//
// A fallback is a recovery, never evidence of coverage: a material that landed
// on `BasicMaterial` is not a supported material.
enum class MaterialFallback {
  None,
  // A diagnosed supported simplification of the authored graph was evaluated.
  Simplification,
  // The existing constant-base-color basic material.
  BasicMaterial,
  // The explicit error material, used when nothing else may be substituted.
  ErrorMaterial,
};

// Which rungs a host permits. Descending the ladder is a policy decision: a
// batch render may prefer an obvious error material over a silently
// approximated one, while an interactive viewport prefers to keep drawing.
struct MaterialFallbackPolicy {
  bool allow_simplification{true};
  bool allow_basic_material{true};
};

// Everything a record carries about *where* a failure happened. Every field is
// optional because producers detect failures at different depths; an empty
// field is omitted from the bridged diagnostic rather than reported as empty.
//
// A producer fills a field only when it can attribute the failure that
// precisely. Guessing is worse than leaving a field empty: a host that reads a
// wrong element or node is pointed away from the actual failure.
struct MaterialDiagnosticContext {
  // Identity of the material or generated module, when one exists. A failure
  // before generation has none.
  std::string material_identity;
  // Hierarchical path of the offending element within the source document, or
  // the most specific enclosing element the producer could attribute. A failure
  // detected after generation, against reflected interface rather than against
  // a document element, may only be able to name the renderable.
  std::string element_path;
  // Node category (the authored node type) and input name, when the failure is
  // attributable to one node or one input. A failure detected against reflected
  // interface has no authored node to name and leaves the category empty.
  std::string node_category;
  std::string input_name;
  // Logical identifier of the source document. Never a host-absolute path.
  std::string source_document;
  // Backend target spelling for compile and target failures, e.g. "spirv".
  std::string backend_target;
  // Producer-spelled versions. A value names what it is a version *of*, because
  // one producer may report a library version where another reports a code
  // generator version, e.g. "MaterialX/1.39.6" or "MaterialXGenSlang/1.0".
  std::string generator_version;
  std::string compiler_version;
};

struct MaterialDiagnostic {
  std::uint32_t schema_version{kMaterialDiagnosticSchemaVersion};
  MaterialDiagnosticCategory category{
      MaterialDiagnosticCategory::GenerationFailure};
  DiagnosticSeverity severity{DiagnosticSeverity::Error};
  // The rung actually taken. Producers that cannot substitute a material leave
  // this at the value `SelectMaterialFallback` returned for their policy. A
  // record that did not cost the material anything, such as a warning against a
  // material that still generated, stays at `None`, so evidence never counts a
  // substitution the renderer never made.
  MaterialFallback fallback{MaterialFallback::ErrorMaterial};
  std::string message;
  MaterialDiagnosticContext context;
};

// Stable, host-facing identifiers. These are contract surface: they appear in
// diagnostics and evidence, so they are spelled once here.
[[nodiscard]] std::string_view MaterialDiagnosticCode(
    MaterialDiagnosticCategory category) noexcept;
[[nodiscard]] std::string_view MaterialFallbackName(
    MaterialFallback fallback) noexcept;

// The lowest-cost rung the policy allows for this category.
//
// Only categories whose failure is confined to part of a graph can be
// simplified; a document that failed to parse, a compile that failed, or a
// cache that cannot be trusted has nothing to simplify toward. When the policy
// forbids a rung, selection continues down the ladder rather than failing.
//
// This answers what recovery the category *permits*. It does not know whether
// the producer performed it; see `ResolveMaterialFallback`.
[[nodiscard]] MaterialFallback SelectMaterialFallback(
    MaterialDiagnosticCategory category,
    const MaterialFallbackPolicy& policy = {}) noexcept;

// The rung actually taken, given whether the producer emitted a usable
// simplified material.
//
// A producer that rejects a document outright has simplified nothing, even
// when the category would have allowed it. Reporting `Simplification` there
// would claim coverage that was never generated, so selection descends to the
// next permitted rung instead.
[[nodiscard]] MaterialFallback ResolveMaterialFallback(
    MaterialDiagnosticCategory category, bool simplified_material_produced,
    const MaterialFallbackPolicy& policy = {}) noexcept;

// Flatten a material record onto `merlin-diagnostic/v1`. The context survives
// as `key=value` pairs appended to the message, in declaration order, so a host
// that only understands v1 still sees every field the record carried.
//
// That tail is a rendering for a host that has no structured channel, not an
// encoding: values are written verbatim, so one containing a space is not
// separable again. A consumer that needs the fields back reads
// `MaterialDiagnostic` itself, which is why producers bridge rather than parse.
//
// `disposition` distinguishes the three outcomes v1 already spells: a
// substituted material is a `Fallback`, an error that substituted nothing is a
// `Rejected`, and a record that cost the material nothing, such as a warning on
// a material that still generated, is `Ignored`.
[[nodiscard]] Diagnostic ToDiagnostic(const MaterialDiagnostic& diagnostic);

// Aggregated fallback evidence for one material, one frame, or one build,
// depending on the scope the caller resets it over.
//
// `effective_fallback` is the *worst* rung any recorded diagnostic landed on,
// which is the honest summary: a material that simplified one node and dropped
// another to the basic material did not get simplification-quality output.
struct MaterialFallbackEvidence {
  std::uint64_t recorded_count{};
  std::uint64_t simplification_count{};
  std::uint64_t basic_material_count{};
  std::uint64_t error_material_count{};
  MaterialFallback effective_fallback{MaterialFallback::None};

  void Record(MaterialFallback fallback) noexcept;
  void Record(const MaterialDiagnostic& diagnostic) noexcept;

  // True when any material was substituted. Capability evidence uses this to
  // keep a fallback from being reported as MaterialX coverage.
  [[nodiscard]] bool fallback_taken() const noexcept {
    return effective_fallback != MaterialFallback::None;
  }

  friend constexpr bool operator==(const MaterialFallbackEvidence&,
                                   const MaterialFallbackEvidence&) = default;
};

// Deterministic single-line summary for capability and telemetry evidence, in
// the repository's existing `key=value` diagnostic style.
[[nodiscard]] std::string MakeMaterialFallbackEvidenceRecord(
    const MaterialFallbackEvidence& evidence);

}  // namespace merlin
