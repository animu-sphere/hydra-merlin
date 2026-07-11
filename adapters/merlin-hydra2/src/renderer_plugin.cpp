#include "adapter.hpp"

#include <pxr/pxr.h>

#include <pxr/base/tf/registryManager.h>
#include <pxr/imaging/hd/rendererCreateArgs.h>
#include <pxr/imaging/hd/rendererPlugin.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>

#include <string>

PXR_NAMESPACE_OPEN_SCOPE

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
