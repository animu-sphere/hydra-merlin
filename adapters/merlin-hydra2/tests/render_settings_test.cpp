#include "adapter.hpp"

#include <pxr/pxr.h>

#include <pxr/base/vt/value.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <limits>
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
  const auto exposure_descriptor = std::find_if(descriptors.begin(), descriptors.end(),
      [](const HdRenderSettingDescriptor& descriptor) {
        return descriptor.key == HdMerlinRenderSettingsTokens->exposureEv;
      });
  if (!Check(exposure_descriptor != descriptors.end() &&
                 exposure_descriptor->defaultValue.IsHolding<float>() &&
                 exposure_descriptor->defaultValue.UncheckedGet<float>() == 0.0F,
          "exposure descriptor must use the host-supported float UI type"))
    return 1;
  (void)NoDiagnostics(pending);
  if (!Check(descriptors.size() == 5 && described_flag(enabled) &&
                 described_flag(tiled),
          "Gaussian render setting descriptors are not the two flags") ||
      !Check(Policy(pending, "disabled", "sorted-stream") &&
                 !Flag(pending, enabled) && !Flag(pending, tiled),
          "default Gaussian render settings are incorrect")) {
    return 1;
  }
  const auto& exposure = HdMerlinRenderSettingsTokens->exposureEv;
  const auto& tone = HdMerlinRenderSettingsTokens->toneMapping;
  if (!Check(pending.GetRenderSetting(exposure) == VtValue(0.0) &&
                 Setting(pending, tone) == "reinhard",
          "incorrect Forward output defaults"))
    return 1;
  pending.SetRenderSetting(exposure, VtValue(1.0));
  pending.SetRenderSetting(tone, VtValue(TfToken("none")));
  const auto output_version = pending.GetRenderSettingsVersion();
  pending.SetRenderSetting(exposure, VtValue(1.0F));
  pending.SetRenderSetting(tone, VtValue(std::string("none")));
  if (!Check(pending.GetRenderSettingsVersion() == output_version,
          "unchanged output settings advanced revision"))
    return 1;
  for (const auto& invalid : {VtValue(33.0), VtValue(-33.0),
           VtValue(std::numeric_limits<double>::quiet_NaN()), VtValue(true)}) {
    pending.SetRenderSetting(exposure, invalid);
    if (!Check(pending.GetRenderSetting(exposure) == VtValue(1.0) &&
                   pending.GetRenderSettingsVersion() == output_version,
            "invalid exposure changed state") ||
        !Check(Rejected(pending, "renderer-settings.invalid-exposure", exposure,
                   "keep 1.000000 EV/none"),
            "invalid exposure lost diagnostic"))
      return 1;
  }
  pending.SetRenderSetting(tone, VtValue(std::string("aces")));
  if (!Check(Setting(pending, tone) == "none" &&
                 pending.GetRenderSettingsVersion() == output_version,
          "ACES changed applied output settings") ||
      !Check(Rejected(pending, "renderer-settings.tone-mapping-unsupported", tone,
                 "keep 1.000000 EV/none"),
          "ACES lost rejection diagnostic"))
    return 1;
  pending.SetRenderSetting(tone, VtValue(std::string("invalid")));
  if (!Check(Rejected(pending, "renderer-settings.invalid-tone-mapping", tone,
                 "keep 1.000000 EV/none"),
          "unknown tone mapping lost diagnostic"))
    return 1;
  pending.SetOutputTransformSettings({});
  (void)NoDiagnostics(pending);

  // The Forward surface debug view shares the settings vocabulary: surface
  // views apply, representable views outside the surface context and unknown
  // names reject and keep the effective view.
  const auto& debug_view = HdMerlinRenderSettingsTokens->debugView;
  if (!Check(Setting(pending, debug_view) == "none",
          "incorrect debug view default"))
    return 1;
  for (const auto* name :
      {"geometric-normal", "shading-normal-base", "normal", "tangent",
          "bitangent", "front-facing"}) {
    pending.SetRenderSetting(debug_view, VtValue(TfToken(name)));
    if (!Check(Setting(pending, debug_view) == name,
            "surface debug view was not applied"))
      return 1;
  }
  const auto debug_version = pending.GetRenderSettingsVersion();
  pending.SetRenderSetting(debug_view, VtValue(std::string("front-facing")));
  if (!Check(pending.GetRenderSettingsVersion() == debug_version,
          "unchanged debug view advanced revision"))
    return 1;
  pending.SetRenderSetting(debug_view, VtValue(std::string("depth")));
  if (!Check(Setting(pending, debug_view) == "front-facing" &&
                 pending.GetRenderSettingsVersion() == debug_version,
          "unsupported debug view changed state") ||
      !Check(Rejected(pending, "renderer-settings.debug-view-unsupported",
                 debug_view, "keep front-facing"),
          "unsupported debug view lost diagnostic"))
    return 1;
  pending.SetRenderSetting(debug_view, VtValue(std::string("albedo")));
  if (!Check(Rejected(pending, "renderer-settings.invalid-debug-view",
                 debug_view, "keep front-facing"),
          "unknown debug view lost diagnostic"))
    return 1;
  pending.SetDebugView(merlin::render::DebugView::None);
  (void)NoDiagnostics(pending);
  HdRenderSettingsMap debug_map;
  debug_map[debug_view] = VtValue(std::string("geometric-normal"));
  HdMerlinRenderDelegate initial_debug(debug_map);
  if (!Check(Setting(initial_debug, debug_view) == "geometric-normal",
          "initial debug view was ignored"))
    return 1;

  HdRenderSettingsMap output_map;
  output_map[exposure] = VtValue(-2.0);
  output_map[tone] = VtValue(std::string("none"));
  HdMerlinRenderDelegate initial_output(output_map);
  if (!Check(initial_output.GetRenderSetting(exposure) == VtValue(-2.0) &&
                 Setting(initial_output, tone) == "none",
          "initial output settings were ignored"))
    return 1;
  output_map[exposure] = VtValue(100.0);
  output_map[tone] = VtValue(std::string("aces"));
  HdMerlinRenderDelegate invalid_output(output_map);
  if (!Check(invalid_output.GetRenderSetting(exposure) == VtValue(0.0) &&
                 Setting(invalid_output, tone) == "reinhard",
          "invalid initial output did not retain defaults"))
    return 1;
  (void)NoDiagnostics(pending);

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
