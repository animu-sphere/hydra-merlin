#pragma once

#include <array>
#include <cstdint>
#include <optional>
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

// Color attachment clear value used by every backend and adapter when the
// host does not supply one. Viewport background expectations in tests are
// quantized from these components.
inline constexpr Vec4 kDefaultClearColor{0.018F, 0.025F, 0.028F, 1.0F};

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
struct TextureTag;
struct SamplerTag;
struct InstanceTag;
struct CameraTag;
struct LightTag;
struct RenderSettingsTag;

using MeshHandle = Handle<MeshTag>;
using MaterialHandle = Handle<MaterialTag>;
using TextureHandle = Handle<TextureTag>;
using SamplerHandle = Handle<SamplerTag>;
using InstanceHandle = Handle<InstanceTag>;
using CameraHandle = Handle<CameraTag>;
using LightHandle = Handle<LightTag>;
using RenderSettingsHandle = Handle<RenderSettingsTag>;

struct MeshDescriptor {
  std::string label;
  // Mesh input adapters normalize interpolation and indexing so every
  // optional primvar has either zero entries or one entry per position.
  std::vector<Vec3> positions;
  std::vector<Vec3> normals;
  std::vector<Vec4> colors;
  std::vector<Vec2> texcoords;
  std::vector<std::uint32_t> indices;
};

enum class AlphaMode { Opaque, Masked, Blended };

enum class TextureFormat { Rgba8Unorm };

struct TextureDescriptor {
  std::string label;
  std::uint32_t width{};
  std::uint32_t height{};
  TextureFormat format{TextureFormat::Rgba8Unorm};
  // Tightly packed, top-left RGBA texels. Texture resources are revisioned by
  // RenderWorld in exactly the same way as meshes and materials.
  std::vector<std::uint8_t> pixels;
};

enum class FilterMode { Nearest, Linear };
enum class AddressMode { Repeat, MirroredRepeat, ClampToEdge };

struct SamplerDescriptor {
  std::string label;
  FilterMode min_filter{FilterMode::Linear};
  FilterMode mag_filter{FilterMode::Linear};
  AddressMode address_u{AddressMode::Repeat};
  AddressMode address_v{AddressMode::Repeat};
};

enum class MaterialFeature : std::uint32_t {
  None = 0,
  VertexColor = 1U << 0U,
  BaseColorTexture = 1U << 1U,
  DirectionalLight = 1U << 2U,
};

[[nodiscard]] constexpr MaterialFeature operator|(MaterialFeature lhs,
                                                   MaterialFeature rhs) noexcept {
  return static_cast<MaterialFeature>(static_cast<std::uint32_t>(lhs) |
                                      static_cast<std::uint32_t>(rhs));
}

[[nodiscard]] constexpr MaterialFeature operator&(MaterialFeature lhs,
                                                   MaterialFeature rhs) noexcept {
  return static_cast<MaterialFeature>(static_cast<std::uint32_t>(lhs) &
                                      static_cast<std::uint32_t>(rhs));
}

constexpr MaterialFeature& operator|=(MaterialFeature& lhs,
                                      MaterialFeature rhs) noexcept {
  lhs = lhs | rhs;
  return lhs;
}

[[nodiscard]] constexpr bool HasMaterialFeature(MaterialFeature value,
                                                MaterialFeature feature) noexcept {
  return (value & feature) != MaterialFeature::None;
}

struct MaterialParameterBlock {
  Vec4 base_color{1.0F, 1.0F, 1.0F, 1.0F};
  float metallic{};
  float roughness{0.5F};
  float alpha_cutoff{0.5F};
};

struct TextureBinding {
  TextureHandle texture;
  SamplerHandle sampler;
  std::uint32_t texcoord_set{};
};

// Versioned, host-neutral contract for generated material functions. Source
// integrations populate this logical metadata; renderer backends derive their
// own native bindings and pipeline layouts from it.
inline constexpr std::uint32_t kMaterialAbiVersion = 1;
inline constexpr std::uint32_t kMaterialReflectionSchemaVersion = 1;

enum class MaterialValueType {
  Float,
  Float2,
  Float3,
  Float4,
  Integer,
  Boolean,
  Texture2D,
  Sampler,
  CombinedTextureSampler,
  Unknown,
};

