#include <merlin/core/render_world.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
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
    for (std::uint32_t i = 0; i < slots_.size(); ++i) {
      auto& slot = slots_[i];
      if (!slot.value) {
        slot.value = std::move(descriptor);
        slot.revision = 1;
        return MakeHandle<HandleType>(i, slot.generation);
      }
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
};

void ValidateChangeAspects(ObjectKind kind, ChangeAspect aspects) {
  const auto allowed = DefaultChangeAspects(kind);
  const auto bits = static_cast<std::uint32_t>(aspects);
  const auto allowed_bits = static_cast<std::uint32_t>(allowed);
  if (aspects == ChangeAspect::None || (bits & ~allowed_bits) != 0U) {
    throw std::invalid_argument("change aspects do not match resource kind");
  }
}

}  // namespace

class RenderWorld::Impl {
 public:
  template <typename Descriptor, typename HandleType>
  HandleType Create(Store<Descriptor, HandleType>& store, ObjectKind kind,
                    Descriptor descriptor) {
    auto handle = store.Create(std::move(descriptor));
    pending.push_back({kind, ChangeKind::Created, handle.value(),
                       DefaultChangeAspects(kind), store.Revision(handle)});
    return handle;
  }

  template <typename Descriptor, typename HandleType>
  void Update(Store<Descriptor, HandleType>& store, ObjectKind kind,
              HandleType handle, Descriptor descriptor,
              ChangeAspect aspects) {
    ValidateChangeAspects(kind, aspects);
    const auto resource_revision = store.Update(handle, std::move(descriptor));
    pending.push_back({kind, ChangeKind::Updated, handle.value(), aspects,
                       resource_revision});
  }

  template <typename Descriptor, typename HandleType>
  void Remove(Store<Descriptor, HandleType>& store, ObjectKind kind,
              HandleType handle) {
    const auto resource_revision = store.Remove(handle);
    pending.push_back({kind, ChangeKind::Removed, handle.value(),
                       DefaultChangeAspects(kind), resource_revision});
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
  if (descriptor.base_color_texture) {
    (void)impl_->textures.Get(descriptor.base_color_texture->texture);
    (void)impl_->samplers.Get(descriptor.base_color_texture->sampler);
  }
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
                             ChangeAspect aspects) {
  ValidateMesh(d);
  impl_->Update(impl_->meshes, ObjectKind::Mesh, h, std::move(d), aspects);
}
void RenderWorld::UpdateMaterial(MaterialHandle h, MaterialDescriptor d,
                                 ChangeAspect aspects) {
  ValidateMaterialParameters(d);
  if (d.base_color_texture) {
    (void)impl_->textures.Get(d.base_color_texture->texture);
    (void)impl_->samplers.Get(d.base_color_texture->sampler);
  }
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
  std::vector<Change> compact;
  compact.reserve(impl_->pending.size());
  for (const auto& change : impl_->pending) {
    const auto existing = std::find_if(
        compact.begin(), compact.end(), [&](const Change& candidate) {
          return candidate.object_kind == change.object_kind &&
                 candidate.handle == change.handle;
        });
    if (existing == compact.end()) {
      compact.push_back(change);
      continue;
    }
    existing->aspects |= change.aspects;
    existing->resource_revision = change.resource_revision;
    if (existing->change_kind == ChangeKind::Created &&
        change.change_kind == ChangeKind::Removed) {
      compact.erase(existing);
    } else if (existing->change_kind != ChangeKind::Created) {
      existing->change_kind = change.change_kind;
    }
  }
  impl_->pending.clear();
  if (compact.empty()) {
    return {impl_->revision, {}};
  }
  ++impl_->revision;
  return {impl_->revision, std::move(compact)};
}

std::uint64_t RenderWorld::revision() const noexcept { return impl_->revision; }

}  // namespace merlin
