#include <merlin/core/render_world.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace merlin {
namespace detail {

struct HandleFactory {
  template <typename HandleType>
  static HandleType Make(std::uint64_t value) {
    return HandleType(value);
  }
};

}  // namespace detail
namespace {

constexpr std::uint64_t kIndexMask = 0xffffffffULL;

void ValidateMesh(const MeshDescriptor& descriptor) {
  if (descriptor.positions.empty() || descriptor.indices.empty()) {
    throw std::invalid_argument("mesh requires positions and indices");
  }
  if ((descriptor.indices.size() % 3U) != 0U) {
    throw std::invalid_argument("mesh index count must describe triangles");
  }
  if (std::any_of(descriptor.indices.begin(), descriptor.indices.end(),
                  [&](std::uint32_t index) {
                    return index >= descriptor.positions.size();
                  })) {
    throw std::invalid_argument("mesh index is outside the position array");
  }
  const auto validate_primvar_size = [&](std::size_t size,
                                         const char* name) {
    if (size != 0 && size != descriptor.positions.size()) {
      throw std::invalid_argument(std::string("mesh ") + name +
                                  " count must match positions");
    }
  };
  validate_primvar_size(descriptor.normals.size(), "normal");
  validate_primvar_size(descriptor.colors.size(), "color");
  validate_primvar_size(descriptor.texcoords.size(), "texcoord");
}

void ValidateTexture(const TextureDescriptor& descriptor) {
  if (descriptor.width == 0 || descriptor.height == 0) {
    throw std::invalid_argument("texture extent must be non-zero");
  }
  const auto texel_count = static_cast<std::uint64_t>(descriptor.width) *
                           descriptor.height;
  if (texel_count > std::numeric_limits<std::size_t>::max() / 4U ||
      descriptor.pixels.size() != static_cast<std::size_t>(texel_count * 4U)) {
    throw std::invalid_argument("texture requires tightly packed RGBA8 pixels");
  }
}

bool MaterialValueMatchesType(const MaterialValue& value,
                              MaterialValueType type) {
  switch (type) {
    case MaterialValueType::Float:
      return std::holds_alternative<float>(value);
    case MaterialValueType::Float2:
      return std::holds_alternative<Vec2>(value);
    case MaterialValueType::Float3:
      return std::holds_alternative<Vec3>(value);
    case MaterialValueType::Float4:
      return std::holds_alternative<Vec4>(value);
    case MaterialValueType::Integer:
      return std::holds_alternative<std::int32_t>(value);
    case MaterialValueType::Boolean:
      return std::holds_alternative<bool>(value);
    case MaterialValueType::Texture2D:
    case MaterialValueType::Sampler:
    case MaterialValueType::CombinedTextureSampler:
    case MaterialValueType::Unknown:
      return false;
  }
  return false;
}

bool IsFiniteMaterialValue(const MaterialValue& value) {
  return std::visit(
      [](const auto& typed) {
        using Value = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Value, float>) {
          return std::isfinite(typed);
        } else if constexpr (std::is_same_v<Value, Vec2>) {
          return std::isfinite(typed.x) && std::isfinite(typed.y);
        } else if constexpr (std::is_same_v<Value, Vec3>) {
          return std::isfinite(typed.x) && std::isfinite(typed.y) &&
                 std::isfinite(typed.z);
        } else if constexpr (std::is_same_v<Value, Vec4>) {
          return std::isfinite(typed.x) && std::isfinite(typed.y) &&
                 std::isfinite(typed.z) && std::isfinite(typed.w);
        } else {
          return true;
        }
      },
      value);
}

