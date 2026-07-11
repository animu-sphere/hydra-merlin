#include <pxr/pxr.h>

#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>

#include <iostream>

PXR_NAMESPACE_USING_DIRECTIVE

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "expected plugin resource directory\n";
    return 2;
  }
  const auto plugins = PlugRegistry::GetInstance().RegisterPlugins(argv[1]);
  if (plugins.empty()) {
    std::cerr << "plugin resource directory registered no plugins\n";
    return 1;
  }
  const TfToken plugin_id("HdMerlinRendererPlugin");
  if (!HdRendererPluginRegistry::GetInstance().IsRegisteredPlugin(plugin_id)) {
    std::cerr << "HdMerlinRendererPlugin was not discovered\n";
    return 1;
  }
  return 0;
}
