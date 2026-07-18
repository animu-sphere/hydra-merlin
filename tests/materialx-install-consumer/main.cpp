#include <merlin/materialx/compiler.hpp>

int main() {
  const auto result = merlin::materialx::CompileMaterialFunction(
      "<materialx version=\"1.39\" />");
  return !result && !result.diagnostics.empty() &&
                 result.diagnostics.front().code ==
                     merlin::materialx::DiagnosticCode::MissingStandardLibrary
             ? 0
             : 1;
}
