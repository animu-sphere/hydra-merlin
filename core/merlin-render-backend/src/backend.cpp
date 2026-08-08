#include <merlin/render/backend.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace merlin::render {

std::optional<RendererSettingsValidationError> ValidateRendererSettings(
    const RendererSettings& settings,
    const RendererCapabilities* capabilities) {
  const auto invalid = [](std::string code, std::string message) {
    return std::optional<RendererSettingsValidationError>{
        RendererSettingsValidationError{std::move(code), std::move(message)}};
  };
  if (settings.schema_version != kRendererSettingsSchemaVersion) {
    return invalid("renderer-settings.unsupported-schema",
                   "Renderer settings schema version " +
                       std::to_string(settings.schema_version) +
                       " is unsupported; expected version " +
                       std::to_string(kRendererSettingsSchemaVersion) + ".");
  }
  if (BackendRequestName(settings.backend) == "unknown") {
    return invalid("renderer-settings.invalid-backend",
                   "Renderer backend selection is invalid.");
  }
  if (PresentationModeName(settings.presentation_mode) == "unknown") {
    return invalid("renderer-settings.invalid-presentation",
                   "Renderer presentation mode is invalid.");
  }
  if (RenderPathName(settings.render_path) == "unknown") {
    return invalid("renderer-settings.invalid-render-path",
                   "Renderer path is invalid.");
  }
  if (AovName(settings.aov) == "unknown") {
    return invalid("renderer-settings.invalid-aov",
                   "Renderer AOV selection is invalid.");
  }
  if (LightingModeName(settings.lighting_mode) == "unknown") {
    return invalid("renderer-settings.invalid-lighting",
                   "Renderer lighting mode is invalid.");
  }
  if (!std::isfinite(settings.exposure_ev) || settings.exposure_ev < -32.0F ||
      settings.exposure_ev > 32.0F) {
    return invalid("renderer-settings.invalid-exposure",
                   "Exposure must be finite and between -32 and 32 EV.");
  }
  if (ToneMappingName(settings.tone_mapping) == "unknown") {
    return invalid("renderer-settings.invalid-tone-mapping",
                   "Renderer tone-mapping mode is invalid.");
  }
  if (AlphaPolicyName(settings.alpha_policy) == "unknown") {
    return invalid("renderer-settings.invalid-alpha-policy",
                   "Renderer alpha policy is invalid.");
  }
  if (DebugViewName(settings.debug_view) == "unknown") {
    return invalid("renderer-settings.invalid-debug-view",
                   "Renderer debug view is invalid.");
  }
  if (TelemetryModeName(settings.telemetry) == "unknown") {
    return invalid("renderer-settings.invalid-telemetry",
                   "Renderer telemetry mode is invalid.");
  }
  if (capabilities == nullptr) {
    return std::nullopt;
  }

  std::optional<BackendKind> requested_backend;
  if (settings.backend == BackendRequest::Vulkan) {
    requested_backend = BackendKind::Vulkan;
  } else if (settings.backend == BackendRequest::Metal) {
    requested_backend = BackendKind::Metal;
  }
  if (requested_backend && *requested_backend != capabilities->backend) {
    return invalid("renderer-settings.backend-mismatch",
                   "Renderer settings request " +
                       std::string(BackendRequestName(settings.backend)) +
                       " but the selected backend is " +
                       std::string(BackendKindName(capabilities->backend)) +
                       ".");
  }
  if ((settings.presentation_mode == PresentationMode::Native ||
       settings.presentation_mode == PresentationMode::Host) &&
      !capabilities->external_presentation) {
    return invalid("renderer-settings.presentation-unsupported",
                   "The selected backend does not support external "
                   "presentation.");
  }
  if (settings.render_path == RenderPath::ExperimentalVisibility) {
    return invalid("renderer-settings.render-path-unsupported",
                   "The selected backend does not support the experimental "
                   "Visibility render path.");
  }
  if (settings.validation && !capabilities->validation_enabled) {
    return invalid("renderer-settings.validation-unavailable",
                   "Validation was requested but is not enabled by the "
                   "selected backend.");
  }
  return std::nullopt;
}

