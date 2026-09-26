#include "adapter.hpp"

#include <pxr/pxr.h>

#include <pxr/base/vt/value.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

bool Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

// Reports capabilities only: settings validation never submits a frame.
class CapabilityBackend final : public merlin::render::Backend {
public:
  explicit CapabilityBackend(bool gpu_driven_gaussian) {
    capabilities_.backend_name = "capability-test";
    capabilities_.gpu_driven_gaussian = gpu_driven_gaussian;
  }

  const merlin::render::RendererCapabilities& capabilities()
      const noexcept override {
    return capabilities_;
  }
  merlin::render::RendererStatistics statistics() const noexcept override {
    return {};
  }
  std::optional<merlin::render::PresentationTarget>
  default_presentation_target() const noexcept override {
    return std::nullopt;
  }
  void ResizePresentationTarget(
      merlin::render::PresentationTarget, std::uint32_t,
      std::uint32_t) override {
    Unused("resize presentation");
  }
  merlin::render::CompletionToken Submit(
      const merlin::render::RenderRequest&) override {
    Unused("submit");
  }
  bool IsComplete(merlin::render::CompletionToken) const override {
    Unused("query completion");
  }
  merlin::render::RenderResult Resolve(
      merlin::render::CompletionToken, std::chrono::nanoseconds) override {
    Unused("resolve");
  }

private:
  [[noreturn]] static void Unused(const char* operation) {
    throw merlin::render::RendererError(
        merlin::render::RendererErrorCode::InvalidRequest, operation,
        "the capability test backend does not render");
  }

  merlin::render::RendererCapabilities capabilities_;
};

std::string Setting(const HdRenderDelegate& delegate, const TfToken& key) {
  const auto value = delegate.GetRenderSetting(key);
  return value.IsHolding<std::string>() ? value.UncheckedGet<std::string>()
                                        : std::string("<not a string>");
}

bool Flag(const HdRenderDelegate& delegate, const TfToken& key) {
  const auto value = delegate.GetRenderSetting(key);
  return value.IsHolding<bool>() && value.UncheckedGet<bool>();
}

bool Policy(const HdRenderDelegate& delegate, std::string_view mode,
    std::string_view raster) {
  return Setting(delegate,
             HdMerlinRenderSettingsTokens->gpuDrivenGaussianMode) == mode &&
         Setting(delegate,
             HdMerlinRenderSettingsTokens->gpuDrivenGaussianRaster) ==
             raster;
}

// Consumes the diagnostics reported since the previous call.
bool Rejected(HdMerlinRenderDelegate& delegate, std::string_view code,
    const TfToken& source, std::string_view recovery) {
  const auto diagnostics = delegate.GetLatestViewportFrame().diagnostics;
  return std::any_of(diagnostics.begin(), diagnostics.end(),
      [&](const merlin::Diagnostic& diagnostic) {
        return diagnostic.code == code &&
               diagnostic.source == source.GetString() &&
               diagnostic.disposition ==
                   merlin::DiagnosticDisposition::Rejected &&
               diagnostic.recovery == recovery;
      });
}

bool NoDiagnostics(HdMerlinRenderDelegate& delegate) {
  return delegate.GetLatestViewportFrame().diagnostics.empty();
}

} // namespace

