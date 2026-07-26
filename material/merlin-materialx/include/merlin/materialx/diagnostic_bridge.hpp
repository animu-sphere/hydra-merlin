#pragma once

#include <merlin/core/diagnostic.hpp>
#include <merlin/core/material_diagnostic.hpp>
#include <merlin/materialx/compiler.hpp>

namespace merlin::materialx {

// Integration-local diagnostics stay MaterialX-shaped so the compiler can name
// what it actually saw. Hosts never consume that shape: this bridge classifies
// each record against the Core material contract and flattens it onto the
// existing `merlin-diagnostic/v1` sink, so Hydra, the viewport, and headless
// runs all receive one diagnostic stream.
//
// Nothing here exposes a MaterialX SDK type in either direction.

[[nodiscard]] merlin::MaterialDiagnosticCategory ToMaterialDiagnosticCategory(
    DiagnosticCode code) noexcept;

[[nodiscard]] merlin::DiagnosticSeverity ToDiagnosticSeverity(
    DiagnosticSeverity severity) noexcept;

// Classify one compile diagnostic, filling its context from the result that
// produced it. The fallback is resolved against what the compile actually
// emitted, so a rejected document never reports a simplification it never
// generated.
[[nodiscard]] merlin::MaterialDiagnostic ToMaterialDiagnostic(
    const Diagnostic& diagnostic, const CompileResult& result,
    const merlin::MaterialFallbackPolicy& policy = {});

// Report every diagnostic in `result` to a host sink and return the fallback
// evidence the report implies. A clean compile reports nothing and yields
// evidence whose effective fallback is `None`.
merlin::MaterialFallbackEvidence ReportCompileDiagnostics(
    const CompileResult& result, merlin::DiagnosticSink& sink,
    const merlin::MaterialFallbackPolicy& policy = {});

}  // namespace merlin::materialx
