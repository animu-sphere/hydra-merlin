#include <merlin/render/backend.hpp>

#include <cassert>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class FakeBackend final : public merlin::render::Backend {
 public:
  explicit FakeBackend(merlin::render::BackendKind kind,
                       bool presentation = false)
      : owner_(kind == merlin::render::BackendKind::Vulkan ? 41U : 42U) {
    capabilities_.backend = kind;
    capabilities_.backend_name =
        std::string(merlin::render::BackendKindName(kind));
    capabilities_.cpu_readback = true;
    capabilities_.external_presentation = presentation;
    if (presentation) {
      presentation_ = merlin::render::PresentationTarget(owner_, 1);
    }
  }

  const merlin::render::RendererCapabilities& capabilities()
      const noexcept override {
    return capabilities_;
  }

  merlin::render::RendererStatistics statistics() const noexcept override {
    return statistics_;
  }

  std::optional<merlin::render::PresentationTarget>
  default_presentation_target() const noexcept override {
    return presentation_;
  }

  void ResizePresentationTarget(merlin::render::PresentationTarget target,
                                std::uint32_t width,
                                std::uint32_t height) override {
    if (!presentation_ || target != *presentation_ || width == 0 ||
        height == 0) {
      throw merlin::render::RendererError(
          merlin::render::RendererErrorCode::InvalidRequest,
          "resize presentation", "invalid fake target or extent");
    }
    width_ = width;
    height_ = height;
  }

  merlin::render::CompletionToken Submit(
      const merlin::render::RenderRequest& request) override {
    if (!request.snapshot) {
      throw merlin::render::RendererError(
          merlin::render::RendererErrorCode::InvalidRequest, "submit",
          "snapshot is null");
    }
    if (request.presentation &&
        (!presentation_ || request.presentation != *presentation_)) {
      throw merlin::render::RendererError(
          merlin::render::RendererErrorCode::InvalidRequest, "submit",
          "presentation target belongs to another backend");
    }
    ++statistics_.frames_submitted;
    return merlin::render::CompletionToken(owner_, ++completion_);
  }

  bool IsComplete(merlin::render::CompletionToken token) const override {
    Validate(token);
    return true;
  }

  merlin::render::RenderResult Resolve(
      merlin::render::CompletionToken token,
      std::chrono::nanoseconds) override {
    Validate(token);
    if (token.value() <= resolved_) {
      throw merlin::render::RendererError(
          merlin::render::RendererErrorCode::InvalidToken, "resolve",
          "token was already resolved");
    }
    resolved_ = token.value();
    merlin::render::RenderResult result;
    result.completion_value = token.value();
    return result;
  }

 private:
  void Validate(merlin::render::CompletionToken token) const {
    if (token.owner() != owner_ || token.value() == 0 ||
        token.value() > completion_) {
      throw merlin::render::RendererError(
          merlin::render::RendererErrorCode::InvalidToken, "query",
          "token belongs to another backend");
    }
  }

  std::uint64_t owner_{};
  std::uint64_t completion_{};
  std::uint64_t resolved_{};
  std::uint32_t width_{};
  std::uint32_t height_{};
  merlin::render::RendererCapabilities capabilities_;
  merlin::render::RendererStatistics statistics_;
  std::optional<merlin::render::PresentationTarget> presentation_;
};

class FakeFactory final : public merlin::render::BackendFactory {
 public:
  FakeFactory(merlin::render::BackendKind kind, bool available,
              bool presentation = false)
      : kind_(kind), available_(available), presentation_(presentation) {}

  merlin::render::BackendKind kind() const noexcept override { return kind_; }

  merlin::render::BackendAvailability availability() const override {
    return {available_, available_ ? "" : "test-disabled"};
  }

  std::unique_ptr<merlin::render::Backend> Create(
      const merlin::render::BackendCreateInfo&) const override {
    return std::make_unique<FakeBackend>(kind_, presentation_);
  }

 private:
  merlin::render::BackendKind kind_;
  bool available_{};
  bool presentation_{};
};

}  // namespace