int main() {
  const auto& mode = HdMerlinRenderSettingsTokens->gpuDrivenGaussianMode;
  const auto& raster = HdMerlinRenderSettingsTokens->gpuDrivenGaussianRaster;
  const auto& enabled =
      HdMerlinRenderSettingsTokens->gpuDrivenGaussianEnabled;
  const auto& tiled = HdMerlinRenderSettingsTokens->gpuDrivenGaussianTiled;

  // Without a backend, vocabulary errors reject immediately and a valid
  // request stays pending until the first frame validates it.
  HdMerlinRenderDelegate pending;
  // Hosts list only the flags, so a settings dialog never re-sends a stale
  // policy name.
  const auto descriptors = pending.GetRenderSettingDescriptors();
  const auto described_flag = [&](const TfToken& key) {
    return std::any_of(descriptors.begin(), descriptors.end(),
        [&](const HdRenderSettingDescriptor& descriptor) {
          return descriptor.key == key &&
                 descriptor.defaultValue.IsHolding<bool>() &&
                 !descriptor.defaultValue.UncheckedGet<bool>();
        });
  };
  (void)NoDiagnostics(pending);
  if (!Check(descriptors.size() == 2 && described_flag(enabled) &&
                 described_flag(tiled),
          "Gaussian render setting descriptors are not the two flags") ||
      !Check(Policy(pending, "disabled", "sorted-stream") &&
                 !Flag(pending, enabled) && !Flag(pending, tiled),
          "default Gaussian render settings are incorrect")) {
    return 1;
  }
  const auto initial_version = pending.GetRenderSettingsVersion();
  pending.SetRenderSetting(mode, VtValue(std::string("fast")));
  pending.SetRenderSetting(raster, VtValue(4));
  if (!Check(Policy(pending, "disabled", "sorted-stream") &&
                 pending.GetRenderSettingsVersion() == initial_version,
          "an invalid Gaussian render setting was stored") ||
      !Check(Rejected(pending,
                 "renderer-settings.invalid-gaussian-raster-path", raster,
                 "keep disabled/sorted-stream"),
          "an invalid Gaussian raster path was not rejected")) {
    return 1;
  }
  pending.SetRenderSetting(mode, VtValue(std::string("require")));
  pending.SetRenderSetting(raster, VtValue(TfToken("tiled")));
  const auto requested_version = pending.GetRenderSettingsVersion();
  pending.SetRenderSetting(mode, VtValue(TfToken("require")));
  pending.SetRenderSetting(raster, VtValue(std::string("tiled")));
  if (!Check(Policy(pending, "require", "tiled"),
          "a pending Gaussian request is not reported") ||
      !Check(requested_version != initial_version &&
                 pending.GetRenderSettingsVersion() == requested_version,
          "an unchanged Gaussian request advanced the settings version") ||
      !Check(NoDiagnostics(pending),
          "a pending Gaussian request reported a diagnostic")) {
    return 1;
  }

  // The flags report and edit the same policy. Turning GPU execution on
  // keeps require, off disables it, and on again selects prefer.
  pending.SetRenderSetting(enabled, VtValue(true));
  if (!Check(Flag(pending, enabled) && Flag(pending, tiled) &&
                 Policy(pending, "require", "tiled") &&
                 pending.GetRenderSettingsVersion() == requested_version,
          "an unchanged GPU Gaussian flag changed the policy")) {
    return 1;
  }
  pending.SetRenderSetting(enabled, VtValue(false));
  pending.SetRenderSetting(tiled, VtValue(false));
  if (!Check(Policy(pending, "disabled", "sorted-stream") &&
                 !Flag(pending, enabled) && !Flag(pending, tiled),
          "clearing the Gaussian flags did not select the CPU reference")) {
    return 1;
  }
  pending.SetRenderSetting(enabled, VtValue(true));
  pending.SetRenderSetting(tiled, VtValue(std::string("true")));
  if (!Check(Policy(pending, "prefer", "sorted-stream"),
          "setting the GPU Gaussian flag did not select prefer") ||
      !Check(Rejected(pending,
                 "renderer-settings.invalid-gaussian-raster-path", tiled,
                 "keep prefer/sorted-stream"),
          "a non-bool Gaussian flag was not rejected")) {
    return 1;
  }

  // Host-provided settings are read at creation; an invalid one falls back
  // to its default explicitly, and a flag refines a policy name.
  HdRenderSettingsMap initial_settings;
  initial_settings[mode] = VtValue(std::string("prefer"));
  initial_settings[raster] = VtValue(TfToken("tiled"));
  HdMerlinRenderDelegate initial(initial_settings);
  HdRenderSettingsMap flag_settings;
  flag_settings[mode] = VtValue(std::string("require"));
  flag_settings[enabled] = VtValue(true);
  flag_settings[tiled] = VtValue(true);
  HdMerlinRenderDelegate flag_initial(flag_settings);
  HdRenderSettingsMap invalid_settings;
  invalid_settings[mode] = VtValue(std::string("always"));
  HdMerlinRenderDelegate invalid_initial(invalid_settings);
  if (!Check(Policy(initial, "prefer", "tiled") &&
                 Policy(flag_initial, "require", "tiled"),
          "host-provided Gaussian render settings were ignored") ||
      !Check(Policy(invalid_initial, "disabled", "sorted-stream"),
          "an invalid host-provided Gaussian mode was kept") ||
      !Check(Rejected(invalid_initial,
                 "renderer-settings.invalid-gpu-driven-gaussian-mode", mode,
                 "keep disabled/sorted-stream"),
          "an invalid host-provided Gaussian mode was not rejected")) {
    return 1;
  }

  // With a backend, capability validation is immediate and a rejection keeps
  // the applied policy.
  HdMerlinRenderDelegate unsupported(
      std::make_shared<CapabilityBackend>(false));
  (void)NoDiagnostics(unsupported);
  unsupported.SetRenderSetting(mode, VtValue(std::string("require")));
  if (!Check(Policy(unsupported, "disabled", "sorted-stream"),
          "an unsupported required Gaussian policy was applied") ||
      !Check(Rejected(unsupported,
                 "renderer-settings.gpu-driven-gaussian-unsupported", mode,
                 "keep disabled/sorted-stream"),
          "an unsupported required Gaussian policy was not rejected")) {
    return 1;
  }
  unsupported.SetRenderSetting(mode, VtValue(std::string("prefer")));
  unsupported.SetRenderSetting(raster, VtValue(std::string("tiled")));
  unsupported.SetRenderSetting(mode, VtValue(std::string("require")));
  if (!Check(Policy(unsupported, "prefer", "tiled"),
          "a preferred Gaussian policy was not kept after a rejection") ||
      !Check(Rejected(unsupported,
                 "renderer-settings.gpu-driven-gaussian-unsupported", mode,
                 "keep prefer/tiled"),
          "a rejection did not name the kept Gaussian policy")) {
    return 1;
  }

  HdMerlinRenderDelegate supported(std::make_shared<CapabilityBackend>(true));
  (void)NoDiagnostics(supported);
  supported.SetGpuDrivenGaussianSettings(
      {merlin::render::GpuDrivenGaussianMode::Require,
          merlin::render::GaussianRasterPath::Tiled});
  if (!Check(Policy(supported, "require", "tiled") &&
                 NoDiagnostics(supported),
          "a supported required Gaussian policy was not applied")) {
    return 1;
  }
  return 0;
}