std::string_view RendererErrorCodeName(RendererErrorCode code) noexcept {
  switch (code) {
    case RendererErrorCode::InvalidRequest: return "invalid-request";
    case RendererErrorCode::InvalidToken: return "invalid-token";
    case RendererErrorCode::ResourceBusy: return "resource-busy";
    case RendererErrorCode::Timeout: return "timeout";
    case RendererErrorCode::DeviceLost: return "device-lost";
    case RendererErrorCode::Unsupported: return "unsupported";
    case RendererErrorCode::BackendUnavailable: return "backend-unavailable";
    case RendererErrorCode::BackendFailure: return "backend-failure";
    case RendererErrorCode::ResourceExhausted: return "resource-exhausted";
  }
  return "unknown";
}

RendererError::RendererError(RendererErrorCode code, std::string operation,
                             std::string detail, std::int32_t native_code)
    : std::runtime_error(std::string(RendererErrorCodeName(code)) + ": " +
                         operation + ": " + detail),
      code_(code),
      operation_(std::move(operation)),
      detail_(std::move(detail)),
      native_code_(native_code) {}

namespace {

std::optional<BackendKind> RequestedKind(BackendRequest request) {
  switch (request) {
    case BackendRequest::Automatic: return std::nullopt;
    case BackendRequest::Vulkan: return BackendKind::Vulkan;
    case BackendRequest::Metal: return BackendKind::Metal;
  }
  return std::nullopt;
}

BackendKind PlatformPreference() noexcept {
#if defined(__APPLE__)
  return BackendKind::Metal;
#else
  return BackendKind::Vulkan;
#endif
}

}  // namespace

std::unique_ptr<Backend> CreateBackend(
    const BackendCreateInfo& info, std::span<BackendFactory* const> factories,
    BackendSelection* selection) {
  if (factories.empty()) {
    throw RendererError(RendererErrorCode::BackendUnavailable,
                        "select renderer backend",
                        "the application supplied no backend factories");
  }
  if (std::any_of(factories.begin(), factories.end(),
                  [](const BackendFactory* factory) {
                    return factory == nullptr;
                  })) {
    throw RendererError(RendererErrorCode::InvalidRequest,
                        "select renderer backend",
                        "backend factory list contains null");
  }

  const auto requested = RequestedKind(info.backend);
  const auto preferred = requested.value_or(PlatformPreference());
  std::vector<BackendFactory*> ordered(factories.begin(), factories.end());
  std::stable_sort(ordered.begin(), ordered.end(),
                   [preferred](const BackendFactory* lhs,
                               const BackendFactory* rhs) {
                     return lhs->kind() == preferred && rhs->kind() != preferred;
                   });

  std::string unavailable;
  for (const auto* factory : ordered) {
    if (requested && factory->kind() != *requested) {
      continue;
    }
    const auto probe = factory->availability();
    if (!probe.available) {
      if (!unavailable.empty()) {
        unavailable += "; ";
      }
      unavailable += std::string(BackendKindName(factory->kind())) + ": " +
                     (probe.detail.empty() ? "unavailable" : probe.detail);
      continue;
    }
    auto backend = factory->Create(info);
    if (!backend) {
      throw RendererError(RendererErrorCode::BackendFailure,
                          "create renderer backend",
                          std::string(BackendKindName(factory->kind())) +
                              " factory returned null");
    }
    if (selection != nullptr) {
      selection->requested = info.backend;
      selection->selected = factory->kind();
      selection->automatic = !requested.has_value();
      selection->reason = requested ? "explicit request"
                                    : factory->kind() == preferred
                                          ? "platform preference"
                                          : "available fallback";
    }
    return backend;
  }

  const auto detail = requested
                          ? std::string(BackendKindName(*requested)) +
                                " was requested but is unavailable" +
                                (unavailable.empty() ? "" : ": " + unavailable)
                          : "no supplied backend is available" +
                                (unavailable.empty() ? "" : ": " + unavailable);
  throw RendererError(RendererErrorCode::BackendUnavailable,
                      "select renderer backend", detail);
}

}  // namespace merlin::render