struct MaterialParameterLayoutEntry {
  std::string name;
  MaterialValueType type{MaterialValueType::Unknown};
  std::uint32_t array_size{1};
};

struct MaterialParameterLayout {
  std::vector<MaterialParameterLayoutEntry> entries;
};

struct MaterialResourceLayoutEntry {
  std::string name;
  MaterialValueType type{MaterialValueType::Unknown};
  std::uint32_t array_size{1};
};

struct MaterialResourceLayout {
  std::vector<MaterialResourceLayoutEntry> entries;
};

enum class MaterialInputRequirement : std::uint32_t {
  None = 0,
  Position = 1U << 0U,
  ShadingNormal = 1U << 1U,
  Texcoord0 = 1U << 2U,
};

[[nodiscard]] constexpr MaterialInputRequirement operator|(
    MaterialInputRequirement lhs, MaterialInputRequirement rhs) noexcept {
  return static_cast<MaterialInputRequirement>(
      static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

constexpr MaterialInputRequirement& operator|=(
    MaterialInputRequirement& lhs, MaterialInputRequirement rhs) noexcept {
  lhs = lhs | rhs;
  return lhs;
}

enum class MaterialResultField : std::uint32_t {
  None = 0,
  BaseColor = 1U << 0U,
  Metalness = 1U << 1U,
  SpecularRoughness = 1U << 2U,
  ShadingNormal = 1U << 3U,
};

[[nodiscard]] constexpr MaterialResultField operator|(
    MaterialResultField lhs, MaterialResultField rhs) noexcept {
  return static_cast<MaterialResultField>(static_cast<std::uint32_t>(lhs) |
                                          static_cast<std::uint32_t>(rhs));
}

constexpr MaterialResultField& operator|=(MaterialResultField& lhs,
                                          MaterialResultField rhs) noexcept {
  lhs = lhs | rhs;
  return lhs;
}

struct MaterialFeatureRequirements {
  MaterialInputRequirement inputs{MaterialInputRequirement::None};
  MaterialResultField results{MaterialResultField::None};
};

struct MaterialModule {
  std::string key;
  std::string entry_point{"evaluateMaterial"};
  std::uint32_t abi_version{kMaterialAbiVersion};
  std::uint32_t reflection_schema_version{kMaterialReflectionSchemaVersion};
  // Monotonic content revision for replacement under a stable external
  // material identity. Parameter-only edits do not change this revision.
  std::uint64_t revision{1};
  MaterialParameterLayout parameters;
  MaterialResourceLayout resources;
  MaterialFeatureRequirements requirements;
};

// MaterialIR is the only material representation accepted by the host-neutral
// scene model. Source graphs (Hydra, MaterialX, or a future DCC adapter) must be
// normalized before they cross this boundary.
struct MaterialIR {
  std::string label;
  MaterialParameterBlock parameters;
  std::optional<TextureBinding> base_color_texture;
  AlphaMode alpha_mode{AlphaMode::Opaque};
  bool double_sided{};
  MaterialFeature features{MaterialFeature::VertexColor |
                           MaterialFeature::DirectionalLight};
  // Generated module identity and logical reflection are optional. Existing
  // basic materials continue through the handwritten renderer path.
  std::optional<MaterialModule> module;
};

using MaterialDescriptor = MaterialIR;

struct InstanceDescriptor {
  std::string label;
  MeshHandle mesh;
  MaterialHandle material;
  Mat4 transform;
  bool visible{true};
};

// Winding after the camera projection and viewport transform. This is carried
// with the camera because a projection-space axis reflection reverses it.
enum class FrontFaceWinding {
  Clockwise,
  CounterClockwise,
};

struct CameraDescriptor {
  std::string label;
  Mat4 view;
  Mat4 projection;
  FrontFaceWinding front_face{FrontFaceWinding::Clockwise};
};

enum class LightType { Directional, Point, Spot, Dome };

struct LightDescriptor {
  std::string label;
  LightType type{LightType::Directional};
  Vec3 color{1.0F, 1.0F, 1.0F};
  float intensity{1.0F};
  // Directional lights emit along transformed local -Z; shading uses the
  // opposite transformed +Z vector toward the source.
  Mat4 transform;
};

struct RenderSettingsDescriptor {
  std::string label;
  std::uint32_t width{};
  std::uint32_t height{};
  bool color_aov{true};
  bool depth_aov{true};
};

}  // namespace merlin
