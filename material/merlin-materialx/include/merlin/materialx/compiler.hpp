#pragma once

#include <merlin/core/types.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace merlin::materialx {

enum class DiagnosticSeverity { Warning, Error };

// Failures this integration can actually detect. Compile, target, reflection
// agreement, and cache failures are detected by the Slang compilation and
// artifact layers, so they are classified by the Core material contract rather
// than duplicated here.
enum class DiagnosticCode {
  InvalidDocument,
  MissingStandardLibrary,
  RenderableNotFound,
  AmbiguousRenderable,
  UnsupportedRenderable,
  UnsupportedNode,
  UnsupportedInput,
  // A reflected value whose MaterialX type has no MaterialIR equivalent, or
  // whose authored default cannot be read as its declared type. Distinct from
  // UnsupportedInput because the graph is fine and the boundary crossing is
  // not.
  UnsupportedConversion,
  // A generator source include or standard-library document that could not be
  // read, or that resolved outside every registered data root.
  MissingInclude,
  // An image resource the host can never resolve, because the document left it
  // without a filename.
  MissingTexture,
  GenerationFailure,
};

struct Diagnostic {
  DiagnosticSeverity severity{DiagnosticSeverity::Error};
  DiagnosticCode code{DiagnosticCode::GenerationFailure};
  std::string element_path;
  std::string message;
  // Authored node type, when the failure is attributable to one node.
  std::string node_category;
  // Input or reflected variable name, when it is attributable to one input.
  std::string input_name;
};

struct MaterialFunctionPort {
  std::string block;
  std::string name;
  std::string variable;
  std::string type;
  std::string default_value;
};

struct MaterialResourceDefaultEntry {
  std::string name;
  merlin::MaterialValueType type{merlin::MaterialValueType::Unknown};
  std::vector<std::string> values;
};

struct MaterialResourceDefaultState {
  std::string key;
  std::vector<MaterialResourceDefaultEntry> entries;
};

struct MaterialDependencyFingerprint {
  // Logical path below the selected MaterialX data root. Host-specific
  // absolute paths never participate in portable material identity.
  std::string path;
  std::string content_sha256;
};

struct MaterialFunctionModule {
  std::string source;
  std::string entry_point{"evaluateMaterial"};
  std::string output_type;
  // Topology-only, target-neutral identity. What a compiled artifact of this
  // module is keyed by is a separate question, answered by
  // merlin::MakeShaderArtifactKey with this key as one of its inputs.
  std::string module_key;
  // Runtime uniform values and texture/resource defaults are deliberately
  // identified separately from generated shader topology.
  std::string instance_key;
  std::string resource_key;
  std::string materialx_version;
  std::string generator_version;
  // Merlin's tested upstream compatibility baseline. The actual dependency
  // and generator versions are reported by the two fields above.
  std::string generator_revision;
  // Standard-library documents and transitive generator source includes are
  // tracked separately so cache evidence can explain which dependency class
  // invalidated a topology-only module.
  std::string standard_library_fingerprint;
  std::string source_dependency_fingerprint;
  std::vector<MaterialDependencyFingerprint> standard_library_dependencies;
  std::vector<MaterialDependencyFingerprint> source_dependencies;
  std::vector<MaterialFunctionPort> inputs;
  std::vector<MaterialFunctionPort> uniforms;
  merlin::MaterialModule logical_module;
  // Typed runtime defaults can cross directly into MaterialIR. Resource
  // identifiers remain unresolved until a host adapter maps them to handles.
  merlin::MaterialParameterState parameter_defaults;
  MaterialResourceDefaultState resource_defaults;
};

struct CompileOptions {
  // Hierarchical MaterialX name path for the output or node to compile. When
  // empty, the document must contain exactly one renderable element.
  std::string renderable_path;

  // Each path is a MaterialX data root containing libraries/targets,
  // libraries/stdlib, and libraries/pbrlib.
  std::vector<std::filesystem::path> library_search_paths;

  // Logical identifier for the compiled document, used only to give
  // diagnostics a source. A host-absolute path would make the diagnostic
  // unreproducible, so callers pass an asset-relative or logical name.
  std::string source_document;
};

struct CompileResult {
  std::optional<MaterialFunctionModule> module;
  std::vector<Diagnostic> diagnostics;

  // Echoed so a failed compile still reports where it came from and which
  // versions produced it; a caller holding only diagnostics has no module to
  // read them from.
  std::string source_document;
  std::string materialx_version;
  // Empty when the failure occurred before the generator was constructed.
  std::string generator_version;

  [[nodiscard]] explicit operator bool() const noexcept {
    return module.has_value();
  }
};

// Parse and compile a MaterialX document into a graph-only Slang material
// function. The public boundary intentionally contains no MaterialX SDK types.
[[nodiscard]] CompileResult CompileMaterialFunction(
    std::string_view document_xml, const CompileOptions& options = {});

}  // namespace merlin::materialx
