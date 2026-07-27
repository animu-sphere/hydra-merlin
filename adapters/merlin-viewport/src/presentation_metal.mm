#include "presentation_metal.hpp"

#include "window.hpp"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <cstdint>
#include <stdexcept>

namespace merlin::viewport {

metal::PresentationOptions MakeGlfwMetalPresentation(Window& window,
                                                      bool vsync) {
  auto* glfw_window = static_cast<GLFWwindow*>(window.native_window());
  NSWindow* cocoa_window = glfwGetCocoaWindow(glfw_window);
  if (cocoa_window == nil || cocoa_window.contentView == nil) {
    throw std::runtime_error(
        "GLFW did not provide a Cocoa content view for Metal presentation");
  }

  NSView* view = cocoa_window.contentView;
  view.wantsLayer = YES;
  CAMetalLayer* layer = [CAMetalLayer layer];
  layer.frame = view.bounds;
  layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
  layer.contentsScale = cocoa_window.backingScaleFactor;
  layer.drawableSize = CGSizeMake(window.width(), window.height());
  view.layer = layer;
  if (glfwGetWindowAttrib(glfw_window, GLFW_VISIBLE) != GLFW_TRUE) {
    // CAMetalLayer does not vend drawables for an ordered-out window. Keep the
    // capability-test surface compositor-active but fully transparent.
    cocoa_window.alphaValue = 0.0;
    [cocoa_window orderFront:nil];
  }

  metal::PresentationOptions result;
  result.layer =
      reinterpret_cast<std::uintptr_t>((__bridge void*)layer);
  result.vsync = vsync;
  result.color_space = metal::PresentationColorSpace::Srgb;
  result.dynamic_range = metal::PresentationDynamicRange::Standard;
  result.drawable_count = 3;
  return result;
}

}  // namespace merlin::viewport