int main() {
  using namespace merlin::render;

  FakeFactory vulkan(BackendKind::Vulkan, true, true);
  FakeFactory metal(BackendKind::Metal, false);
  std::vector<BackendFactory*> factories{&metal, &vulkan};
  BackendSelection selection;
  auto backend = CreateBackend({}, factories, &selection);
  assert(selection.requested == BackendRequest::Automatic);
  assert(selection.selected == BackendKind::Vulkan);
  assert(selection.automatic);
#if defined(__APPLE__)
  // Automatic selection prefers Metal on Apple platforms; the unavailable
  // Metal fake forces the Vulkan fallback.
  assert(selection.reason == "available fallback");
#else
  assert(selection.reason == "platform preference");
#endif
  assert(backend->capabilities().contract_version == kBackendContractVersion);
  assert(backend->capabilities().external_presentation);
  assert(backend->default_presentation_target());

  RendererSettings settings;
  settings.presentation_mode = PresentationMode::Native;
  assert(!ValidateRendererSettings(settings, &backend->capabilities()));
  settings.schema_version = kRendererSettingsSchemaVersion + 1;
  auto settings_error =
      ValidateRendererSettings(settings, &backend->capabilities());
  assert(settings_error &&
         settings_error->code == "renderer-settings.unsupported-schema");
  settings = {};
  settings.exposure_ev = std::numeric_limits<float>::infinity();
  settings_error = ValidateRendererSettings(settings);
  assert(settings_error &&
         settings_error->code == "renderer-settings.invalid-exposure");
  settings = {};
  settings.backend = BackendRequest::Metal;
  settings_error =
      ValidateRendererSettings(settings, &backend->capabilities());
  assert(settings_error &&
         settings_error->code == "renderer-settings.backend-mismatch");
  settings = {};
  settings.render_path = RenderPath::ExperimentalVisibility;
  settings_error =
      ValidateRendererSettings(settings, &backend->capabilities());
  assert(settings_error &&
         settings_error->code ==
             "renderer-settings.render-path-unsupported");
  settings = {};
  settings.gpu_driven_indexed.mode = GpuDrivenIndexedMode::Require;
  settings_error =
      ValidateRendererSettings(settings, &backend->capabilities());
  assert(settings_error &&
         settings_error->code ==
             "renderer-settings.gpu-driven-indexed-unsupported");
  settings.gpu_driven_indexed.mode = GpuDrivenIndexedMode::Prefer;
  assert(!ValidateRendererSettings(settings, &backend->capabilities()));
  auto gpu_driven_capabilities = backend->capabilities();
  gpu_driven_capabilities.gpu_driven_indexed = true;
  settings.gpu_driven_indexed.mode = GpuDrivenIndexedMode::Require;
  assert(!ValidateRendererSettings(settings, &gpu_driven_capabilities));
  settings.gpu_driven_indexed.mode =
      static_cast<GpuDrivenIndexedMode>(999);
  settings_error = ValidateRendererSettings(settings);
  assert(settings_error &&
         settings_error->code ==
             "renderer-settings.invalid-gpu-driven-indexed-mode");
  settings = {};
  settings.presentation_mode = PresentationMode::Native;
  auto offscreen_capabilities = backend->capabilities();
  offscreen_capabilities.external_presentation = false;
  settings_error =
      ValidateRendererSettings(settings, &offscreen_capabilities);
  assert(settings_error &&
         settings_error->code ==
             "renderer-settings.presentation-unsupported");
  settings = {};
  settings.validation = true;
  settings_error =
      ValidateRendererSettings(settings, &backend->capabilities());
  assert(settings_error &&
         settings_error->code ==
             "renderer-settings.validation-unavailable");
  const auto require_unsupported = [&](RendererSettings candidate,
                                       std::string_view code) {
    const auto error =
        ValidateRendererSettings(candidate, &backend->capabilities());
    assert(error && error->code == code);
  };
  settings = {};
  settings.aov = merlin::Aov::Normal;
  require_unsupported(settings, "renderer-settings.aov-unsupported");
  settings = {};
  settings.lighting_mode = LightingMode::Environment;
  require_unsupported(settings, "renderer-settings.lighting-unsupported");
  settings = {};
  settings.exposure_ev = 1.0F;
  require_unsupported(settings, "renderer-settings.exposure-unsupported");
  settings = {};
  settings.tone_mapping = ToneMapping::Aces;
  require_unsupported(settings,
                      "renderer-settings.tone-mapping-unsupported");
  settings = {};
  settings.alpha_policy = AlphaPolicy::Blend;
  require_unsupported(settings,
                      "renderer-settings.alpha-policy-unsupported");
  settings = {};
  settings.debug_view = DebugView::Normal;
  require_unsupported(settings,
                      "renderer-settings.debug-view-unsupported");
  settings = {};
  settings.telemetry = TelemetryMode::Detailed;
  require_unsupported(settings,
                      "renderer-settings.telemetry-unsupported");
  settings = {};
  auto validation_capabilities = backend->capabilities();
  validation_capabilities.validation_enabled = true;
  settings_error =
      ValidateRendererSettings(settings, &validation_capabilities);
  assert(settings_error &&
         settings_error->code ==
             "renderer-settings.validation-already-enabled");

  auto snapshot = std::make_shared<merlin::extraction::FrameSnapshot>();
  RenderRequest request;
  request.snapshot = snapshot;
  request.presentation = *backend->default_presentation_target();
  backend->ResizePresentationTarget(request.presentation, 1280, 720);
  const auto token = backend->Submit(request);
  assert(token);
  assert(backend->IsComplete(token));
  assert(backend->Resolve(token).completion_value == token.value());

  bool duplicate_resolve_rejected = false;
  try {
    (void)backend->Resolve(token);
  } catch (const RendererError& error) {
    duplicate_resolve_rejected = error.code() == RendererErrorCode::InvalidToken;
    duplicate_resolve_rejected =
        duplicate_resolve_rejected && error.operation() == "resolve" &&
        error.detail() == "token was already resolved";
  }
  assert(duplicate_resolve_rejected);

  BackendCreateInfo metal_request;
  metal_request.backend = BackendRequest::Metal;
  bool unavailable_rejected = false;
  try {
    (void)CreateBackend(metal_request, factories);
  } catch (const RendererError& error) {
    unavailable_rejected =
        error.code() == RendererErrorCode::BackendUnavailable &&
        std::string(error.what()).find("test-disabled") != std::string::npos;
  }
  assert(unavailable_rejected);

  bool null_factory_rejected = false;
  std::vector<BackendFactory*> invalid{nullptr};
  try {
    (void)CreateBackend({}, invalid);
  } catch (const RendererError& error) {
    null_factory_rejected = error.code() == RendererErrorCode::InvalidRequest;
  }
  assert(null_factory_rejected);
  return 0;
}