void ValidateMaterialParameters(const MaterialDescriptor& descriptor) {
  const auto& p = descriptor.parameters;
  const std::array<float, 7> values{p.base_color.x, p.base_color.y,
                                    p.base_color.z, p.base_color.w, p.metallic,
                                    p.roughness, p.alpha_cutoff};
  if (std::any_of(values.begin(), values.end(),
                  [](float value) { return !std::isfinite(value); })) {
    throw std::invalid_argument("material parameters must be finite");
  }
  if (p.metallic < 0.0F || p.metallic > 1.0F || p.roughness < 0.0F ||
      p.roughness > 1.0F || p.alpha_cutoff < 0.0F || p.alpha_cutoff > 1.0F) {
    throw std::invalid_argument(
        "material metallic, roughness, and alpha cutoff must be in [0, 1]");
  }
  constexpr auto supported = MaterialFeature::VertexColor |
                             MaterialFeature::BaseColorTexture |
                             MaterialFeature::DirectionalLight;
  const auto unsupported = static_cast<std::uint32_t>(descriptor.features) &
                           ~static_cast<std::uint32_t>(supported);
  if (unsupported != 0U) {
    throw std::invalid_argument("material feature mask contains unknown bits");
  }
  if (HasMaterialFeature(descriptor.features,
                         MaterialFeature::BaseColorTexture) &&
      !descriptor.base_color_texture) {
    throw std::invalid_argument(
        "base-color texture feature requires a texture binding");
  }
  if (descriptor.base_color_texture &&
      descriptor.base_color_texture->texcoord_set != 0U) {
    throw std::invalid_argument(
        "only texture coordinate set 0 is currently supported");
  }
  if (!descriptor.module) {
    if (!descriptor.generated_parameters.key.empty() ||
        !descriptor.generated_parameters.entries.empty() ||
        !descriptor.generated_resources.key.empty() ||
        !descriptor.generated_resources.entries.empty()) {
      throw std::invalid_argument(
          "generated material state requires a material module");
    }
    return;
  }

  const auto& module = *descriptor.module;
  if (module.key.empty() || module.entry_point.empty() ||
      module.revision == 0U) {
    throw std::invalid_argument(
        "material module requires a key, entry point, and non-zero revision");
  }
  if (module.abi_version != kMaterialAbiVersion ||
      module.reflection_schema_version !=
          kMaterialReflectionSchemaVersion) {
    throw std::invalid_argument(
        "material module ABI or reflection schema is unsupported");
  }
  const auto validate_layout = [](const auto& entries, bool resources) {
    std::unordered_set<std::string> names;
    for (const auto& entry : entries) {
      if (entry.name.empty() || entry.array_size == 0U ||
          entry.type == MaterialValueType::Unknown) {
        throw std::invalid_argument(
            "material module layout entries require a name, type, and "
            "non-zero array size");
      }
      const bool resource_type =
          entry.type == MaterialValueType::Texture2D ||
          entry.type == MaterialValueType::Sampler ||
          entry.type == MaterialValueType::CombinedTextureSampler;
      if (resource_type != resources) {
        throw std::invalid_argument(
            "material module parameter/resource layout type mismatch");
      }
      if (!names.insert(entry.name).second) {
        throw std::invalid_argument(
            "material module layout contains a duplicate name");
      }
    }
  };
  validate_layout(module.parameters.entries, false);
  validate_layout(module.resources.entries, true);

  constexpr auto supported_inputs =
      MaterialInputRequirement::PositionObject |
      MaterialInputRequirement::PositionWorld |
      MaterialInputRequirement::NormalObject |
      MaterialInputRequirement::NormalWorld |
      MaterialInputRequirement::Texcoord0;
  constexpr auto supported_results =
      MaterialResultField::BaseColor | MaterialResultField::Metalness |
      MaterialResultField::SpecularRoughness |
      MaterialResultField::ShadingNormal;
  const auto input_bits = static_cast<std::uint32_t>(
      module.requirements.inputs);
  const auto result_bits = static_cast<std::uint32_t>(
      module.requirements.results);
  if ((input_bits & ~static_cast<std::uint32_t>(supported_inputs)) != 0U ||
      (result_bits & ~static_cast<std::uint32_t>(supported_results)) != 0U) {
    throw std::invalid_argument(
        "material module requirements contain unknown bits");
  }

  if (descriptor.generated_parameters.key.empty() ||
      descriptor.generated_resources.key.empty()) {
    throw std::invalid_argument(
        "generated material state requires parameter and resource keys");
  }

  const auto validate_parameter_state = [&] {
    if (descriptor.generated_parameters.entries.size() !=
        module.parameters.entries.size()) {
      throw std::invalid_argument(
          "generated parameter state does not match the module layout");
    }
    std::unordered_set<std::string> names;
    for (const auto& value : descriptor.generated_parameters.entries) {
      if (!names.insert(value.name).second) {
        throw std::invalid_argument(
            "generated parameter state contains a duplicate name");
      }
      const auto layout = std::find_if(
          module.parameters.entries.begin(), module.parameters.entries.end(),
          [&](const MaterialParameterLayoutEntry& candidate) {
            return candidate.name == value.name;
          });
      if (layout == module.parameters.entries.end() ||
          layout->type != value.type ||
          value.values.size() != layout->array_size ||
          std::any_of(value.values.begin(), value.values.end(),
                      [&](const MaterialValue& element) {
                        return !MaterialValueMatchesType(element, value.type) ||
                               !IsFiniteMaterialValue(element);
                      })) {
        throw std::invalid_argument(
            "generated parameter value does not match the module layout");
      }
    }
  };
  validate_parameter_state();

  if (descriptor.generated_resources.entries.size() !=
      module.resources.entries.size()) {
    throw std::invalid_argument(
        "generated resource state does not match the module layout");
  }
  std::unordered_set<std::string> resource_names;
  for (const auto& binding : descriptor.generated_resources.entries) {
    if (!resource_names.insert(binding.name).second) {
      throw std::invalid_argument(
          "generated resource state contains a duplicate name");
    }
    const auto layout = std::find_if(
        module.resources.entries.begin(), module.resources.entries.end(),
        [&](const MaterialResourceLayoutEntry& candidate) {
          return candidate.name == binding.name;
        });
    const auto valid_binding = [&](const MaterialResourceValue& value) {
      switch (binding.type) {
        case MaterialValueType::Texture2D:
          return value.texture.valid() && !value.sampler.valid();
        case MaterialValueType::Sampler:
          return !value.texture.valid() && value.sampler.valid();
        case MaterialValueType::CombinedTextureSampler:
          return value.texture.valid() && value.sampler.valid();
        case MaterialValueType::Float:
        case MaterialValueType::Float2:
        case MaterialValueType::Float3:
        case MaterialValueType::Float4:
        case MaterialValueType::Integer:
        case MaterialValueType::Boolean:
        case MaterialValueType::Unknown:
          return false;
      }
      return false;
    };
    if (layout == module.resources.entries.end() ||
        layout->type != binding.type ||
        binding.values.size() != layout->array_size ||
        !std::all_of(binding.values.begin(), binding.values.end(),
                     valid_binding)) {
      throw std::invalid_argument(
          "generated resource binding does not match the module layout");
    }
  }
}

