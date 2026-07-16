#include <merlin/render/backend.hpp>

#include <algorithm>
#include <utility>

namespace merlin::render {

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
