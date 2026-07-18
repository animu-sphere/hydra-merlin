#pragma once

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

struct MaterialFunctionModule {
  std::string source;
  std::string entry_point{"evaluateMaterial"};
  std::string output_type;
  std::string cache_key;
  std::string materialx_version;
  std::string generator_version;
  // Merlin's tested upstream compatibility baseline. The actual dependency
  // and generator versions are reported by the two fields above.
  std::string generator_revision;
  std::vector<MaterialFunctionPort> inputs;
  std::vector<MaterialFunctionPort> uniforms;
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
