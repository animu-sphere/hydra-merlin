#include "adapter.hpp"
#include "distant_light.hpp"

#include <pxr/base/gf/rotation.h>
#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/tokens.h>

#include <merlin/render/forward_lighting.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <numbers>
#include <stdexcept>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

void Check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class CaptureBackend final : public merlin::render::Backend {
public:
  const merlin::render::RendererCapabilities& capabilities() const noexcept override {
    return capabilities_;
  }
  merlin::render::RendererStatistics statistics() const noexcept override {
    return {};
  }
  std::optional<merlin::render::PresentationTarget>
  default_presentation_target() const noexcept override {
    return merlin::render::PresentationTarget(99, 1);
  }
  void ResizePresentationTarget(merlin::render::PresentationTarget,
      std::uint32_t, std::uint32_t) override {
  }
  merlin::render::CompletionToken Submit(const merlin::render::RenderRequest& request) override {
    snapshot = request.snapshot;
    return {99, 1};
  }
  bool IsComplete(merlin::render::CompletionToken) const override {
    return true;
  }
  merlin::render::RenderResult Resolve(merlin::render::CompletionToken,
      std::chrono::nanoseconds) override {
    return {};
  }
  std::shared_ptr<const merlin::extraction::FrameSnapshot> snapshot;

private:
  merlin::render::RendererCapabilities capabilities_;
};

class LightSource final : public HdSceneDelegate {
public:
  explicit LightSource(HdRenderIndex* index) : HdSceneDelegate(index, SdfPath("/source")) {
  }
  GfMatrix4d GetTransform(const SdfPath&) override {
    return transform;
  }
  VtValue GetLightParamValue(const SdfPath&, const TfToken& name) override {
    const auto found = values.find(name);
    return found == values.end() ? VtValue{} : found->second;
  }
  GfMatrix4d transform{1.0};
  std::map<TfToken, VtValue> values;
};

void VerifyEnergy() {
  using namespace merlin::hydra::detail;
  DistantLightEnergy input;
  input.intensity = 15000.0F;
  const auto camera = NormalizeDistantLightEnergy(input);
  Check(camera && *camera > 0.32F && *camera < 0.33F,
      "Hdx camera radiance became an unbounded Lambert multiplier");

  // Independently integrate radiance*cos(theta) over the light's spherical
  // cap with midpoint quadrature, then divide by pi for white Lambert albedo.
  for (const auto angle : {0.53F, 10.0F, 60.0F, 180.0F}) {
    input.angle_degrees = angle;
    const double radius = angle * std::numbers::pi / 360.0;
    constexpr int samples = 10000;
    double integral = 0.0;
    for (int sample = 0; sample < samples; ++sample) {
      const double theta = (sample + 0.5) * radius / samples;
      integral += std::cos(theta) * std::sin(theta);
    }
    const auto expected = input.intensity * 2.0 * integral * radius / samples;
    const auto converted = NormalizeDistantLightEnergy(input);
    Check(converted && std::abs(*converted - expected) < expected * 1e-6,
        "distant light response disagrees with hemispherical integration");
  }
  input.intensity = 1.0F;
  input.normalize = true;
  for (const auto angle : {0.0F, 0.53F, 60.0F, 180.0F}) {
    input.angle_degrees = angle;
    const auto converted = NormalizeDistantLightEnergy(input);
    Check(converted && std::abs(*converted - 1.0 / std::numbers::pi) < 1e-6,
        "normalize did not preserve face-on illuminance");
  }
  input.exposure = 1.0F;
  input.diffuse = 0.5F;
  Check(std::abs(*NormalizeDistantLightEnergy(input) - 1.0 / std::numbers::pi) < 1e-6,
      "exposure and diffuse did not multiply the response");
  input.intensity = 0.0F;
  input.exposure = std::numeric_limits<float>::max();
  Check(NormalizeDistantLightEnergy(input) == 0.0F, "zero energy overflowed");
  input.intensity = 1.0F;
  Check(!NormalizeDistantLightEnergy(input), "unrepresentable response accepted");
  input.exposure = -std::numeric_limits<float>::max();
  Check(NormalizeDistantLightEnergy(input) == 0.0F, "underflow did not yield zero");
  input = {};
  input.angle_degrees = 0.0F;
  Check(std::abs(*NormalizeDistantLightEnergy(input) - 1.0 / std::numbers::pi) < 1e-6,
      "parallel delta light lost the size-factor-1 convention");
  input.angle_degrees = 181.0F;
  Check(!NormalizeDistantLightEnergy(input), "unsupported angular domain accepted");
  for (int field = 0; field < 4; ++field) {
    for (const auto invalid : {std::numeric_limits<float>::infinity(),
             std::numeric_limits<float>::quiet_NaN()}) {
      input = {};
      auto* parameter = field == 0 ? &input.intensity : field == 1 ? &input.exposure
                                                    : field == 2   ? &input.diffuse
                                                                   : &input.angle_degrees;
      *parameter = invalid;
      Check(!NormalizeDistantLightEnergy(input), "non-finite light input accepted");
    }
  }
}

