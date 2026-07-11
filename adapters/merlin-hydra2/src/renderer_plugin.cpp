#include <pxr/pxr.h>

#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/registryManager.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/rendererCreateArgs.h>
#include <pxr/imaging/hd/rendererPlugin.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/imaging/hd/tokens.h>

#include <memory>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

class HdMerlinRenderPass final : public HdRenderPass {
 public:
  HdMerlinRenderPass(HdRenderIndex* index,
                     const HdRprimCollection& collection)
      : HdRenderPass(index, collection) {}

 private:
  void _Execute(const HdRenderPassStateSharedPtr& render_pass_state,
                const TfTokenVector& render_tags) override {
    (void)render_pass_state;
    (void)render_tags;
    // The discovery slice intentionally performs no rendering. Mesh ingress,
    // AOV binding, and the RenderWorld commit boundary land in the next slice.
  }
};

class HdMerlinRenderDelegate final : public HdRenderDelegate {
 public:
  explicit HdMerlinRenderDelegate(const HdRenderSettingsMap& settings = {})
      : HdRenderDelegate(settings),
        resources_(std::make_shared<HdResourceRegistry>()) {}

  const TfTokenVector& GetSupportedRprimTypes() const override {
    static const TfTokenVector empty;
    return empty;
  }

  const TfTokenVector& GetSupportedSprimTypes() const override {
    static const TfTokenVector empty;
    return empty;
  }

  const TfTokenVector& GetSupportedBprimTypes() const override {
    static const TfTokenVector empty;
    return empty;
  }

  HdResourceRegistrySharedPtr GetResourceRegistry() const override {
    return resources_;
  }

  HdRenderPassSharedPtr CreateRenderPass(
      HdRenderIndex* index, const HdRprimCollection& collection) override {
    return std::make_shared<HdMerlinRenderPass>(index, collection);
  }

  HdInstancer* CreateInstancer(HdSceneDelegate* delegate,
                               const SdfPath& id) override {
    (void)delegate;
    (void)id;
    return nullptr;
  }

  void DestroyInstancer(HdInstancer* instancer) override { (void)instancer; }

  HdRprim* CreateRprim(const TfToken& type_id,
                       const SdfPath& rprim_id) override {
    (void)type_id;
    (void)rprim_id;
    return nullptr;
  }

  void DestroyRprim(HdRprim* rprim) override { (void)rprim; }

  HdSprim* CreateSprim(const TfToken& type_id,
                       const SdfPath& sprim_id) override {
    (void)type_id;
    (void)sprim_id;
    return nullptr;
  }

  HdSprim* CreateFallbackSprim(const TfToken& type_id) override {
    (void)type_id;
    return nullptr;
  }

  void DestroySprim(HdSprim* sprim) override { (void)sprim; }

  HdBprim* CreateBprim(const TfToken& type_id,
                       const SdfPath& bprim_id) override {
    (void)type_id;
    (void)bprim_id;
    return nullptr;
  }

  HdBprim* CreateFallbackBprim(const TfToken& type_id) override {
    (void)type_id;
    return nullptr;
  }

  void DestroyBprim(HdBprim* bprim) override { (void)bprim; }

  void CommitResources(HdChangeTracker* tracker) override { (void)tracker; }

  HdAovDescriptor GetDefaultAovDescriptor(const TfToken& name) const override {
    if (name == HdAovTokens->color) {
      return {HdFormatUNorm8Vec4, false, VtValue(GfVec4f(0.0F))};
    }
    if (name == HdAovTokens->depth) {
      return {HdFormatFloat32, false, VtValue(1.0F)};
    }
    return {};
  }

 private:
  HdResourceRegistrySharedPtr resources_;
};

}  // namespace

class HdMerlinRendererPlugin final : public HdRendererPlugin {
 public:
  bool IsSupported(const HdRendererCreateArgs& args,
                   std::string* reason_why_not) const override {
    if (!args.gpuEnabled) {
      if (reason_why_not != nullptr) {
        *reason_why_not = "Merlin requires a Vulkan-capable GPU";
      }
      return false;
    }
    return true;
  }

  HdRenderDelegate* CreateRenderDelegate() override {
    return new HdMerlinRenderDelegate;
  }

  HdRenderDelegate* CreateRenderDelegate(
      const HdRenderSettingsMap& settings) override {
    return new HdMerlinRenderDelegate(settings);
  }

  void DeleteRenderDelegate(HdRenderDelegate* delegate) override {
    delete delegate;
  }
};

TF_REGISTRY_FUNCTION(TfType) {
  HdRendererPluginRegistry::Define<HdMerlinRendererPlugin>();
}

PXR_NAMESPACE_CLOSE_SCOPE
