#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace merlin {

namespace detail {
struct HandleFactory;
}

struct Vec2 {
  float x{};
  float y{};
};

struct Vec3 {
  float x{};
  float y{};
  float z{};
};

struct Vec4 {
  float x{};
  float y{};
  float z{};
  float w{};
};

struct Mat4 {
  std::array<float, 16> values{
      1.0F, 0.0F, 0.0F, 0.0F,
      0.0F, 1.0F, 0.0F, 0.0F,
      0.0F, 0.0F, 1.0F, 0.0F,
      0.0F, 0.0F, 0.0F, 1.0F};
};

template <typename Tag>
class Handle {
 public:
  constexpr Handle() = default;

  // ChangeSet consumers receive serialized handle values. Rehydrating a handle
  // does not make it valid; RenderWorld still performs generation validation.
  [[nodiscard]] static constexpr Handle FromValue(std::uint64_t value) noexcept {
    return Handle(value);
  }

  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  friend constexpr bool operator==(Handle, Handle) = default;

 private:
  explicit constexpr Handle(std::uint64_t value) : value_(value) {}
  std::uint64_t value_{};

  friend struct detail::HandleFactory;
};

struct MeshTag;
struct MaterialTag;
struct InstanceTag;
struct CameraTag;
struct LightTag;

using MeshHandle = Handle<MeshTag>;
using MaterialHandle = Handle<MaterialTag>;
using InstanceHandle = Handle<InstanceTag>;
using CameraHandle = Handle<CameraTag>;
using LightHandle = Handle<LightTag>;

struct MeshDescriptor {
  std::string label;
  std::vector<Vec3> positions;
  std::vector<Vec3> normals;
  std::vector<Vec2> texcoords;
  std::vector<std::uint32_t> indices;
};

enum class AlphaMode { Opaque, Masked, Blended };

struct MaterialDescriptor {
  std::string label;
  Vec4 base_color{1.0F, 1.0F, 1.0F, 1.0F};
  float metallic{};
  float roughness{0.5F};
  AlphaMode alpha_mode{AlphaMode::Opaque};
  bool double_sided{};
};

struct InstanceDescriptor {
  std::string label;
  MeshHandle mesh;
  MaterialHandle material;
  Mat4 transform;
  bool visible{true};
};

struct CameraDescriptor {
  std::string label;
  Mat4 view;
  Mat4 projection;
};

enum class LightType { Directional, Point, Spot, Dome };

struct LightDescriptor {
  std::string label;
  LightType type{LightType::Directional};
  Vec3 color{1.0F, 1.0F, 1.0F};
  float intensity{1.0F};
  Mat4 transform;
};

}  // namespace merlin
