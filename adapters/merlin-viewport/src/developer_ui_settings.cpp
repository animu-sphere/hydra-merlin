#include "developer_ui.hpp"

#include <cmath>
#include <string>

namespace merlin::viewport {

bool ApplyDeveloperUiRendererSettings(
    const DeveloperUiRendererSettingsRequest& request,
    const render::RendererCapabilities& capabilities,
    DeveloperUiRendererSettings& settings,
    DeveloperUiSettingsFeedback& feedback) {
  ++feedback.serial;
  for (const auto component : request.clear_color) {
    if (!std::isfinite(component) || component < 0.0F || component > 1.0F) {
      feedback.status = DeveloperUiSettingsStatus::Rejected;
      feedback.message =
          "Clear color components must be finite values from 0 to 1.";
      return false;
    }
  }
  if (request.continuous_color_readback && !capabilities.cpu_readback) {
    feedback.status = DeveloperUiSettingsStatus::Rejected;
    feedback.message =
        "The selected backend does not support CPU image readback.";
    return false;
  }
  const bool supported_aov =
      request.inspection_aov == Aov::Color ||
      request.inspection_aov == Aov::Depth ||
      request.inspection_aov == Aov::PrimId ||
      request.inspection_aov == Aov::InstanceId;
  if (request.aov_inspection_enabled && !supported_aov) {
    feedback.status = DeveloperUiSettingsStatus::Rejected;
    feedback.message = "The selected AOV is not available for inspection.";
    return false;
  }
  if (request.aov_inspection_enabled && !capabilities.cpu_readback) {
    feedback.status = DeveloperUiSettingsStatus::Rejected;
    feedback.message =
        "AOV inspection requires backend CPU image readback support.";
    return false;
  }

  settings.available = true;
  settings.clear_color = request.clear_color;
  settings.continuous_color_readback = request.continuous_color_readback;
  settings.aov_inspection_enabled = request.aov_inspection_enabled;
  settings.inspection_aov = request.inspection_aov;
  ++settings.revision;
  feedback.status = DeveloperUiSettingsStatus::Applied;
  feedback.message = "Renderer settings revision " +
                     std::to_string(settings.revision) + " is active.";
  return true;
}

}  // namespace merlin::viewport
