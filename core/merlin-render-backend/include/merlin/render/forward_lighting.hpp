#pragma once

#include <merlin/extraction/frame_snapshot.hpp>

namespace merlin::render {

// The current Forward profile evaluates one directional light. Select by
// stable identity, never by the dense table slot, which can move on removal.
// With no directional input, retain the unit +Z diagnostic light.
struct ForwardDirectionalLighting {
  Vec4 direction_intensity{0.0F, 0.0F, 1.0F, 1.0F};
  Vec3 color{1.0F, 1.0F, 1.0F};
  std::uint64_t light{};
};

// Directional lights emit along local -Z; the surface-to-source vector is
// transformed +Z. Translation and camera matrices do not affect it. A zero or
// non-finite axis uses +Z. Non-finite/negative color or intensity is rejected.
[[nodiscard]] ForwardDirectionalLighting ExtractForwardDirectionalLighting(
    const extraction::FrameSnapshot& snapshot);

} // namespace merlin::render
