#pragma once

#include <cstdint>
#include <string_view>

namespace merlin {

enum class Aov {
  Color,
  Depth,
  Normal,
  Albedo,
  Roughness,
  Metallic,
  Emission,
  PrimId,
  InstanceId,
  MotionVector
};

[[nodiscard]] constexpr std::string_view AovName(Aov aov) noexcept {
  switch (aov) {
    case Aov::Color: return "color";
    case Aov::Depth: return "depth";
    case Aov::Normal: return "normal";
    case Aov::Albedo: return "albedo";
    case Aov::Roughness: return "roughness";
    case Aov::Metallic: return "metallic";
    case Aov::Emission: return "emission";
    case Aov::PrimId: return "primId";
    case Aov::InstanceId: return "instanceId";
    case Aov::MotionVector: return "motionVector";
  }
  return "unknown";
}

struct RenderProduct {
  std::uint32_t width{512};
  std::uint32_t height{512};
  Aov aov{Aov::Color};
};

}  // namespace merlin
