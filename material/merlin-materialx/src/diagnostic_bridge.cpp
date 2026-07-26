#include <merlin/materialx/diagnostic_bridge.hpp>

namespace merlin::materialx {

merlin::MaterialDiagnosticCategory ToMaterialDiagnosticCategory(
    DiagnosticCode code) noexcept {
  switch (code) {
    case DiagnosticCode::InvalidDocument:
      return merlin::MaterialDiagnosticCategory::InvalidDocument;
    case DiagnosticCode::MissingStandardLibrary:
      return merlin::MaterialDiagnosticCategory::MissingLibrary;
    case DiagnosticCode::RenderableNotFound:
      return merlin::MaterialDiagnosticCategory::RenderableNotFound;
    case DiagnosticCode::AmbiguousRenderable:
      return merlin::MaterialDiagnosticCategory::AmbiguousRenderable;
    // A renderable whose type the slice does not accept is rejected for the
    // same reason an interior node is: the authored node is outside the
    // supported set.
    case DiagnosticCode::UnsupportedRenderable:
    case DiagnosticCode::UnsupportedNode:
      return merlin::MaterialDiagnosticCategory::UnsupportedNode;
    case DiagnosticCode::UnsupportedInput:
      return merlin::MaterialDiagnosticCategory::UnsupportedInput;
    case DiagnosticCode::UnsupportedConversion:
      return merlin::MaterialDiagnosticCategory::UnsupportedConversion;
    case DiagnosticCode::MissingInclude:
      return merlin::MaterialDiagnosticCategory::MissingInclude;
    case DiagnosticCode::MissingTexture:
      return merlin::MaterialDiagnosticCategory::MissingTexture;
    // The document is fine and the graph is supported; the generator emitted
    // something outside the boundary it was given, which is a failure of
    // generation rather than of the material that was authored.
    case DiagnosticCode::GeneratedPassDeclaration:
    case DiagnosticCode::GenerationFailure:
      return merlin::MaterialDiagnosticCategory::GenerationFailure;
  }
  return merlin::MaterialDiagnosticCategory::GenerationFailure;
}

merlin::DiagnosticSeverity ToDiagnosticSeverity(
    DiagnosticSeverity severity) noexcept {
  switch (severity) {
    case DiagnosticSeverity::Warning:
      return merlin::DiagnosticSeverity::Warning;
    case DiagnosticSeverity::Error:
      return merlin::DiagnosticSeverity::Error;
  }
  return merlin::DiagnosticSeverity::Error;
}

merlin::MaterialDiagnostic ToMaterialDiagnostic(
    const Diagnostic& diagnostic, const CompileResult& result,
    const merlin::MaterialFallbackPolicy& policy) {
  merlin::MaterialDiagnostic bridged;
  bridged.category = ToMaterialDiagnosticCategory(diagnostic.code);
  bridged.severity = ToDiagnosticSeverity(diagnostic.severity);
  // Only a failure costs a material. A warning leaves the compile intact, so it
  // claims no rung: descending the ladder for it would report a substitution
  // the renderer never performed, and would count one in the evidence.
  //
  // For a failure, a module in hand is the only proof that a usable
  // simplification exists; every early return in the compiler leaves none.
  bridged.fallback =
      bridged.severity == merlin::DiagnosticSeverity::Error
          ? merlin::ResolveMaterialFallback(bridged.category,
                                            result.module.has_value(), policy)
          : merlin::MaterialFallback::None;
  bridged.message = diagnostic.message;
  bridged.context.material_identity =
      result.module ? result.module->module_key : std::string{};
  bridged.context.element_path = diagnostic.element_path;
  bridged.context.node_category = diagnostic.node_category;
  bridged.context.input_name = diagnostic.input_name;
  bridged.context.source_document = result.source_document;
  // The version names what it is a version of: a document rejected before the
  // generator existed can still report which library read it, and a host must
  // not have to guess which of the two it is looking at.
  bridged.context.generator_version =
      result.generator_version.empty()
          ? "MaterialX/" + result.materialx_version
          : "MaterialXGenSlang/" + result.generator_version;
  // Document-level failures happen before any target is chosen, so
  // `backend_target` stays empty rather than guessing one.
  return bridged;
}

merlin::MaterialFallbackEvidence ReportCompileDiagnostics(
    const CompileResult& result, merlin::DiagnosticSink& sink,
    const merlin::MaterialFallbackPolicy& policy) {
  merlin::MaterialFallbackEvidence evidence;
  for (const auto& diagnostic : result.diagnostics) {
    const auto bridged = ToMaterialDiagnostic(diagnostic, result, policy);
    evidence.Record(bridged);
    sink.Report(merlin::ToDiagnostic(bridged));
  }
  return evidence;
}

}  // namespace merlin::materialx
