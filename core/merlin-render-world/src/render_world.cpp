#include <merlin/core/render_world.hpp>

#include <cstddef>
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
        return MakeHandle<HandleType>(i, slot.generation);
      }
    }
    Slot slot;
    slot.value = std::move(descriptor);
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

  void Update(HandleType handle, Descriptor descriptor) {
    Get(handle) = std::move(descriptor);
  }

  void Remove(HandleType handle) {
    const auto index = HandleIndex(handle);
    (void)Get(handle);
    auto& slot = slots_[index];
    slot.value.reset();
    ++slot.generation;
    if (slot.generation == 0) {
      slot.generation = 1;
    }
  }

 private:
  struct Slot {
    std::uint32_t generation{1};
    std::optional<Descriptor> value;
  };
  std::vector<Slot> slots_;
};

}  // namespace

class RenderWorld::Impl {
 public:
  template <typename Descriptor, typename HandleType>
  HandleType Create(Store<Descriptor, HandleType>& store, ObjectKind kind,
                    Descriptor descriptor) {
    auto handle = store.Create(std::move(descriptor));
    pending.push_back({kind, ChangeKind::Created, handle.value()});
    return handle;
  }

  template <typename Descriptor, typename HandleType>
  void Update(Store<Descriptor, HandleType>& store, ObjectKind kind,
              HandleType handle, Descriptor descriptor) {
    store.Update(handle, std::move(descriptor));
    pending.push_back({kind, ChangeKind::Updated, handle.value()});
  }

  template <typename Descriptor, typename HandleType>
  void Remove(Store<Descriptor, HandleType>& store, ObjectKind kind,
              HandleType handle) {
    store.Remove(handle);
    pending.push_back({kind, ChangeKind::Removed, handle.value()});
  }

  Store<MeshDescriptor, MeshHandle> meshes;
  Store<MaterialDescriptor, MaterialHandle> materials;
  Store<InstanceDescriptor, InstanceHandle> instances;
  Store<CameraDescriptor, CameraHandle> cameras;
  Store<LightDescriptor, LightHandle> lights;
  std::vector<Change> pending;
  std::uint64_t revision{};
};

RenderWorld::RenderWorld() : impl_(std::make_unique<Impl>()) {}
RenderWorld::~RenderWorld() = default;
RenderWorld::RenderWorld(RenderWorld&&) noexcept = default;
RenderWorld& RenderWorld::operator=(RenderWorld&&) noexcept = default;

MeshHandle RenderWorld::CreateMesh(MeshDescriptor descriptor) {
  if (descriptor.positions.empty() || descriptor.indices.empty()) {
    throw std::invalid_argument("mesh requires positions and indices");
  }
  return impl_->Create(impl_->meshes, ObjectKind::Mesh, std::move(descriptor));
}

MaterialHandle RenderWorld::CreateMaterial(MaterialDescriptor descriptor) {
  return impl_->Create(impl_->materials, ObjectKind::Material, std::move(descriptor));
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

void RenderWorld::UpdateMesh(MeshHandle h, MeshDescriptor d) { impl_->Update(impl_->meshes, ObjectKind::Mesh, h, std::move(d)); }
void RenderWorld::UpdateMaterial(MaterialHandle h, MaterialDescriptor d) { impl_->Update(impl_->materials, ObjectKind::Material, h, std::move(d)); }
void RenderWorld::UpdateInstance(InstanceHandle h, InstanceDescriptor d) {
  (void)impl_->meshes.Get(d.mesh);
  (void)impl_->materials.Get(d.material);
  impl_->Update(impl_->instances, ObjectKind::Instance, h, std::move(d));
}
void RenderWorld::UpdateCamera(CameraHandle h, CameraDescriptor d) { impl_->Update(impl_->cameras, ObjectKind::Camera, h, std::move(d)); }
void RenderWorld::UpdateLight(LightHandle h, LightDescriptor d) { impl_->Update(impl_->lights, ObjectKind::Light, h, std::move(d)); }

void RenderWorld::Remove(MeshHandle h) { impl_->Remove(impl_->meshes, ObjectKind::Mesh, h); }
void RenderWorld::Remove(MaterialHandle h) { impl_->Remove(impl_->materials, ObjectKind::Material, h); }
void RenderWorld::Remove(InstanceHandle h) { impl_->Remove(impl_->instances, ObjectKind::Instance, h); }
void RenderWorld::Remove(CameraHandle h) { impl_->Remove(impl_->cameras, ObjectKind::Camera, h); }
void RenderWorld::Remove(LightHandle h) { impl_->Remove(impl_->lights, ObjectKind::Light, h); }

const MeshDescriptor& RenderWorld::Get(MeshHandle h) const { return impl_->meshes.Get(h); }
const MaterialDescriptor& RenderWorld::Get(MaterialHandle h) const { return impl_->materials.Get(h); }
const InstanceDescriptor& RenderWorld::Get(InstanceHandle h) const { return impl_->instances.Get(h); }
const CameraDescriptor& RenderWorld::Get(CameraHandle h) const { return impl_->cameras.Get(h); }
const LightDescriptor& RenderWorld::Get(LightHandle h) const { return impl_->lights.Get(h); }

ChangeSet RenderWorld::Commit() {
  if (impl_->pending.empty()) {
    return {impl_->revision, {}};
  }
  ++impl_->revision;
  ChangeSet result{impl_->revision, std::move(impl_->pending)};
  impl_->pending.clear();
  return result;
}

std::uint64_t RenderWorld::revision() const noexcept { return impl_->revision; }

}  // namespace merlin
