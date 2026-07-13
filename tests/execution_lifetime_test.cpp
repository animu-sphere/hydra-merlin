// Exercises the v0.4.0 request/submission/completion/resolve contract. Requires
// a Vulkan device; exits 77 (CTest skip) when the renderer cannot be created.

#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/vulkan/renderer.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>

namespace {

merlin::MeshDescriptor Triangle(float x) {
  merlin::MeshDescriptor mesh;
  mesh.positions = {{x - 0.35F, -0.5F, 0.2F},
                    {x + 0.35F, 0.5F, 0.2F},
                    {x - 0.35F, 0.5F, 0.2F}};
  mesh.indices = {0, 1, 2};
  return mesh;
}

bool IsCode(const merlin::vulkan::RendererError& error,
            merlin::vulkan::RendererErrorCode code) {
  return error.code() == code;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: execution_lifetime_test SHADER_DIR\n";
    return 1;
  }
  const std::filesystem::path shader_dir = argv[1];
  const merlin::vulkan::ShaderPaths shaders{
      shader_dir / "triangle.vert.spv", shader_dir / "triangle.frag.spv"};

  std::optional<merlin::vulkan::Renderer> renderer;
  try {
    renderer.emplace(merlin::vulkan::RendererOptions{false, 2});
  } catch (const std::exception& error) {
    std::cerr << "skip: Vulkan renderer unavailable: " << error.what() << '\n';
    return 77;
  }

  merlin::RenderWorld world;
  merlin::extraction::SceneExtractor extractor;
  const auto mesh = world.CreateMesh(Triangle(-0.25F));
  const auto material = world.CreateMaterial({});
  merlin::InstanceDescriptor instance;
  instance.mesh = mesh;
  instance.material = material;
  world.CreateInstance(instance);
  extractor.Apply(world, world.Commit());

  merlin::vulkan::RenderRequest first;
  first.snapshot = extractor.snapshot();
  first.width = 64;
  first.height = 64;
  first.shaders = shaders;
  first.products = {{merlin::Aov::Color, true},
                    {merlin::Aov::Depth, true},
                    {merlin::Aov::PrimId, true}};
  auto unsupported = first;
  unsupported.products = {{merlin::Aov::Normal, true}};
  bool classified_unsupported{};
  try {
    (void)renderer->Submit(unsupported);
  } catch (const merlin::vulkan::RendererError& error) {
    classified_unsupported =
        IsCode(error, merlin::vulkan::RendererErrorCode::Unsupported);
  }
  assert(classified_unsupported);

  const auto first_token = renderer->Submit(first);
  assert(first_token);

  // Record an edited scene while the first frame can still read the old
  // geometry range. The backend must version the range instead of overwriting
  // it in place.
  world.UpdateMesh(mesh, Triangle(0.25F), merlin::ChangeAspect::Points);
  extractor.Apply(world, world.Commit());
  merlin::vulkan::RenderRequest second = first;
  second.snapshot = extractor.snapshot();
  second.products = {{merlin::Aov::Color, false},
                     {merlin::Aov::Depth, false}};
  const auto second_token = renderer->Submit(second);
  assert(second_token.value() > first_token.value());

  bool busy{};
  try {
    (void)renderer->Submit(second);
  } catch (const merlin::vulkan::RendererError& error) {
    busy = IsCode(error, merlin::vulkan::RendererErrorCode::ResourceBusy);
  }
  assert(busy);

  const auto second_result = renderer->Resolve(
      second_token, std::chrono::nanoseconds::max());
  assert(second_result.scene_revision == second.snapshot->revision);
  assert(second_result.cpu_readback_aovs.empty());
  assert(second_result.counters.readback_bytes == 0);
  assert(second_result.color.pixels.empty());

  assert(renderer->IsComplete(first_token));
  const auto first_result = renderer->Resolve(first_token);
  assert(first_result.scene_revision == first.snapshot->revision);
  assert(merlin::vulkan::HasCpuReadback(first_result, merlin::Aov::Color));
  assert(!first_result.color.pixels.empty());
  assert(!first_result.depth.pixels.empty());
  assert(!first_result.prim_id.pixels.empty());

  bool consumed{};
  try {
    (void)renderer->Resolve(first_token);
  } catch (const merlin::vulkan::RendererError& error) {
    consumed = IsCode(error, merlin::vulkan::RendererErrorCode::InvalidToken);
  }
  assert(consumed);

  std::cout << "execution lifetime contract verified: first="
            << first_token.value() << " second=" << second_token.value()
            << '\n';
  return 0;
}