template <typename HandleType>
HandleType MakeHandle(std::uint32_t index, std::uint32_t generation) {
  const auto value = (static_cast<std::uint64_t>(generation) << 32U) |
                     (static_cast<std::uint64_t>(index) + 1U);
  return detail::HandleFactory::Make<HandleType>(value);
}

template <typename HandleType>
std::uint32_t HandleIndex(HandleType handle) {
  return static_cast<std::uint32_t>((handle.value() & kIndexMask) - 1U);
}

template <typename HandleType>
std::uint32_t HandleGeneration(HandleType handle) {
  return static_cast<std::uint32_t>(handle.value() >> 32U);
}

template <typename Descriptor, typename HandleType>
class Store {
 public:
  HandleType Create(Descriptor descriptor) {
    if (!free_slots_.empty()) {
      const auto index = free_slots_.back();
      free_slots_.pop_back();
      auto& slot = slots_[index];
      slot.value = std::move(descriptor);
      slot.revision = 1;
      return MakeHandle<HandleType>(index, slot.generation);
    }
    Slot slot;
    slot.value = std::move(descriptor);
    slot.revision = 1;
    slots_.push_back(std::move(slot));
    return MakeHandle<HandleType>(static_cast<std::uint32_t>(slots_.size() - 1U), 1U);
  }

