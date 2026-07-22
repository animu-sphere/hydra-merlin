#pragma once

#include <merlin/core/types.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace merlin::materialx {

enum class DiagnosticSeverity { Warning, Error };

enum class DiagnosticCode {
  InvalidDocument,
  MissingStandardLibrary,
  RenderableNotFound,
  AmbiguousRenderable,
  UnsupportedRenderable,
  UnsupportedNode,
  UnsupportedInput,
  GenerationFailure,
};

struct Diagnostic {
  DiagnosticSeverity severity{DiagnosticSeverity::Error};
  DiagnosticCode code{DiagnosticCode::GenerationFailure};
  std::string element_path;
  std::string message;
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
  // Topology-only, target-neutral identity. `cache_key` remains as a
  // compatibility alias for this key.
  std::string module_key;
  std::string cache_key;
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
};

struct CompileResult {
  std::optional<MaterialFunctionModule> module;
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] explicit operator bool() const noexcept {
    return module.has_value();
  }
};

// Parse and compile a MaterialX document into a graph-only Slang material
// function. The public boundary intentionally contains no MaterialX SDK types.
[[nodiscard]] CompileResult CompileMaterialFunction(
    std::string_view document_xml, const CompileOptions& options = {});

}  // namespace merlin::materialx
