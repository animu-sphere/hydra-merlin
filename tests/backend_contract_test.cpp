#include <merlin/render/backend.hpp>

#include <cassert>
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