  Descriptor& Get(HandleType handle) {
    return const_cast<Descriptor&>(std::as_const(*this).Get(handle));
  }

  const Descriptor& Get(HandleType handle) const {
    if (!handle.valid()) {
      throw std::invalid_argument("invalid null Merlin handle");
    }
    const auto index = HandleIndex(handle);
    if (index >= slots_.size()) {
      throw std::out_of_range("Merlin handle index is out of range");
    }
    const auto& slot = slots_[index];
    if (!slot.value || slot.generation != HandleGeneration(handle)) {
      throw std::invalid_argument("stale Merlin handle");
    }
    return *slot.value;
  }

  std::uint64_t Update(HandleType handle, Descriptor descriptor) {
    Get(handle) = std::move(descriptor);
    auto& slot = slots_[HandleIndex(handle)];
    return BumpRevision(slot);
  }

  std::uint64_t Remove(HandleType handle) {
    const auto index = HandleIndex(handle);
    (void)Get(handle);
    auto& slot = slots_[index];
    const auto tombstone_revision = BumpRevision(slot);
    slot.value.reset();
    ++slot.generation;
    if (slot.generation == 0) {
      slot.generation = 1;
    }
    free_slots_.push_back(index);
    return tombstone_revision;
  }

  std::uint64_t Revision(HandleType handle) const {
    (void)Get(handle);
    return slots_[HandleIndex(handle)].revision;
  }

 private:
  struct Slot {
    std::uint32_t generation{1};
    std::uint64_t revision{};
    std::optional<Descriptor> value;
  };

  static std::uint64_t BumpRevision(Slot& slot) {
    if (slot.revision == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error("Merlin resource revision overflow");
    }
    return ++slot.revision;
  }

  std::vector<Slot> slots_;
  std::vector<std::uint32_t> free_slots_;
};

void ValidateChangeAspects(ObjectKind kind, ChangeAspect aspects) {
  const auto allowed = DefaultChangeAspects(kind);
  const auto bits = static_cast<std::uint32_t>(aspects);
  const auto allowed_bits = static_cast<std::uint32_t>(allowed);
  if (aspects == ChangeAspect::None || (bits & ~allowed_bits) != 0U) {
    throw std::invalid_argument("change aspects do not match resource kind");
  }
}

std::vector<ElementRange> NormalizeRanges(std::vector<ElementRange> ranges,
                                          std::size_t element_count,
                                          const char* label) {
  for (const auto& range : ranges) {
    if (range.count == 0U || range.first > element_count ||
        range.count > element_count - range.first) {
      throw std::invalid_argument(std::string(label) +
                                  " change range is out of bounds");
    }
  }
  std::sort(ranges.begin(), ranges.end(),
            [](const ElementRange& lhs, const ElementRange& rhs) {
              return lhs.first < rhs.first;
            });
  std::vector<ElementRange> normalized;
  for (const auto& range : ranges) {
    if (normalized.empty()) {
      normalized.push_back(range);
      continue;
    }
    auto& previous = normalized.back();
    const auto previous_end = previous.first + previous.count;
    const auto range_end = range.first + range.count;
    if (range.first <= previous_end) {
      previous.count = std::max(previous_end, range_end) - previous.first;
    } else {
      normalized.push_back(range);
    }
  }
  return normalized;
}

