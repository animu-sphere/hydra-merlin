#include <merlin/materialx/compiler.hpp>
#include <merlin/materialx/diagnostic_bridge.hpp>

#include <vector>

namespace {

class CollectingSink final : public merlin::DiagnosticSink {
 public:
  void Report(const merlin::Diagnostic& diagnostic) override {
    reported.push_back(diagnostic);
  }

  std::vector<merlin::Diagnostic> reported;
};

}  // namespace

int main() {
  const auto result = merlin::materialx::CompileMaterialFunction(
      "<materialx version=\"1.39\" />");
  if (result || result.diagnostics.empty() ||
      result.diagnostics.front().code !=
          merlin::materialx::DiagnosticCode::MissingStandardLibrary) {
    return 1;
  }

  // The diagnostic bridge is part of the installed public boundary, so an
  // install-tree consumer must reach the Core contract through it and receive
  // the same classification and named recovery a build-tree consumer does.
  CollectingSink sink;
  const auto evidence =
      merlin::materialx::ReportCompileDiagnostics(result, sink);
  return sink.reported.size() == result.diagnostics.size() &&
                 sink.reported.front().code ==
                     "material.dependency.library-missing" &&
                 sink.reported.front().recovery == "basic-material" &&
                 evidence.effective_fallback ==
                     merlin::MaterialFallback::BasicMaterial
             ? 0
             : 1;
}
