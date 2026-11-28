#include "adapter.hpp"

#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/assetPath.h>

#include <algorithm>
#include <initializer_list>
#include <iostream>
#include <limits>
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

void VerifyStandardSurface(const char* texture_path) {
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
  const auto check_subset_diagnostic = [&](std::initializer_list<std::string_view> reasons) {
    const auto diagnostics = delegate.GetLatestViewportFrame().diagnostics;
    Check(std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& item) {
      return item.code == "hydra.material.materialx-subset" &&
             item.source == path.GetString() &&
             std::all_of(reasons.begin(), reasons.end(), [&](std::string_view reason) {
               return item.message.find(reason) != std::string::npos;
             });
    }),
        "material fallback did not report the expected diagnostic");
  };
  const auto check_rejection = [&](std::string_view reason) {
    const auto record = sync();
    Check(!record.emissive_texture && record.parameters.emissive_radiance.x == 0.0F,
        "unsupported emission input did not fall back to zero emission");
    check_subset_diagnostic({reason});
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
  check_subset_diagnostic({"opacity ("});
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

  const auto color_path = path.AppendChild(TfToken("constantColor"));
  const TfToken emission_color("emission_color");
  const TfToken value("value");
  auto& color = source.network.nodes[color_path];
  color.nodeTypeId = TfToken("ND_constant_color3");
  surface.parameters[emission_color] = VtValue(GfVec3f(1.0F, 0.5F, 0.25F));
  surface.inputConnections.erase(emission_color);
  const auto inline_color = sync();

  surface.inputConnections[emission_color] = {{color_path, out}};
  color.parameters[value] = surface.parameters[emission_color];
  const auto connected = sync();
  Check(!connected.emissive_texture && connected.revision == inline_color.revision,
      "equivalent constant node changed the material revision");
  color.parameters.erase(value);
  const auto default_color = sync();
  Check(default_color.parameters.emissive_radiance.x == 0.0F,
      "constant node without a value did not use its zero default");

  color.parameters[value] = VtValue(GfVec3f(0.5F, 1.0F, 0.25F));
  color.inputConnections[value] = {{image_path, out}};
  check_rejection("emission_color (");
  color.inputConnections.erase(value);
  surface.inputConnections[emission_color] = {{color_path, TfToken("wrongOutput")}};
  check_rejection("emission_color (");
  surface.inputConnections[emission_color] = {{color_path, out}};
  const TfToken color_space("colorSpace:value");
  color.parameters[color_space] = VtValue(TfToken("srgb_texture"));
  check_rejection("emission_color (");
  color.parameters[color_space] = VtValue(TfToken("lin_rec709"));
  const auto edited = sync();
  Check(edited.material == inline_color.material && !edited.emissive_texture &&
        edited.parameters.emissive_radiance.x == 0.25F &&
        edited.parameters.emissive_radiance.y == 0.5F &&
        edited.parameters.emissive_radiance.z == 0.125F,
      "valid constant node edit did not update the existing material");

  const TfToken base("base");
  const TfToken base_color("base_color");
  const TfToken metalness("metalness");
  const TfToken roughness("specular_roughness");
  const auto scalar_path = path.AppendChild(TfToken("constantScalar"));
  auto& scalar = source.network.nodes[scalar_path];
  scalar.nodeTypeId = TfToken("ND_constant_float");
  scalar.parameters[value] = VtValue(0.5F);
  surface.parameters[base] = surface.parameters[metalness] =
      surface.parameters[roughness] = scalar.parameters[value];
  surface.parameters[base_color] = color.parameters[value];
  const auto inline_surface = sync();
  for (const auto& input : {base, metalness, roughness}) {
    surface.inputConnections[input] = {{scalar_path, out}};
  }
  surface.inputConnections[base_color] = {{color_path, out}};
  const auto connected_surface = sync();
  Check(connected_surface.revision == inline_surface.revision &&
        !connected_surface.base_color_texture && !connected_surface.metallic_texture &&
        !connected_surface.roughness_texture,
      "equivalent base and metallic-roughness constants changed the material");

  scalar.parameters[value] = VtValue(0.25F);
  const auto edited_surface = sync();
  Check(edited_surface.material == inline_surface.material &&
        edited_surface.revision != inline_surface.revision &&
        edited_surface.parameters.base_color.x == 0.125F &&
        edited_surface.parameters.base_color.y == 0.25F &&
        edited_surface.parameters.base_color.z == 0.0625F &&
        edited_surface.parameters.base_color.w == 1.0F &&
        edited_surface.parameters.metallic == 0.25F &&
        edited_surface.parameters.roughness == 0.25F,
      "constant value edit did not update the existing opaque material");

  surface.inputConnections.erase(base);
  scalar.parameters[value] = VtValue(2.0F);
  const auto clamped = sync();
  Check(clamped.parameters.metallic == 1.0F && clamped.parameters.roughness == 1.0F,
      "connected metallic and roughness values did not use the literal clamp");
  scalar.parameters.erase(value);
  color.parameters.erase(value);
  const auto defaults = sync();
  Check(defaults.parameters.base_color.x == 0.0F && defaults.parameters.base_color.w == 1.0F &&
        defaults.parameters.metallic == 0.0F && defaults.parameters.roughness == 0.0F,
      "omitted constant values did not use their node defaults");

  surface.inputConnections[base] = {{scalar_path, out}};
  scalar.parameters[value] = VtValue(std::numeric_limits<float>::infinity());
  surface.inputConnections[base_color] = {{color_path, TfToken("wrongOutput")}};
  const auto fallback_surface = sync();
  Check(fallback_surface.parameters.base_color.x == 0.25F &&
        fallback_surface.parameters.metallic == 0.5F &&
        fallback_surface.parameters.roughness == 0.5F,
      "invalid constant connections did not retain the inline fallback values");
  check_subset_diagnostic({"base (", "base_color (", "metalness (", "specular_roughness ("});

  scalar.parameters[value] = VtValue(0.25F);
  surface.inputConnections[base_color] = {{image_path, out}};
  const auto scalar_image_path = path.AppendChild(TfToken("scalarImage"));
  auto& scalar_image = source.network.nodes[scalar_image_path];
  scalar_image.nodeTypeId = TfToken("ND_image_float");
  scalar_image.parameters[file] = image.parameters[file];
  surface.inputConnections[metalness] = surface.inputConnections[roughness] =
      {{scalar_image_path, out}};
  const auto textured = sync();
  Check(textured.base_color_texture && textured.metallic_texture && textured.roughness_texture &&
        textured.parameters.base_color.x == 0.25F &&
        textured.parameters.metallic == 0.5F && textured.parameters.roughness == 0.5F,
      "restoring images lost the base weight or metallic-roughness fallback values");

  const TfToken transmission("transmission");
  const TfToken transmission_color("transmission_color");
  surface.inputConnections.erase(emission_color);
  surface.inputConnections.erase(base);
  surface.parameters[base] = VtValue(0.25F);
  surface.parameters[transmission] = VtValue(0.5F);
  surface.parameters[transmission_color] = VtValue(GfVec3f(0.25F, 0.5F, 1.0F));
  const auto inline_transmission = sync();
  Check(inline_transmission.parameters.transmission_weight == 0.5F &&
        inline_transmission.parameters.transmission_color.x == 0.25F &&
        inline_transmission.parameters.transmission_color.y == 0.5F &&
        inline_transmission.parameters.transmission_color.z == 1.0F &&
        inline_transmission.alpha_mode == merlin::AlphaMode::Opaque,
      "authored transmission changed coverage or lost the tint");
  scalar.parameters[value] = surface.parameters[transmission];
  color.parameters[value] = surface.parameters[transmission_color];
  surface.inputConnections[transmission] = {{scalar_path, out}};
  surface.inputConnections[transmission_color] = {{color_path, out}};
  const auto connected_transmission = sync();
  color.parameters[value] = VtValue(GfVec3f(1.0F, 0.5F, 0.25F));
  const auto edited_transmission = sync();
  Check(connected_transmission.revision == inline_transmission.revision &&
        edited_transmission.material == connected_transmission.material &&
        edited_transmission.revision != connected_transmission.revision &&
        edited_transmission.parameters.transmission_color.x == 1.0F,
      "connected transmission tint edit did not update the existing material");
  for (const auto* input : {"transmission_depth", "thin_walled"}) {
    surface.parameters[TfToken(input)] =
        std::string_view(input) == "thin_walled" ? VtValue(true) : VtValue(0.5F);
    Check(sync().parameters.transmission_weight == 0.0F,
        "unsupported dielectric mode did not fall back to zero transmission");
    check_subset_diagnostic({input});
    surface.parameters.erase(TfToken(input));
  }
  color.parameters[value] = VtValue(GfVec3f(-0.25F, 0.5F, 1.0F));
  Check(sync().parameters.transmission_weight == 0.0F,
      "invalid tint did not fall back to zero transmission");
  check_subset_diagnostic({"transmission_color ("});
  surface.inputConnections.erase(transmission_color);
  surface.parameters.erase(transmission_color);
  const auto default_tint = sync();
  Check(default_tint.parameters.transmission_weight == 0.5F &&
        default_tint.parameters.transmission_color.x == 1.0F &&
        default_tint.parameters.transmission_color.y == 1.0F &&
        default_tint.parameters.transmission_color.z == 1.0F,
      "transmission did not recover with the default white tint");
}

} // namespace

int main(int argc, char** argv) try {
  Check(argc == 2, "expected checker texture path");
  VerifyStandardSurface(argv[1]);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
