#include "developer_ui.hpp"

#include <algorithm>
#include <utility>

namespace merlin::viewport {
namespace {

bool SameDiagnostic(const DeveloperUiDiagnosticEntry& entry,
                    DeveloperUiDiagnosticOrigin origin,
                    const Diagnostic& diagnostic) {
  return entry.origin == origin &&
         entry.diagnostic.schema_version == diagnostic.schema_version &&
         entry.diagnostic.code == diagnostic.code &&
         entry.diagnostic.severity == diagnostic.severity &&
         entry.diagnostic.disposition == diagnostic.disposition &&
         entry.diagnostic.source == diagnostic.source &&
         entry.diagnostic.message == diagnostic.message &&
         entry.diagnostic.recovery == diagnostic.recovery;
}

}  // namespace

DeveloperUiDiagnosticHistory::DeveloperUiDiagnosticHistory(
    std::size_t capacity)
    : capacity_(std::max<std::size_t>(capacity, 1U)) {}

void DeveloperUiDiagnosticHistory::Report(const Diagnostic& diagnostic) {
  std::scoped_lock lock(mutex_);
  const auto frame_index = frame_index_;
  if (!entries_.empty() &&
      SameDiagnostic(entries_.back(), DeveloperUiDiagnosticOrigin::Backend,
                     diagnostic) &&
      (frame_index <= entries_.back().last_frame ||
       frame_index - entries_.back().last_frame <= 1U)) {
    entries_.back().last_frame = std::max(entries_.back().last_frame,
                                          frame_index);
    ++entries_.back().occurrences;
    return;
  }
  if (entries_.size() == capacity_) {
    entries_.erase(entries_.begin());
  }
  entries_.push_back({next_sequence_++, frame_index, frame_index, 1,
                      DeveloperUiDiagnosticOrigin::Backend, diagnostic});
}

void DeveloperUiDiagnosticHistory::SetFrameIndex(
    std::uint64_t frame_index) noexcept {
  std::scoped_lock lock(mutex_);
  frame_index_ = frame_index;
}

void DeveloperUiDiagnosticHistory::Record(
    DeveloperUiDiagnosticOrigin origin, std::uint64_t frame_index,
    const Diagnostic& diagnostic) {
  std::scoped_lock lock(mutex_);
  if (!entries_.empty() && SameDiagnostic(entries_.back(), origin, diagnostic) &&
      (frame_index <= entries_.back().last_frame ||
       frame_index - entries_.back().last_frame <= 1U)) {
    entries_.back().last_frame = std::max(entries_.back().last_frame,
                                          frame_index);
    ++entries_.back().occurrences;
    return;
  }
  if (entries_.size() == capacity_) {
    entries_.erase(entries_.begin());
  }
  entries_.push_back(
      {next_sequence_++, frame_index, frame_index, 1, origin, diagnostic});
}

std::vector<DeveloperUiDiagnosticEntry>
DeveloperUiDiagnosticHistory::Snapshot() const {
  std::scoped_lock lock(mutex_);
  return entries_;
}

void DeveloperUiDiagnosticHistory::Clear() {
  std::scoped_lock lock(mutex_);
  entries_.clear();
}

void RecordDeveloperUiBackendSelection(
    DeveloperUiDiagnosticHistory& history, std::uint64_t frame_index,
    const render::BackendSelection& selection,
    const render::RendererCapabilities& capabilities) {
  Diagnostic selected;
  selected.code = "viewport.backend.selected";
  selected.severity = DiagnosticSeverity::Info;
  selected.disposition = DiagnosticDisposition::Ignored;
  selected.source = capabilities.backend_name;
  selected.message = selection.reason;
  selected.recovery = std::string(render::BackendKindName(selection.selected));
  history.Record(DeveloperUiDiagnosticOrigin::Backend, frame_index, selected);

  if (!capabilities.bindless_textures) {
    history.Record(
        DeveloperUiDiagnosticOrigin::Backend, frame_index,
        {kDiagnosticSchemaVersion, "viewport.backend.bindless-fallback",
         DiagnosticSeverity::Warning, DiagnosticDisposition::Fallback,
         capabilities.backend_name,
         "Bindless textures are unavailable on the selected backend path.",
         "conventional-descriptors"});
  }
  if (!capabilities.generated_materials) {
    history.Record(
        DeveloperUiDiagnosticOrigin::Backend, frame_index,
        {kDiagnosticSchemaVersion,
         "viewport.backend.generated-material-fallback",
         DiagnosticSeverity::Warning, DiagnosticDisposition::Fallback,
         capabilities.backend_name,
         "Generated materials are unavailable on the selected backend path.",
         "basic-material"});
  }
  if (!capabilities.timestamp_queries) {
    history.Record(
        DeveloperUiDiagnosticOrigin::Backend, frame_index,
        {kDiagnosticSchemaVersion, "viewport.backend.gpu-timing-unavailable",
         DiagnosticSeverity::Info, DiagnosticDisposition::Fallback,
         capabilities.backend_name,
         "GPU timestamp queries are unavailable on the selected backend path.",
         "cpu-frame-timing"});
  }
}

void RecordDeveloperUiSettingsFeedback(
    DeveloperUiDiagnosticHistory& history, std::uint64_t frame_index,
    const DeveloperUiSettingsFeedback& feedback) {
  if (feedback.status == DeveloperUiSettingsStatus::None) {
    return;
  }
  const bool rejected =
      feedback.status == DeveloperUiSettingsStatus::Rejected;
  history.Record(
      DeveloperUiDiagnosticOrigin::Host, frame_index,
      {kDiagnosticSchemaVersion,
       rejected ? "viewport.settings.rejected" : "viewport.settings.applied",
       rejected ? DiagnosticSeverity::Warning : DiagnosticSeverity::Info,
       rejected ? DiagnosticDisposition::Rejected
                : DiagnosticDisposition::Ignored,
       "merlin-viewport", feedback.message,
       rejected ? "retain-previous-settings" : "settings-active"});
}

}  // namespace merlin::viewport
