#pragma once

#include <merlin/core/material_diagnostic.hpp>
#include <merlin/materialx/compiler.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace merlin::materialx::internal {

// The Core pass-neutrality rule applied to this integration's own generated
// source, restated as this integration's diagnostics.
//
// This is declared rather than kept file-local because no MaterialX document
// can make the generator emit a pass declaration: a test that pins the
// translation has to hand it a generated source of its own. Without a seam, the
// step between the Core rule and a `CompileResult` would be the one part of the
// check nothing ever exercises.
//
// `context` is echoed onto the Core records so a caller supplies what it
// already knows about the material; the returned diagnostics carry the messages
// those records produced, under `element_path`, since the offending construct
// is named in the message rather than in a source position field.
[[nodiscard]] std::vector<Diagnostic> DiagnosePassDeclarations(
    std::string_view source, std::string element_path,
    const merlin::MaterialDiagnosticContext& context = {});

}  // namespace merlin::materialx::internal
