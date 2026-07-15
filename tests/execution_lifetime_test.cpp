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
#include <utility>
#include <vector>

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
      shader_dir / "triangle.vert.spv", shader_dir / "triangle.frag.spv",
      shader_dir / "triangle.bindless.vert.spv",
      shader_dir / "triangle.bindless.frag.spv"};

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

  auto missing_shader = first;
  missing_shader.shaders.vertex =
      shader_dir / "missing-for-error-classification.vert.spv";
  missing_shader.shaders.bindless_vertex =
      shader_dir / "missing-for-error-classification.bindless.vert.spv";
  bool classified_missing_shader{};
  try {
    (void)renderer->Submit(missing_shader);
  } catch (const merlin::vulkan::RendererError& error) {
    classified_missing_shader =
        IsCode(error, merlin::vulkan::RendererErrorCode::InvalidRequest) &&
        error.operation() == "load SPIR-V shader";
  }
  assert(classified_missing_shader);

  const auto first_token = renderer->Submit(first);
  assert(first_token);
  const auto first_in_flight_statistics = renderer->statistics();

  // Record an edited scene while the first frame can still read the old
  // geometry range. The expanded payload also forces the upload ring to grow;
  // telemetry must retain the old ring generation until its frame completes.
  auto edited_triangle = Triangle(-0.25F);
  edited_triangle.positions.resize(8192);
  edited_triangle.positions[0].x += 0.5F;
  const auto edited_vertex_bytes =
      edited_triangle.positions.size() *
      sizeof(merlin::extraction::DrawVertex);
  assert(edited_vertex_bytes >
         first_in_flight_statistics.upload_ring.capacity_bytes);
  world.UpdateMesh(mesh, std::move(edited_triangle),
                   merlin::ChangeAspect::Points,
                   std::vector<merlin::ElementRange>{{0, 1}});
  extractor.Apply(world, world.Commit());
  merlin::vulkan::RenderRequest second = first;
  second.snapshot = extractor.snapshot();
  second.products = {{merlin::Aov::Color, false},
                     {merlin::Aov::Depth, false}};
  const auto second_token = renderer->Submit(second);
  assert(second_token.value() > first_token.value());
  const auto in_flight_statistics = renderer->statistics();
  assert(in_flight_statistics.pending_geometry_retirements ==
         first_in_flight_statistics.pending_geometry_retirements + 1);
  assert(in_flight_statistics.vertex_arena.retiring_ranges ==
         first_in_flight_statistics.vertex_arena.retiring_ranges + 1);
  assert(in_flight_statistics.vertex_arena.retiring_bytes ==
         first_in_flight_statistics.vertex_arena.retiring_bytes +
             3U * sizeof(merlin::extraction::DrawVertex));
  assert(in_flight_statistics.vertex_arena.active_ranges ==
         first_in_flight_statistics.vertex_arena.active_ranges + 1);
  assert(in_flight_statistics.index_arena.retiring_ranges ==
         first_in_flight_statistics.index_arena.retiring_ranges);
  assert(in_flight_statistics.upload_ring.active_regions == 2);
  assert(in_flight_statistics.upload_ring.in_flight_bytes ==
         first_in_flight_statistics.upload_ring.in_flight_bytes +
             edited_vertex_bytes);
  assert(in_flight_statistics.upload_ring.peak_in_flight_bytes >=
         in_flight_statistics.upload_ring.in_flight_bytes);
  assert(in_flight_statistics.upload_ring.peak_active_regions >= 2);
  assert(in_flight_statistics.upload_ring.growth_count ==
         first_in_flight_statistics.upload_ring.growth_count + 1);
  assert(in_flight_statistics.upload_ring.retired_buffers ==
         first_in_flight_statistics.upload_ring.retired_buffers + 1);

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
  assert(second_result.counters.upload_bytes == edited_vertex_bytes);
  assert(second_result.counters.vertex_upload_bytes ==
         second_result.counters.upload_bytes);
  assert(second_result.counters.index_upload_bytes == 0);
  assert(second_result.counters.geometry_range_reuse_count == 0);
  assert(second_result.counters.buffer_suballocation_count == 1);
  assert(second_result.color.pixels.empty());
  const auto collected_statistics = renderer->statistics();
  assert(collected_statistics.pending_geometry_retirements == 0);
  assert(collected_statistics.vertex_arena.retiring_ranges == 0);
  assert(collected_statistics.vertex_arena.retiring_bytes == 0);
  assert(collected_statistics.vertex_arena.active_ranges == 1);
  assert(collected_statistics.upload_ring.active_regions == 0);
  assert(collected_statistics.upload_ring.in_flight_bytes == 0);

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