void MergeKnownRanges(std::vector<ElementRange>& destination,
                      const std::vector<ElementRange>& source) {
  destination.insert(destination.end(), source.begin(), source.end());
  std::sort(destination.begin(), destination.end(),
            [](const ElementRange& lhs, const ElementRange& rhs) {
              return lhs.first < rhs.first;
            });
  std::vector<ElementRange> merged;
  for (const auto& range : destination) {
    if (merged.empty()) {
      merged.push_back(range);
      continue;
    }
    auto& previous = merged.back();
    const auto previous_end = previous.first + previous.count;
    const auto range_end = range.first + range.count;
    if (range.first <= previous_end) {
      previous.count = std::max(previous_end, range_end) - previous.first;
    } else {
      merged.push_back(range);
    }
  }
  destination = std::move(merged);
}

}  // namespace

class RenderWorld::Impl {
 public:
  void ValidateMaterialResourceHandles(
      const MaterialDescriptor& descriptor) const {
    if (descriptor.base_color_texture) {
      (void)textures.Get(descriptor.base_color_texture->texture);
      (void)samplers.Get(descriptor.base_color_texture->sampler);
    }
    for (const auto& entry : descriptor.generated_resources.entries) {
      for (const auto& value : entry.values) {
        if (value.texture.valid()) {
          (void)textures.Get(value.texture);
        }
        if (value.sampler.valid()) {
          (void)samplers.Get(value.sampler);
        }
      }
    }
  }

  template <typename Descriptor, typename HandleType>
  HandleType Create(Store<Descriptor, HandleType>& store, ObjectKind kind,
                    Descriptor descriptor) {
    auto handle = store.Create(std::move(descriptor));
    pending.push_back({kind, ChangeKind::Created, handle.value(),
                       DefaultChangeAspects(kind), store.Revision(handle),
                       false, false, {}, {}});
    return handle;
  }

  template <typename Descriptor, typename HandleType>
  void Update(Store<Descriptor, HandleType>& store, ObjectKind kind,
              HandleType handle, Descriptor descriptor,
              ChangeAspect aspects) {
    ValidateChangeAspects(kind, aspects);
    const auto resource_revision = store.Update(handle, std::move(descriptor));
    pending.push_back({kind, ChangeKind::Updated, handle.value(), aspects,
                       resource_revision, false, false, {}, {}});
  }

  template <typename Descriptor, typename HandleType>
  void Remove(Store<Descriptor, HandleType>& store, ObjectKind kind,
              HandleType handle) {
    const auto resource_revision = store.Remove(handle);
    pending.push_back({kind, ChangeKind::Removed, handle.value(),
                       DefaultChangeAspects(kind), resource_revision, false,
                       false, {}, {}});
  }

  Store<MeshDescriptor, MeshHandle> meshes;
  Store<MaterialDescriptor, MaterialHandle> materials;
  Store<TextureDescriptor, TextureHandle> textures;
  Store<SamplerDescriptor, SamplerHandle> samplers;
  Store<InstanceDescriptor, InstanceHandle> instances;
  Store<CameraDescriptor, CameraHandle> cameras;
  Store<LightDescriptor, LightHandle> lights;
  Store<RenderSettingsDescriptor, RenderSettingsHandle> render_settings;
  std::vector<Change> pending;
  std::uint64_t revision{};
};

RenderWorld::RenderWorld() : impl_(std::make_unique<Impl>()) {}
RenderWorld::~RenderWorld() = default;
RenderWorld::RenderWorld(RenderWorld&&) noexcept = default;
RenderWorld& RenderWorld::operator=(RenderWorld&&) noexcept = default;

MeshHandle RenderWorld::CreateMesh(MeshDescriptor descriptor) {
  ValidateMesh(descriptor);
  return impl_->Create(impl_->meshes, ObjectKind::Mesh, std::move(descriptor));
}

MaterialHandle RenderWorld::CreateMaterial(MaterialDescriptor descriptor) {
  ValidateMaterialParameters(descriptor);
  impl_->ValidateMaterialResourceHandles(descriptor);
  return impl_->Create(impl_->materials, ObjectKind::Material, std::move(descriptor));
}

TextureHandle RenderWorld::CreateTexture(TextureDescriptor descriptor) {
  ValidateTexture(descriptor);
  return impl_->Create(impl_->textures, ObjectKind::Texture,
                       std::move(descriptor));
}

