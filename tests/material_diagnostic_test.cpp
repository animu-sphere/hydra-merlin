// The material diagnostic contract is what a host sees when a material does
// not compile: which category failed, and which material it got instead. Both
// halves are contract surface, so this test pins the stable codes, the fallback
// ladder, the flattening onto merlin-diagnostic/v1, and the evidence summary.
//
// It links only Merlin::RenderWorld, so it runs in a MaterialX-disabled build
// as well; the vocabulary belongs to Core, not to any one producer.

#include <merlin/core/material_diagnostic.hpp>

#include <array>
#include <cassert>
#include <iostream>
#include <set>
#include <string>
#include <string_view>

namespace {

using merlin::MaterialDiagnostic;
using merlin::MaterialDiagnosticCategory;
using merlin::MaterialFallback;
using merlin::MaterialFallbackEvidence;
using merlin::MaterialFallbackPolicy;

// Every category, so a value added to the enum without a code or a fallback
// rule fails here rather than silently reporting the default.
constexpr std::array kAllCategories{
    MaterialDiagnosticCategory::InvalidDocument,
    MaterialDiagnosticCategory::RenderableNotFound,
    MaterialDiagnosticCategory::AmbiguousRenderable,
    MaterialDiagnosticCategory::UnsupportedNode,
    MaterialDiagnosticCategory::UnsupportedInput,
    MaterialDiagnosticCategory::UnsupportedConversion,
    MaterialDiagnosticCategory::MissingLibrary,
    MaterialDiagnosticCategory::MissingInclude,
    MaterialDiagnosticCategory::MissingTexture,
    MaterialDiagnosticCategory::GenerationFailure,
    MaterialDiagnosticCategory::CompileFailure,
    MaterialDiagnosticCategory::TargetFailure,
    MaterialDiagnosticCategory::ReflectionMismatch,
    MaterialDiagnosticCategory::AbiMismatch,
    MaterialDiagnosticCategory::CacheCorrupt,
    MaterialDiagnosticCategory::CacheIncompatible,
};

// The categories whose failure is confined to part of a graph, and so may be
// recovered by evaluating a diagnosed simplification.
constexpr std::array kSimplifiableCategories{
    MaterialDiagnosticCategory::UnsupportedNode,
    MaterialDiagnosticCategory::UnsupportedInput,
    MaterialDiagnosticCategory::UnsupportedConversion,
    MaterialDiagnosticCategory::MissingTexture,
};

bool IsSimplifiable(MaterialDiagnosticCategory category) {
  for (const auto candidate : kSimplifiableCategories) {
    if (candidate == category) {
      return true;
    }
  }
  return false;
}

void VerifyCodesAreStableAndDistinct() {
  std::set<std::string_view> codes;
  for (const auto category : kAllCategories) {
    const auto code = merlin::MaterialDiagnosticCode(category);
    assert(!code.empty());
    // A shared prefix keeps material diagnostics filterable next to the
    // existing hydra.* codes.
    assert(code.starts_with("material."));
    assert(codes.insert(code).second);
  }
  assert(codes.size() == kAllCategories.size());

  std::set<std::string_view> names;
  for (const auto fallback :
       {MaterialFallback::None, MaterialFallback::Simplification,
        MaterialFallback::BasicMaterial, MaterialFallback::ErrorMaterial}) {
    assert(names.insert(merlin::MaterialFallbackName(fallback)).second);
  }
  assert(names.size() == 4U);
}

void VerifyFallbackLadder() {
  // Default policy: partial failures simplify, total failures fall back to the
  // basic material.
  for (const auto category : kAllCategories) {
    const auto selected = merlin::SelectMaterialFallback(category);
    assert(selected == (IsSimplifiable(category)
                            ? MaterialFallback::Simplification
                            : MaterialFallback::BasicMaterial));
  }

  // A host that refuses approximated materials still keeps drawing, one rung
  // lower.
  const MaterialFallbackPolicy no_simplification{false, true};
  for (const auto category : kAllCategories) {
    assert(merlin::SelectMaterialFallback(category, no_simplification) ==
           MaterialFallback::BasicMaterial);
  }

  // A host that refuses both substitutions gets the explicit error material,
  // never a silent success.
  const MaterialFallbackPolicy strict{false, false};
  for (const auto category : kAllCategories) {
    assert(merlin::SelectMaterialFallback(category, strict) ==
           MaterialFallback::ErrorMaterial);
  }

  // Simplification is only permitted, never assumed: a producer that rejected
  // the document outright simplified nothing, so the ladder descends even for
  // a category that would have allowed it.
  for (const auto category : kAllCategories) {
    assert(merlin::ResolveMaterialFallback(category, true) ==
           merlin::SelectMaterialFallback(category));
    assert(merlin::ResolveMaterialFallback(category, false) ==
           MaterialFallback::BasicMaterial);
    assert(merlin::ResolveMaterialFallback(category, false, strict) ==
           MaterialFallback::ErrorMaterial);
  }
}

MaterialDiagnostic MakeUnsupportedNodeDiagnostic() {
  MaterialDiagnostic diagnostic;
  diagnostic.category = MaterialDiagnosticCategory::UnsupportedNode;
  diagnostic.severity = merlin::DiagnosticSeverity::Error;
  diagnostic.fallback = MaterialFallback::BasicMaterial;
  diagnostic.message = "Unsupported MaterialX node category: noise2d";
  diagnostic.context.material_identity = "sha256:abc";
  diagnostic.context.element_path = "NG_prototype/noise";
  diagnostic.context.node_category = "noise2d";
  diagnostic.context.input_name = "amplitude";
  diagnostic.context.source_document = "materials/prototype.mtlx";
  diagnostic.context.backend_target = "spirv";
  diagnostic.context.generator_version = "1.39.6";
  diagnostic.context.compiler_version = "slangc-2025.10";
  return diagnostic;
}

void VerifyBridgeToDiagnosticV1() {
  const auto material = MakeUnsupportedNodeDiagnostic();
  const auto bridged = merlin::ToDiagnostic(material);

  assert(bridged.schema_version == merlin::kDiagnosticSchemaVersion);
  assert(bridged.code == "material.node.unsupported");
  assert(bridged.severity == merlin::DiagnosticSeverity::Error);
  // A substituted material is a fallback, not a rejection: the frame still
  // rendered something.
  assert(bridged.disposition == merlin::DiagnosticDisposition::Fallback);
  assert(bridged.recovery == "basic-material");
  // The most specific locator wins, so a host can point at the element.
  assert(bridged.source == "NG_prototype/noise");

  // Every context field survives the flattening, in declaration order.
  const auto& message = bridged.message;
  assert(message.starts_with(material.message));
  for (const std::string_view field :
       {"material=sha256:abc", "element=NG_prototype/noise",
        "node=noise2d", "input=amplitude",
        "document=materials/prototype.mtlx", "target=spirv",
        "generator=1.39.6", "compiler=slangc-2025.10"}) {
    assert(message.find(field) != std::string::npos);
  }
  assert(message.find("material=") < message.find("element="));
  assert(message.find("generator=") < message.find("compiler="));

  // Absent context is omitted rather than reported as empty, and a record that
  // substituted nothing is a rejection.
  MaterialDiagnostic sparse;
  sparse.category = MaterialDiagnosticCategory::InvalidDocument;
  sparse.fallback = MaterialFallback::None;
  sparse.message = "document did not parse";
  sparse.context.source_document = "materials/broken.mtlx";
  const auto sparse_bridged = merlin::ToDiagnostic(sparse);
  assert(sparse_bridged.disposition == merlin::DiagnosticDisposition::Rejected);
  assert(sparse_bridged.recovery == "none");
  // With no element path, the document identifies the source.
  assert(sparse_bridged.source == "materials/broken.mtlx");
  assert(sparse_bridged.message.find("element=") == std::string::npos);
  assert(sparse_bridged.message.find("node=") == std::string::npos);
  assert(sparse_bridged.message.find("document=materials/broken.mtlx") !=
         std::string::npos);
}

void VerifyEvidence() {
  MaterialFallbackEvidence evidence;
  assert(!evidence.fallback_taken());
  assert(evidence.effective_fallback == MaterialFallback::None);

  evidence.Record(MaterialFallback::None);
  assert(!evidence.fallback_taken());
  assert(evidence.recorded_count == 1U);

  evidence.Record(MaterialFallback::Simplification);
  assert(evidence.fallback_taken());
  assert(evidence.effective_fallback == MaterialFallback::Simplification);

  // The summary reports the worst rung reached, not the last one recorded: a
  // material that dropped one node to the basic material did not get
  // simplification-quality output overall.
  evidence.Record(MaterialFallback::BasicMaterial);
  evidence.Record(MaterialFallback::Simplification);
  assert(evidence.effective_fallback == MaterialFallback::BasicMaterial);

  evidence.Record(MakeUnsupportedNodeDiagnostic());
  assert(evidence.recorded_count == 5U);
  assert(evidence.simplification_count == 2U);
  assert(evidence.basic_material_count == 2U);
  assert(evidence.error_material_count == 0U);

  evidence.Record(MaterialFallback::ErrorMaterial);
  assert(evidence.effective_fallback == MaterialFallback::ErrorMaterial);
  assert(evidence.error_material_count == 1U);

  const auto record = merlin::MakeMaterialFallbackEvidenceRecord(evidence);
  assert(record.find("schema=merlin.material-diagnostic/v1") == 0U);
  assert(record.find("recorded=6") != std::string::npos);
  assert(record.find("simplification=2") != std::string::npos);
  assert(record.find("basic-material=2") != std::string::npos);
  assert(record.find("error-material=1") != std::string::npos);
  assert(record.find("fallback=error-material") != std::string::npos);

  // The record is a pure function of the evidence, so build and frame evidence
  // can be compared across runs.
  MaterialFallbackEvidence replayed;
  replayed.Record(MaterialFallback::None);
  replayed.Record(MaterialFallback::Simplification);
  replayed.Record(MaterialFallback::BasicMaterial);
  replayed.Record(MaterialFallback::Simplification);
  replayed.Record(MaterialFallback::BasicMaterial);
  replayed.Record(MaterialFallback::ErrorMaterial);
  assert(replayed == evidence);
  assert(merlin::MakeMaterialFallbackEvidenceRecord(replayed) == record);
}

}  // namespace

int main() {
  VerifyCodesAreStableAndDistinct();
  VerifyFallbackLadder();
  VerifyBridgeToDiagnosticV1();
  VerifyEvidence();
  std::cout << "material diagnostic contract ok\n";
  return 0;
}
