#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>

#include <cassert>
#include <cstdint>

namespace {

merlin::MeshDescriptor Triangle() {
  merlin::MeshDescriptor mesh;
  mesh.positions = {{-0.5F, -0.5F, 0.0F}, {0.5F, -0.5F, 0.0F},
                    {0.0F, 0.5F, 0.0F}};
  mesh.colors = {{1.0F, 1.0F, 1.0F, 1.0F},
                 {1.0F, 1.0F, 1.0F, 1.0F},
                 {1.0F, 1.0F, 1.0F, 1.0F}};
  mesh.indices = {0, 1, 2};
  return mesh;
}

}  // namespace

int main() {
  // One million indexed triangles exercise 32-bit topology and packed vertex
  // extraction without requiring three million duplicate points.
  {
    constexpr std::uint32_t columns = 1000;
    constexpr std::uint32_t rows = 500;
    merlin::MeshDescriptor mesh;
    mesh.positions.reserve(static_cast<std::size_t>(columns + 1U) *
                           (rows + 1U));
    for (std::uint32_t y = 0; y <= rows; ++y) {
      for (std::uint32_t x = 0; x <= columns; ++x) {
        mesh.positions.push_back(
            {static_cast<float>(x), static_cast<float>(y), 0.0F});
      }
    }
    mesh.indices.reserve(static_cast<std::size_t>(columns) * rows * 6U);
    for (std::uint32_t y = 0; y < rows; ++y) {
      for (std::uint32_t x = 0; x < columns; ++x) {
        const auto a = y * (columns + 1U) + x;
        const auto b = a + 1U;
        const auto c = a + columns + 1U;
        const auto d = c + 1U;
        mesh.indices.insert(mesh.indices.end(), {a, b, d, a, d, c});
      }
    }
    merlin::RenderWorld world;
    const auto mesh_handle = world.CreateMesh(std::move(mesh));
    const auto material = world.CreateMaterial({});
    merlin::InstanceDescriptor instance;
    instance.mesh = mesh_handle;
    instance.material = material;
    world.CreateInstance(instance);
    merlin::extraction::SceneExtractor extractor;
    extractor.Apply(world, world.Commit());
    const auto snapshot = extractor.snapshot();
    assert(snapshot->geometries.size() == 1);
    assert(snapshot->geometries.front().indices->size() == 3'000'000U);
    assert(snapshot->draws.size() == 1);
  }

  // Ten thousand independently keyed small meshes cover deterministic map and
  // draw-list construction for DCC scenes dominated by small objects.
  {
    constexpr std::uint32_t mesh_count = 10'000;
    merlin::RenderWorld world;
    const auto material = world.CreateMaterial({});
    for (std::uint32_t i = 0; i < mesh_count; ++i) {
      const auto mesh = world.CreateMesh(Triangle());
      merlin::InstanceDescriptor instance;
      instance.mesh = mesh;
      instance.material = material;
      instance.transform.values[12] = static_cast<float>(i);
      world.CreateInstance(instance);
    }
    merlin::extraction::SceneExtractor extractor;
    extractor.Apply(world, world.Commit());
    assert(extractor.snapshot()->geometries.size() == mesh_count);
    assert(extractor.snapshot()->draws.size() == mesh_count);
  }

  // Repeated primvar edits must replace only the vertex payload and preserve
  // the immutable topology payload across every revision.
  {
    merlin::RenderWorld world;
    auto descriptor = Triangle();
    const auto mesh = world.CreateMesh(descriptor);
    merlin::extraction::SceneExtractor extractor;
    extractor.Apply(world, world.Commit());
    auto previous = extractor.snapshot();
    for (std::uint32_t revision = 0; revision < 256; ++revision) {
      descriptor.colors[0].x = static_cast<float>(revision) / 255.0F;
      world.UpdateMesh(mesh, descriptor, merlin::ChangeAspect::Primvars);
      extractor.Apply(world, world.Commit());
      const auto current = extractor.snapshot();
      assert(current->geometries.front().indices ==
             previous->geometries.front().indices);
      assert(current->geometries.front().vertices !=
             previous->geometries.front().vertices);
      previous = current;
    }
    assert(previous->revision == 257);
  }
  return 0;
}
