#pragma once

#include <merlin/metal/backend.hpp>

namespace merlin::viewport {

class Window;

[[nodiscard]] metal::PresentationOptions MakeGlfwMetalPresentation(
    Window& window, bool vsync);

}  // namespace merlin::viewport