SamplerHandle RenderWorld::CreateSampler(SamplerDescriptor descriptor) {
  return impl_->Create(impl_->samplers, ObjectKind::Sampler,
                       std::move(descriptor));
}

InstanceHandle RenderWorld::CreateInstance(InstanceDescriptor descriptor) {
  (void)impl_->meshes.Get(descriptor.mesh);
  (void)impl_->materials.Get(descriptor.material);
  return impl_->Create(impl_->instances, ObjectKind::Instance, std::move(descriptor));
}

CameraHandle RenderWorld::CreateCamera(CameraDescriptor descriptor) {
  return impl_->Create(impl_->cameras, ObjectKind::Camera, std::move(descriptor));
}

LightHandle RenderWorld::CreateLight(LightDescriptor descriptor) {
  return impl_->Create(impl_->lights, ObjectKind::Light, std::move(descriptor));
}

RenderSettingsHandle RenderWorld::CreateRenderSettings(
    RenderSettingsDescriptor descriptor) {
  return impl_->Create(impl_->render_settings, ObjectKind::RenderSettings,
                       std::move(descriptor));
}

void RenderWorld::UpdateMesh(MeshHandle h, MeshDescriptor d,
                             ChangeAspect aspects,
                             std::optional<std::vector<ElementRange>> vertex_ranges,
                             std::optional<std::vector<ElementRange>> index_ranges) {
  ValidateMesh(d);
  const auto& previous = impl_->meshes.Get(h);
  const auto vertex_aspects = ChangeAspect::Points | ChangeAspect::Primvars |
                              ChangeAspect::VertexLayout;
  if (vertex_ranges && !HasAnyAspect(aspects, vertex_aspects)) {
    throw std::invalid_argument(
        "vertex change ranges require a vertex-payload aspect");
  }
  if (index_ranges && !HasAnyAspect(aspects, ChangeAspect::Topology)) {
    throw std::invalid_argument(
        "index change ranges require the topology aspect");
  }
  if (vertex_ranges) {
    const bool shape_changed =
        previous.positions.size() != d.positions.size() ||
        previous.normals.size() != d.normals.size() ||
        previous.colors.size() != d.colors.size() ||
        previous.texcoords.size() != d.texcoords.size();
    if (shape_changed) {
      vertex_ranges.reset();
    } else {
      *vertex_ranges = NormalizeRanges(std::move(*vertex_ranges),
                                       d.positions.size(), "vertex");
    }
  }
  if (index_ranges) {
    if (previous.indices.size() != d.indices.size()) {
      index_ranges.reset();
    } else {
      *index_ranges = NormalizeRanges(std::move(*index_ranges),
                                      d.indices.size(), "index");
    }
  }
  ValidateChangeAspects(ObjectKind::Mesh, aspects);
  const auto resource_revision = impl_->meshes.Update(h, std::move(d));
  impl_->pending.push_back({ObjectKind::Mesh, ChangeKind::Updated, h.value(),
                            aspects, resource_revision,
                            vertex_ranges.has_value(), index_ranges.has_value(),
                            vertex_ranges ? std::move(*vertex_ranges)
                                          : std::vector<ElementRange>{},
                            index_ranges ? std::move(*index_ranges)
                                         : std::vector<ElementRange>{}});
}
void RenderWorld::UpdateMaterial(MaterialHandle h, MaterialDescriptor d,
                                 ChangeAspect aspects) {
  ValidateMaterialParameters(d);
  impl_->ValidateMaterialResourceHandles(d);
  impl_->Update(impl_->materials, ObjectKind::Material, h, std::move(d),
                aspects);
}
void RenderWorld::UpdateTexture(TextureHandle h, TextureDescriptor d,
                                ChangeAspect aspects) {
  ValidateTexture(d);
  impl_->Update(impl_->textures, ObjectKind::Texture, h, std::move(d), aspects);
}
void RenderWorld::UpdateSampler(SamplerHandle h, SamplerDescriptor d,
                                ChangeAspect aspects) {
  impl_->Update(impl_->samplers, ObjectKind::Sampler, h, std::move(d), aspects);
}
void RenderWorld::UpdateInstance(InstanceHandle h, InstanceDescriptor d,
                                 ChangeAspect aspects) {
  (void)impl_->meshes.Get(d.mesh);
  (void)impl_->materials.Get(d.material);
  impl_->Update(impl_->instances, ObjectKind::Instance, h, std::move(d),
                aspects);
}
void RenderWorld::UpdateCamera(CameraHandle h, CameraDescriptor d,
                               ChangeAspect aspects) {
  impl_->Update(impl_->cameras, ObjectKind::Camera, h, std::move(d), aspects);
}
void RenderWorld::UpdateLight(LightHandle h, LightDescriptor d,
                              ChangeAspect aspects) {
  impl_->Update(impl_->lights, ObjectKind::Light, h, std::move(d), aspects);
}
void RenderWorld::UpdateRenderSettings(RenderSettingsHandle h,
                                       RenderSettingsDescriptor d,
                                       ChangeAspect aspects) {
  impl_->Update(impl_->render_settings, ObjectKind::RenderSettings, h,
                std::move(d), aspects);
}