void VerifyHydraTransport() {
  auto backend = std::make_shared<CaptureBackend>();
  HdMerlinRenderDelegate delegate(backend);
  std::unique_ptr<HdRenderIndex> index(HdRenderIndex::New(&delegate, {}));
  Check(index != nullptr, "could not create render index");
  LightSource source(index.get());
  const SdfPath path("/source/cameraLight");
  const auto destroy_light = [&](HdSprim* sprim) { delegate.DestroySprim(sprim); };
  std::unique_ptr<HdSprim, decltype(destroy_light)>
      light(delegate.CreateSprim(HdPrimTypeTokens->distantLight, path), destroy_light);
  auto state = std::make_shared<HdRenderPassState>();
  state->SetViewport(GfVec4d(0.0, 0.0, 16.0, 16.0));
  auto pass = delegate.CreateRenderPass(index.get(), HdRprimCollection());
  const auto render = [&] {
    pass->Execute(state, {});
    Check(backend->snapshot != nullptr, "no adapter snapshot submitted");
    return merlin::render::ExtractForwardDirectionalLighting(*backend->snapshot);
  };
  const auto sync = [&](HdDirtyBits bits) {
    light->Sync(&source, nullptr, &bits);
    Check(bits == HdLight::Clean, "light dirty bits were not consumed");
  };
  source.values[HdLightTokens->intensity] = VtValue(15000.0F);
  source.values[HdLightTokens->angle] = VtValue(0.53F);
  source.values[HdLightTokens->normalize] = VtValue(false);
  source.transform.SetRotate(GfRotation(GfVec3d(0, 1, 0), 30));
  sync(HdLight::AllDirty);
  const auto applied = render();
  Check(std::abs(applied.direction_intensity.x - 0.5F) < 1e-6F,
      "light rotation was not transported in world space");
  Check(applied.direction_intensity.w > 0.32F && applied.direction_intensity.w < 0.33F,
      "Sprim did not transport normalized Hdx energy");
  const auto retained = backend->snapshot;
  sync(HdLight::Clean);
  (void)render();
  Check(backend->snapshot->lights.front().revision == retained->lights.front().revision,
      "clean light created a new light revision");
  source.values[HdLightTokens->exposure] = VtValue(1.0F);
  sync(HdLight::DirtyParams);
  Check(std::abs(render().direction_intensity.w - 2 * applied.direction_intensity.w) < 1e-6F,
      "Sprim did not apply exposure");
  const auto before_rejection = backend->snapshot;
  source.values[HdLightTokens->normalize] = VtValue(std::string("true"));
  source.transform.SetRotate(GfRotation(GfVec3d(0, 1, 0), 90));
  sync(HdLight::AllDirty);
  (void)render();
  Check(backend->snapshot->lights.front().revision == before_rejection->lights.front().revision,
      "invalid parameters partially changed the light");
  const auto diagnostics = delegate.GetLatestViewportFrame().diagnostics;
  Check(std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& item) {
    return item.code == "hydra.light.invalid-parameters" && item.source == path.GetString() &&
           item.recovery == "keep-previous-light";
  }),
      "invalid light lost its diagnostic/recovery");
  source.values[HdLightTokens->normalize] = VtValue(true);
  source.values[HdLightTokens->intensity] = VtValue(0.0F);
  sync(HdLight::AllDirty);
  Check(render().direction_intensity.w == 0.0F, "disabled light gained fallback energy");
  light.reset();
  Check(render().light == 0, "removed host light survived in the snapshot");
  light.reset(delegate.CreateSprim(HdPrimTypeTokens->distantLight, path));
  source.values[HdLightTokens->intensity] = VtValue(-1.0F);
  sync(HdLight::AllDirty);
  Check(render().light == 0, "invalid initial light created a fallback source");
  sync(HdLight::Clean);
  Check(render().light == 0, "clean sync resurrected a rejected source");
  source.values[HdLightTokens->intensity] = VtValue(1.0F);
  sync(HdLight::DirtyTransform);
  Check(render().light != 0, "valid retry did not normalize initial parameters");
}

} // namespace

int main() try {
  VerifyEnergy();
  VerifyHydraTransport();
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
