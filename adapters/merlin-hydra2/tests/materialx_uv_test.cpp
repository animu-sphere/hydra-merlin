#include "adapter.hpp"

#include <pxr/base/gf/vec2f.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/assetPath.h>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

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

class MaterialSource final : public HdSceneDelegate {
public:
  explicit MaterialSource(HdRenderIndex* index)
      : HdSceneDelegate(index, SdfPath("/source")) {
  }
  VtValue GetMaterialResource(const SdfPath&) override {
    return VtValue(network);
  }

  HdMaterialNetwork2 network;
};

void VerifyImageUv(const char* texture_path) {
  auto backend = std::make_shared<CaptureBackend>();
  HdMerlinRenderDelegate delegate(backend);
  std::unique_ptr<HdRenderIndex> index(HdRenderIndex::New(&delegate, {}));
  Check(index != nullptr, "could not create render index");
  MaterialSource source(index.get());
  const SdfPath path("/source/material");
  const auto destroy_material = [&](HdSprim* sprim) { delegate.DestroySprim(sprim); };
  std::unique_ptr<HdSprim, decltype(destroy_material)> material(
      delegate.CreateSprim(HdPrimTypeTokens->material, path), destroy_material);
  Check(material != nullptr, "could not create material sprim");

  const auto surface_path = path.AppendChild(TfToken("surface"));
  const auto image_path = path.AppendChild(TfToken("image"));
  const auto uv_path = path.AppendChild(TfToken("uv"));
  const TfToken out("out");
  const TfToken texcoord("texcoord");
  const TfToken geomprop("geomprop");
  const TfToken fallback("default");
  auto& surface = source.network.nodes[surface_path];
  surface.nodeTypeId = TfToken("ND_standard_surface_surfaceshader");
  surface.parameters[TfToken("emission")] = VtValue(0.5F);
  surface.inputConnections[TfToken("emission_color")] = {{image_path, out}};
  source.network.terminals[HdMaterialTerminalTokens->surface] = {surface_path, out};
  auto& image = source.network.nodes[image_path];
  image.nodeTypeId = TfToken("ND_image_color3");
  image.parameters[TfToken("file")] = VtValue(SdfAssetPath(texture_path));
  auto& uv = source.network.nodes[uv_path];

  auto state = std::make_shared<HdRenderPassState>();
  state->SetViewport(GfVec4d(0.0, 0.0, 16.0, 16.0));
  auto pass = delegate.CreateRenderPass(index.get(), HdRprimCollection());
  const auto sync = [&] {
    HdDirtyBits bits = HdMaterial::AllDirty;
    material->Sync(&source, nullptr, &bits);
    Check(bits == HdMaterial::Clean, "material dirty bits were not consumed");
    pass->Execute(state, {});
    Check(backend->snapshot != nullptr, "no adapter snapshot submitted");
    const auto& materials = backend->snapshot->materials;
    const auto record = std::find_if(materials.begin(), materials.end(), [](const auto& item) {
      return merlin::HasMaterialFeature(item.features, merlin::MaterialFeature::ForwardSpecular);
    });
    Check(record != materials.end(), "no Standard Surface material in the snapshot");
    return *record;
  };
  const auto check_uv0 = [](const auto& record) {
    Check(record.emissive_texture && record.emissive_texture->texcoord_set == 0,
        "image did not bind to UV0");
    Check(record.parameters.emissive_radiance.x == 0.5F,
        "image lost its authored emission weight");
  };
  const auto check_rejection = [&](std::string_view reason) {
    const auto record = sync();
    Check(!record.emissive_texture && record.parameters.emissive_radiance.x == 0.0F,
        "unsupported coordinates did not fall back to zero emission");
    const auto diagnostics = delegate.GetLatestViewportFrame().diagnostics;
    Check(std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& item) {
      return item.code == "hydra.material.materialx-subset" &&
             item.source == path.GetString() &&
             item.message.find(reason) != std::string::npos;
    }),
        "material fallback did not report its UV diagnostic");
  };

  const auto implicit = sync();
  check_uv0(implicit);
  image.inputConnections[texcoord] = {{uv_path, out}};
  uv.nodeTypeId = TfToken("ND_texcoord_vector2");
  uv.parameters[TfToken("index")] = VtValue(0);
  const auto indexed = sync();
  check_uv0(indexed);
  Check(indexed.revision == implicit.revision, "explicit UV0 changed the material");
  uv.parameters[TfToken("index")] = VtValue(1);
  check_rejection("requires texture coordinate set 0");
  uv.parameters.clear();
  uv.nodeTypeId = TfToken("ND_geompropvalue_vector2");
  uv.parameters[geomprop] = VtValue(std::string("st"));
  const auto named = sync();
  check_uv0(named);
  Check(named.material == implicit.material, "named UV changed material identity");
  uv.parameters[fallback] = VtValue(GfVec2f(0.0F));
  Check(sync().revision == named.revision, "zero default changed the material");
  uv.parameters[fallback] = VtValue(GfVec2f(0.25F));
  check_rejection("custom MaterialX geompropvalue defaults");
  uv.parameters.erase(fallback);
  uv.parameters[geomprop] = VtValue(TfToken("st1"));
  check_rejection("requires the st primvar");
  uv.parameters[geomprop] = VtValue(TfToken("st"));
  check_uv0(sync());

  const TfToken opacity("opacity");
  const TfToken file("file");
  surface.inputConnections[opacity] = {{image_path, out}};
  const auto masked = sync();
  check_uv0(masked);
  Check(masked.alpha_mode == merlin::AlphaMode::Masked &&
        masked.parameters.alpha_cutoff == 0.5F &&
        masked.opacity_texture && masked.opacity_texture->texcoord_set == 0 &&
        merlin::HasMaterialFeature(masked.features, merlin::MaterialFeature::OpacityTexture),
      "opacity image did not select UV0 alpha masking");
  Check(masked.opacity_texture->texture_index == masked.emissive_texture->texture_index,
      "opacity and emission could not share the same image");

  image.parameters[file] = VtValue(SdfAssetPath());
  const auto missing = sync();
  Check(missing.alpha_mode == merlin::AlphaMode::Opaque && !missing.opacity_texture &&
        !merlin::HasMaterialFeature(missing.features, merlin::MaterialFeature::OpacityTexture),
      "missing opacity image did not fall back to opaque");
  const auto diagnostics = delegate.GetLatestViewportFrame().diagnostics;
  Check(std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& item) {
    return item.code == "hydra.material.materialx-subset" &&
           item.source == path.GetString() && item.message.find("opacity (") != std::string::npos;
  }), "missing opacity image did not report the input name");
  image.parameters[file] = VtValue(SdfAssetPath(texture_path));
  const auto recovered = sync();
  Check(recovered.material == masked.material && recovered.opacity_texture &&
        recovered.alpha_mode == merlin::AlphaMode::Masked,
      "restoring the opacity image did not recover the same masked material");

  surface.inputConnections.erase(opacity);
  const auto disconnected = sync();
  check_uv0(disconnected);
  Check(disconnected.alpha_mode == merlin::AlphaMode::Opaque && !disconnected.opacity_texture,
      "disconnecting opacity changed the remaining material inputs");
}

} // namespace

int main(int argc, char** argv) try {
  Check(argc == 2, "expected checker texture path");
  VerifyImageUv(argv[1]);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