void RenderWorld::Remove(MeshHandle h) { impl_->Remove(impl_->meshes, ObjectKind::Mesh, h); }
void RenderWorld::Remove(MaterialHandle h) { impl_->Remove(impl_->materials, ObjectKind::Material, h); }
void RenderWorld::Remove(TextureHandle h) { impl_->Remove(impl_->textures, ObjectKind::Texture, h); }
void RenderWorld::Remove(SamplerHandle h) { impl_->Remove(impl_->samplers, ObjectKind::Sampler, h); }
void RenderWorld::Remove(InstanceHandle h) { impl_->Remove(impl_->instances, ObjectKind::Instance, h); }
void RenderWorld::Remove(CameraHandle h) { impl_->Remove(impl_->cameras, ObjectKind::Camera, h); }
void RenderWorld::Remove(LightHandle h) { impl_->Remove(impl_->lights, ObjectKind::Light, h); }
void RenderWorld::Remove(RenderSettingsHandle h) {
  impl_->Remove(impl_->render_settings, ObjectKind::RenderSettings, h);
}

const MeshDescriptor& RenderWorld::Get(MeshHandle h) const { return impl_->meshes.Get(h); }
const MaterialDescriptor& RenderWorld::Get(MaterialHandle h) const { return impl_->materials.Get(h); }
const TextureDescriptor& RenderWorld::Get(TextureHandle h) const { return impl_->textures.Get(h); }
const SamplerDescriptor& RenderWorld::Get(SamplerHandle h) const { return impl_->samplers.Get(h); }
const InstanceDescriptor& RenderWorld::Get(InstanceHandle h) const { return impl_->instances.Get(h); }
const CameraDescriptor& RenderWorld::Get(CameraHandle h) const { return impl_->cameras.Get(h); }
const LightDescriptor& RenderWorld::Get(LightHandle h) const { return impl_->lights.Get(h); }
const RenderSettingsDescriptor& RenderWorld::Get(
    RenderSettingsHandle h) const {
  return impl_->render_settings.Get(h);
}

std::uint64_t RenderWorld::resource_revision(MeshHandle h) const {
  return impl_->meshes.Revision(h);
}
std::uint64_t RenderWorld::resource_revision(MaterialHandle h) const {
  return impl_->materials.Revision(h);
}
std::uint64_t RenderWorld::resource_revision(TextureHandle h) const {
  return impl_->textures.Revision(h);
}
std::uint64_t RenderWorld::resource_revision(SamplerHandle h) const {
  return impl_->samplers.Revision(h);
}
std::uint64_t RenderWorld::resource_revision(InstanceHandle h) const {
  return impl_->instances.Revision(h);
}
std::uint64_t RenderWorld::resource_revision(CameraHandle h) const {
  return impl_->cameras.Revision(h);
}
std::uint64_t RenderWorld::resource_revision(LightHandle h) const {
  return impl_->lights.Revision(h);
}
std::uint64_t RenderWorld::resource_revision(RenderSettingsHandle h) const {
  return impl_->render_settings.Revision(h);
}

