#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace merlin {

inline constexpr std::string_view kDiagnosticSchema = "merlin-diagnostic/v1";
inline constexpr std::uint32_t kDiagnosticSchemaVersion = 1;

enum class DiagnosticSeverity { Info, Warning, Error };
enum class DiagnosticDisposition { Fallback, Rejected, Ignored };

// Host-neutral, versioned diagnostic passed from ingestion or backend
// boundaries to a host-provided sink. `recovery` names the exact fallback or
// rejection policy so callers never need to infer behavior from prose.
struct Diagnostic {
  std::uint32_t schema_version{kDiagnosticSchemaVersion};
  std::string code;
  DiagnosticSeverity severity{DiagnosticSeverity::Warning};
  DiagnosticDisposition disposition{DiagnosticDisposition::Rejected};
  std::string source;
  std::string message;
  std::string recovery;
};

class DiagnosticSink {
 public:
  virtual ~DiagnosticSink() = default;
  virtual void Report(const Diagnostic& diagnostic) = 0;
};

}  // namespace merlin
