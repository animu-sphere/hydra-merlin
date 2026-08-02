#include "developer_ui.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

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
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
