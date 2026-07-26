#include <merlin/core/material_diagnostic.hpp>

#include <utility>

namespace merlin {
namespace {

// Appends `key=value` only when the value is present, so an absent context
// field is silently omitted instead of reported as an empty one.
void AppendContextField(std::string& text, std::string_view key,
                        const std::string& value) {
  if (value.empty()) {
    return;
  }
  if (!text.empty()) {
    text += ' ';
  }
  text += key;
  text += '=';
  text += value;
}

std::string MakeContextRecord(const MaterialDiagnosticContext& context) {
  std::string record;
  AppendContextField(record, "material", context.material_identity);
  AppendContextField(record, "element", context.element_path);
  AppendContextField(record, "node", context.node_category);
  AppendContextField(record, "input", context.input_name);
  AppendContextField(record, "document", context.source_document);
  AppendContextField(record, "target", context.backend_target);
  AppendContextField(record, "generator", context.generator_version);
  AppendContextField(record, "compiler", context.compiler_version);
  return record;
}

// The most specific locator the record carries, used as the v1 `source`.
const std::string& DiagnosticSource(const MaterialDiagnosticContext& context) {
  if (!context.element_path.empty()) {
    return context.element_path;
  }
  if (!context.source_document.empty()) {
    return context.source_document;
  }
  return context.material_identity;
}

}  // namespace

std::string_view MaterialDiagnosticCode(
    MaterialDiagnosticCategory category) noexcept {
  switch (category) {
    case MaterialDiagnosticCategory::InvalidDocument:
      return "material.document.invalid";
    case MaterialDiagnosticCategory::RenderableNotFound:
      return "material.document.renderable-not-found";
    case MaterialDiagnosticCategory::AmbiguousRenderable:
      return "material.document.renderable-ambiguous";
    case MaterialDiagnosticCategory::UnsupportedNode:
      return "material.node.unsupported";
    case MaterialDiagnosticCategory::UnsupportedInput:
      return "material.input.unsupported";
    case MaterialDiagnosticCategory::UnsupportedConversion:
      return "material.conversion.unsupported";
    case MaterialDiagnosticCategory::MissingLibrary:
      return "material.dependency.library-missing";
    case MaterialDiagnosticCategory::MissingInclude:
      return "material.dependency.include-missing";
    case MaterialDiagnosticCategory::MissingTexture:
      return "material.dependency.texture-missing";
    case MaterialDiagnosticCategory::GenerationFailure:
      return "material.generation.failed";
    case MaterialDiagnosticCategory::CompileFailure:
      return "material.compile.failed";
    case MaterialDiagnosticCategory::TargetFailure:
      return "material.target.failed";
    case MaterialDiagnosticCategory::ReflectionMismatch:
      return "material.reflection.mismatch";
    case MaterialDiagnosticCategory::AbiMismatch:
      return "material.abi.mismatch";
    case MaterialDiagnosticCategory::CacheCorrupt:
      return "material.cache.corrupt";
    case MaterialDiagnosticCategory::CacheIncompatible:
      return "material.cache.incompatible";
  }
  return "material.generation.failed";
}

std::string_view MaterialFallbackName(MaterialFallback fallback) noexcept {
  switch (fallback) {
    case MaterialFallback::None:
      return "none";
    case MaterialFallback::Simplification:
      return "simplification";
    case MaterialFallback::BasicMaterial:
      return "basic-material";
    case MaterialFallback::ErrorMaterial:
      return "error-material";
  }
  return "error-material";
}

MaterialFallback SelectMaterialFallback(
    MaterialDiagnosticCategory category,
    const MaterialFallbackPolicy& policy) noexcept {
  switch (category) {
    // A rejected node, input, or conversion leaves the rest of the graph
    // intact, so a diagnosed simplification can still evaluate.
    case MaterialDiagnosticCategory::UnsupportedNode:
    case MaterialDiagnosticCategory::UnsupportedInput:
    case MaterialDiagnosticCategory::UnsupportedConversion:
    case MaterialDiagnosticCategory::MissingTexture:
      if (policy.allow_simplification) {
        return MaterialFallback::Simplification;
      }
      [[fallthrough]];
    // Nothing generated, nothing compiled, or nothing trustworthy was loaded.
    // There is no partial graph to simplify toward, so the basic material is
    // the best remaining rung.
    case MaterialDiagnosticCategory::InvalidDocument:
    case MaterialDiagnosticCategory::RenderableNotFound:
    case MaterialDiagnosticCategory::AmbiguousRenderable:
    case MaterialDiagnosticCategory::MissingLibrary:
    case MaterialDiagnosticCategory::MissingInclude:
    case MaterialDiagnosticCategory::GenerationFailure:
    case MaterialDiagnosticCategory::CompileFailure:
    case MaterialDiagnosticCategory::TargetFailure:
    case MaterialDiagnosticCategory::ReflectionMismatch:
    case MaterialDiagnosticCategory::AbiMismatch:
    case MaterialDiagnosticCategory::CacheCorrupt:
    case MaterialDiagnosticCategory::CacheIncompatible:
      return policy.allow_basic_material ? MaterialFallback::BasicMaterial
                                         : MaterialFallback::ErrorMaterial;
  }
  return MaterialFallback::ErrorMaterial;
}

MaterialFallback ResolveMaterialFallback(
    MaterialDiagnosticCategory category, bool simplified_material_produced,
    const MaterialFallbackPolicy& policy) noexcept {
  const auto selected = SelectMaterialFallback(category, policy);
  if (selected != MaterialFallback::Simplification ||
      simplified_material_produced) {
    return selected;
  }
  return policy.allow_basic_material ? MaterialFallback::BasicMaterial
                                     : MaterialFallback::ErrorMaterial;
}

Diagnostic ToDiagnostic(const MaterialDiagnostic& diagnostic) {
  Diagnostic bridged;
  bridged.schema_version = kDiagnosticSchemaVersion;
  bridged.code = std::string(MaterialDiagnosticCode(diagnostic.category));
  bridged.severity = diagnostic.severity;
  bridged.disposition = diagnostic.fallback == MaterialFallback::None
                            ? DiagnosticDisposition::Rejected
                            : DiagnosticDisposition::Fallback;
  bridged.source = DiagnosticSource(diagnostic.context);
  bridged.message = diagnostic.message;
  if (auto context = MakeContextRecord(diagnostic.context); !context.empty()) {
    if (!bridged.message.empty()) {
      bridged.message += " (";
      bridged.message += context;
      bridged.message += ')';
    } else {
      bridged.message = std::move(context);
    }
  }
  bridged.recovery = std::string(MaterialFallbackName(diagnostic.fallback));
  return bridged;
}

void MaterialFallbackEvidence::Record(MaterialFallback fallback) noexcept {
  ++recorded_count;
  switch (fallback) {
    case MaterialFallback::None:
      break;
    case MaterialFallback::Simplification:
      ++simplification_count;
      break;
    case MaterialFallback::BasicMaterial:
      ++basic_material_count;
      break;
    case MaterialFallback::ErrorMaterial:
      ++error_material_count;
      break;
  }
  // The ladder is declared worst-last, so the higher enumerator is the more
  // degraded result and wins the summary.
  if (fallback > effective_fallback) {
    effective_fallback = fallback;
  }
}

void MaterialFallbackEvidence::Record(
    const MaterialDiagnostic& diagnostic) noexcept {
  Record(diagnostic.fallback);
}

std::string MakeMaterialFallbackEvidenceRecord(
    const MaterialFallbackEvidence& evidence) {
  std::string record;
  record += "schema=";
  record += kMaterialDiagnosticSchema;
  record += " recorded=";
  record += std::to_string(evidence.recorded_count);
  record += " simplification=";
  record += std::to_string(evidence.simplification_count);
  record += " basic-material=";
  record += std::to_string(evidence.basic_material_count);
  record += " error-material=";
  record += std::to_string(evidence.error_material_count);
  record += " fallback=";
  record += MaterialFallbackName(evidence.effective_fallback);
  return record;
}

}  // namespace merlin
