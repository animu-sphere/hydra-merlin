#include "adapter.hpp"

#include <pxr/pxr.h>

#include <pxr/base/tf/registryManager.h>
#include <pxr/imaging/hd/rendererCreateArgs.h>
#include <pxr/imaging/hd/rendererPlugin.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/version.h>

#include <string>

PXR_NAMESPACE_OPEN_SCOPE

class HdMerlinRendererPlugin final : public HdRendererPlugin {
 public:
  bool IsSupported(const HdRendererCreateArgs& args,
                   std::string* reason_why_not) const override {
#if HD_API_VERSION >= 98
    const auto gpu_enabled_source = args.GetGpuEnabled();
    const bool gpu_enabled =
        !gpu_enabled_source || gpu_enabled_source->GetTypedValue(0.0F);
#else
    const bool gpu_enabled = args.gpuEnabled;
#endif
    if (!gpu_enabled) {
      if (reason_why_not != nullptr) {
        *reason_why_not = "Merlin requires a supported GPU backend";
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