ChangeSet RenderWorld::Commit() {
  if (impl_->pending.empty()) {
    return {impl_->revision, {}};
  }
  constexpr auto kind_count =
      static_cast<std::size_t>(ObjectKind::RenderSettings) + 1U;
  std::array<std::size_t, kind_count> pending_counts{};
  for (const auto& change : impl_->pending) {
    ++pending_counts[static_cast<std::size_t>(change.object_kind)];
  }
  std::array<std::unordered_map<std::uint64_t, std::size_t>, kind_count>
      compact_indices;
  for (std::size_t kind = 0; kind < kind_count; ++kind) {
    compact_indices[kind].reserve(pending_counts[kind]);
  }
  std::vector<Change> compact;
  compact.reserve(impl_->pending.size());
  std::vector<bool> keep;
  keep.reserve(impl_->pending.size());
  std::size_t kept_count{};
  for (const auto& change : impl_->pending) {
    auto& indices =
        compact_indices[static_cast<std::size_t>(change.object_kind)];
    const auto found = indices.find(change.handle);
    if (found == indices.end()) {
      indices.emplace(change.handle, compact.size());
      compact.push_back(change);
      keep.push_back(true);
      ++kept_count;
      continue;
    }
    auto& existing = compact[found->second];
    const auto old_aspects = existing.aspects;
    const auto vertex_aspects = ChangeAspect::Points | ChangeAspect::Primvars |
                                ChangeAspect::VertexLayout;
    if (HasAnyAspect(change.aspects, vertex_aspects)) {
      if (HasAnyAspect(old_aspects, vertex_aspects)) {
        if (existing.vertex_ranges_known && change.vertex_ranges_known) {
          MergeKnownRanges(existing.vertex_ranges, change.vertex_ranges);
        } else {
          existing.vertex_ranges_known = false;
          existing.vertex_ranges.clear();
        }
      } else {
        existing.vertex_ranges_known = change.vertex_ranges_known;
        existing.vertex_ranges = change.vertex_ranges;
      }
    }
    if (change.HasAspect(ChangeAspect::Topology)) {
      if (HasAnyAspect(old_aspects, ChangeAspect::Topology)) {
        if (existing.index_ranges_known && change.index_ranges_known) {
          MergeKnownRanges(existing.index_ranges, change.index_ranges);
        } else {
          existing.index_ranges_known = false;
          existing.index_ranges.clear();
        }
      } else {
        existing.index_ranges_known = change.index_ranges_known;
        existing.index_ranges = change.index_ranges;
      }
    }
    existing.aspects |= change.aspects;
    existing.resource_revision = change.resource_revision;
    if (existing.change_kind == ChangeKind::Created &&
        change.change_kind == ChangeKind::Removed) {
      keep[found->second] = false;
      indices.erase(found);
      --kept_count;
    } else if (existing.change_kind != ChangeKind::Created) {
      existing.change_kind = change.change_kind;
    }
  }
  impl_->pending.clear();
  if (kept_count == 0U) {
    return {impl_->revision, {}};
  }
  if (kept_count != compact.size()) {
    std::vector<Change> filtered;
    filtered.reserve(kept_count);
    for (std::size_t index = 0; index < compact.size(); ++index) {
      if (keep[index]) {
        filtered.push_back(std::move(compact[index]));
      }
    }
    compact = std::move(filtered);
  }
  ++impl_->revision;
  return {impl_->revision, std::move(compact)};
}

std::uint64_t RenderWorld::revision() const noexcept { return impl_->revision; }

}  // namespace merlin
