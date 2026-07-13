#pragma once

#include <cstdint>
#include <vector>

namespace merlin {

enum class ObjectKind {
  Mesh,
  Material,
  Texture,
  Sampler,
  Instance,
  Camera,
  Light,
  RenderSettings
};
enum class ChangeKind { Created, Updated, Removed };

enum class ChangeAspect : std::uint32_t {
  None = 0,
  Topology = 1U << 0U,
  Points = 1U << 1U,
  Primvars = 1U << 2U,
  Transform = 1U << 3U,
  Visibility = 1U << 4U,
  MaterialBinding = 1U << 5U,
  MaterialParameters = 1U << 6U,
  Camera = 1U << 7U,
  LightParameters = 1U << 8U,
  RenderSettings = 1U << 9U,
  MaterialFeatures = 1U << 10U,
  TextureData = 1U << 11U,
  SamplerParameters = 1U << 12U,
  All = (1U << 13U) - 1U
};

[[nodiscard]] constexpr ChangeAspect operator|(ChangeAspect lhs,
                                               ChangeAspect rhs) noexcept {
  return static_cast<ChangeAspect>(static_cast<std::uint32_t>(lhs) |
                                   static_cast<std::uint32_t>(rhs));
}

[[nodiscard]] constexpr ChangeAspect operator&(ChangeAspect lhs,
                                               ChangeAspect rhs) noexcept {
  return static_cast<ChangeAspect>(static_cast<std::uint32_t>(lhs) &
                                   static_cast<std::uint32_t>(rhs));
}

constexpr ChangeAspect& operator|=(ChangeAspect& lhs,
                                   ChangeAspect rhs) noexcept {
  lhs = lhs | rhs;
  return lhs;
}

[[nodiscard]] constexpr bool HasAnyAspect(ChangeAspect value,
                                          ChangeAspect mask) noexcept {
  return (value & mask) != ChangeAspect::None;
}

[[nodiscard]] constexpr ChangeAspect DefaultChangeAspects(
    ObjectKind kind) noexcept {
  switch (kind) {
    case ObjectKind::Mesh:
      return ChangeAspect::Topology | ChangeAspect::Points |
             ChangeAspect::Primvars;
    case ObjectKind::Material:
      return ChangeAspect::MaterialParameters | ChangeAspect::MaterialFeatures;
    case ObjectKind::Texture:
      return ChangeAspect::TextureData;
    case ObjectKind::Sampler:
      return ChangeAspect::SamplerParameters;
    case ObjectKind::Instance:
      return ChangeAspect::Transform | ChangeAspect::Visibility |
             ChangeAspect::MaterialBinding;
    case ObjectKind::Camera:
      return ChangeAspect::Camera;
    case ObjectKind::Light:
      return ChangeAspect::Transform | ChangeAspect::LightParameters;
    case ObjectKind::RenderSettings:
      return ChangeAspect::RenderSettings;
  }
  return ChangeAspect::None;
}

struct Change {
  ObjectKind object_kind{};
  ChangeKind change_kind{};
  std::uint64_t handle{};
  ChangeAspect aspects{ChangeAspect::None};
  std::uint64_t resource_revision{};

  [[nodiscard]] constexpr bool HasAspect(ChangeAspect aspect) const noexcept {
    return HasAnyAspect(aspects, aspect);
  }
};

struct ChangeSet {
  std::uint64_t revision{};
  std::vector<Change> changes;

  [[nodiscard]] bool empty() const noexcept { return changes.empty(); }
};

}  // namespace merlin
