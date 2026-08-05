#include "developer_ui.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

}  // namespace

int main() {
  try {
    using namespace merlin::viewport;

    merlin::render::RendererCapabilities capabilities;
    capabilities.cpu_readback = true;
    DeveloperUiRendererSettings settings;
    settings.available = true;
    DeveloperUiSettingsFeedback feedback;

    DeveloperUiRendererSettingsRequest request;
    request.clear_color[0] = std::numeric_limits<float>::quiet_NaN();
    Require(!ApplyDeveloperUiRendererSettings(request, capabilities, settings,
                                              feedback),
            "a non-finite clear color was accepted");
    Require(feedback.status == DeveloperUiSettingsStatus::Rejected,
            "invalid color did not report rejection");
    Require(feedback.serial == 1 && settings.revision == 0,
            "invalid color mutated the applied revision");

    request = {};
    request.continuous_color_readback = true;
    capabilities.cpu_readback = false;
    Require(!ApplyDeveloperUiRendererSettings(request, capabilities, settings,
                                              feedback),
            "unavailable CPU readback was accepted");
    Require(feedback.status == DeveloperUiSettingsStatus::Rejected,
            "unavailable readback did not report rejection");
    Require(feedback.serial == 2 && settings.revision == 0,
            "rejected readback mutated the applied revision");

    capabilities.cpu_readback = true;
    request.clear_color = {0.1F, 0.2F, 0.3F, 1.0F};
    Require(ApplyDeveloperUiRendererSettings(request, capabilities, settings,
                                             feedback),
            "valid renderer settings were rejected");
    Require(feedback.status == DeveloperUiSettingsStatus::Applied,
            "valid settings did not report application");
    Require(feedback.serial == 3 && settings.revision == 1,
            "valid settings did not advance revisions");
    Require(settings.clear_color == request.clear_color &&
                settings.continuous_color_readback,
            "valid settings were not retained");

    request.aov_inspection_enabled = true;
    request.inspection_aov = merlin::Aov::Normal;
    Require(!ApplyDeveloperUiRendererSettings(request, capabilities, settings,
                                              feedback),
            "unsupported inspection AOV was accepted");
    Require(feedback.serial == 4 && settings.revision == 1,
            "unsupported AOV mutated the applied revision");

    request.inspection_aov = merlin::Aov::PrimId;
    request.continuous_color_readback = false;
    capabilities.cpu_readback = false;
    Require(!ApplyDeveloperUiRendererSettings(request, capabilities, settings,
                                              feedback),
            "AOV inspection without CPU readback was accepted");
    Require(feedback.serial == 5 && settings.revision == 1,
            "unavailable inspection mutated the applied revision");
    Require(feedback.message ==
                "AOV inspection requires backend CPU image readback support.",
            "AOV inspection did not report its readback rejection reason");

    capabilities.cpu_readback = true;
    Require(ApplyDeveloperUiRendererSettings(request, capabilities, settings,
                                             feedback),
            "supported AOV inspection was rejected");
    Require(settings.aov_inspection_enabled &&
                settings.inspection_aov == merlin::Aov::PrimId &&
                settings.revision == 2,
            "AOV inspection settings were not retained");

    std::vector<merlin::render::RenderProductRequest> products{
        {merlin::Aov::Color, false}, {merlin::Aov::PrimId, false}};
    AddDeveloperUiAovInspectionProduct(settings, products);
    Require(products.size() == 2 && products[1].cpu_readback,
            "inspection product was duplicated or did not enable readback");

    const std::vector<float> depth{0.25F, 0.5F, 0.75F, 1.0F};
    const auto depth_preview =
        BuildDeveloperUiDepthPreview(2, 2, 42, depth);
    Require(depth_preview.available &&
                depth_preview.aov == merlin::Aov::Depth &&
                depth_preview.frame_index == 42 &&
                depth_preview.pixels.size() == 4 &&
                depth_preview.minimum[0] == 0.25 &&
                depth_preview.maximum[0] == 1.0,
            "depth preview metadata or range is invalid");
    Require(depth_preview.pixels.front().display_rgba[0] == 0 &&
                depth_preview.pixels.back().display_rgba[0] == 255,
            "depth preview normalization is invalid");

    const auto invalid_id = std::numeric_limits<std::uint32_t>::max();
    const std::vector<std::uint32_t> ids{invalid_id, 7, 9, 7};
    const auto id_preview = BuildDeveloperUiIdPreview(
        merlin::Aov::PrimId, 2, 2, 43, ids);
    Require(id_preview.invalid_value_count == 1 &&
                id_preview.minimum[0] == 7.0 &&
                id_preview.maximum[0] == 9.0 &&
                id_preview.pixels[1].id == 7,
            "ID preview values are invalid");

    std::vector<std::uint32_t> edge_ids(128U * 128U);
    edge_ids.back() = 17U;
    const auto edge_preview = BuildDeveloperUiIdPreview(
        merlin::Aov::InstanceId, 128, 128, 44, edge_ids);
    Require(edge_preview.preview_width == 64 &&
                edge_preview.preview_height == 64 &&
                edge_preview.pixels.front().source_x == 0 &&
                edge_preview.pixels.front().source_y == 0 &&
                edge_preview.pixels.back().source_x == 127 &&
                edge_preview.pixels.back().source_y == 127 &&
                edge_preview.pixels.back().id == 17,
            "downsampled preview does not include source image edges");

    const std::vector<std::uint8_t> rgba{
        1, 2, 3, 255, 10, 20, 30, 128,
        4, 5, 6, 255, 40, 50, 60, 64};
    const auto color_preview =
        BuildDeveloperUiColorPreview(2, 2, 45, rgba);
    Require(color_preview.minimum[0] == 1.0 &&
                color_preview.maximum[2] == 60.0 &&
                color_preview.pixels.back().color[3] == 64,
            "color preview values are invalid");

    DeveloperUiDiagnosticHistory history(2);
    const merlin::Diagnostic backend_warning{
        merlin::kDiagnosticSchemaVersion, "vulkan.validation",
        merlin::DiagnosticSeverity::Warning,
        merlin::DiagnosticDisposition::Rejected, "VUID-test",
        "validation warning", "fix-vulkan-diagnostic"};
    history.SetFrameIndex(7);
    history.Report(backend_warning);
    history.Report(backend_warning);
    auto diagnostic_snapshot = history.Snapshot();
    Require(diagnostic_snapshot.size() == 1 &&
                diagnostic_snapshot.front().origin ==
                    DeveloperUiDiagnosticOrigin::Backend &&
                diagnostic_snapshot.front().first_frame == 7 &&
                diagnostic_snapshot.front().last_frame == 7 &&
                diagnostic_snapshot.front().occurrences == 2,
            "consecutive backend diagnostics were not aggregated");

    const merlin::Diagnostic host_warning{
        merlin::kDiagnosticSchemaVersion, "viewport.settings.rejected",
        merlin::DiagnosticSeverity::Warning,
        merlin::DiagnosticDisposition::Rejected, "merlin-viewport",
        "settings rejected", "retain-previous-settings"};
    history.Record(DeveloperUiDiagnosticOrigin::Host, 8, host_warning);
    history.Record(DeveloperUiDiagnosticOrigin::Host, 9, host_warning);
    diagnostic_snapshot = history.Snapshot();
    Require(diagnostic_snapshot.size() == 2 &&
                diagnostic_snapshot.back().first_frame == 8 &&
                diagnostic_snapshot.back().last_frame == 9 &&
                diagnostic_snapshot.back().occurrences == 2,
            "consecutive host diagnostics were not aggregated across frames");

    history.Record(
        DeveloperUiDiagnosticOrigin::Host, 12,
        {merlin::kDiagnosticSchemaVersion, "viewport.stage.opened",
         merlin::DiagnosticSeverity::Info,
         merlin::DiagnosticDisposition::Ignored, "fixture.usda",
         "stage opened", "stage-active"});
    diagnostic_snapshot = history.Snapshot();
    Require(diagnostic_snapshot.size() == 2 &&
                diagnostic_snapshot.front().diagnostic.code ==
                    "viewport.settings.rejected" &&
                diagnostic_snapshot.back().sequence == 3,
            "bounded diagnostic history did not evict the oldest entry");
    history.Clear();
    Require(history.Snapshot().empty(),
            "diagnostic history clear retained entries");

    merlin::render::BackendSelection selection;
    selection.reason = "test backend selected";
    capabilities.backend_name = "test";
    capabilities.bindless_textures = true;
    capabilities.generated_materials = true;
    capabilities.timestamp_queries = true;
    RecordDeveloperUiBackendSelection(history, 13, selection, capabilities);
    diagnostic_snapshot = history.Snapshot();
    Require(diagnostic_snapshot.size() == 1 &&
                diagnostic_snapshot.front().diagnostic.code ==
                    "viewport.backend.selected",
            "backend selection was not recorded in diagnostic history");

    feedback.status = DeveloperUiSettingsStatus::Rejected;
    feedback.message = "rejected for test";
    RecordDeveloperUiSettingsFeedback(history, 14, feedback);
    diagnostic_snapshot = history.Snapshot();
    Require(diagnostic_snapshot.back().origin ==
                DeveloperUiDiagnosticOrigin::Host &&
                diagnostic_snapshot.back().diagnostic.recovery ==
                    "retain-previous-settings",
            "settings rejection was not recorded as a host diagnostic");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
