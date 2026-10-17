#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/vulkan/renderer.hpp>
#include "forward_output_fixture.hpp"
#include "surface_geometry_fixture.hpp"

#include <iostream>
#include <limits>

int main(int argc, char** argv) try {
  if (argc != 3)
    return 1;
  const std::filesystem::path root = argv[1];
  const std::string mode = argv[2];
  const bool scene = mode == "gpu-scene" || mode == "gpu-driven";
  merlin::vulkan::RendererOptions options;
  options.enable_validation = true;
  options.descriptor_backend = mode == "conventional"
                                   ? merlin::vulkan::DescriptorBackendRequest::Conventional
                                   : merlin::vulkan::DescriptorBackendRequest::Bindless;
  const merlin::render::GpuScenePackingCapacities capacities{1, 1, 1, 1};
  if (scene)
    options.gpu_scene_capacities = capacities;
  std::optional<merlin::vulkan::Renderer> renderer;
  try {
    renderer.emplace(options);
  } catch (const std::exception& error) {
    std::cerr << "skip: " << error.what() << '\n';
    return 77;
  }

  merlin::RenderWorld world;
  merlin::extraction::SceneExtractor extractor;
  merlin::MeshDescriptor mesh;
  mesh.positions = {{-0.7F, -0.7F, 0.5F}, {0.7F, -0.7F, 0.5F},
      {0.7F, 0.7F, 0.5F}, {-0.7F, 0.7F, 0.5F}};
  mesh.normals.assign(4, {0, 0, 1});
  mesh.indices = {0, 1, 2, 0, 2, 3};
  merlin::MaterialDescriptor material;
  material.features = merlin::MaterialFeature::DirectionalLight;
  material.parameters.base_color = {0.4F, 0.1875F, 0.1F, 0.75F};
  merlin::InstanceDescriptor instance;
  instance.mesh = world.CreateMesh(mesh);
  instance.material = world.CreateMaterial(material);
  world.CreateInstance(instance);
  extractor.Apply(world, world.Commit());

  merlin::vulkan::RenderRequest request;
  request.snapshot = extractor.snapshot();
  request.width = request.height = 64;
  request.shaders = {root / "triangle.vert.spv", root / "triangle.frag.spv",
      root / "triangle.bindless.vert.spv", root / "triangle.bindless.frag.spv",
      root / "environment.hdr"};
  request.products = {{merlin::Aov::Color, true}, {merlin::Aov::Depth, true},
      {merlin::Aov::PrimId, true}, {merlin::Aov::InstanceId, true}};
  if (mode == "gpu-driven") {
    request.gpu_driven_indexed.mode = merlin::vulkan::GpuDrivenIndexedMode::Require;
  }
  merlin::render::GpuScenePackingState packing(capacities);
  const std::vector placements{merlin::render::GpuGeometryPlacement{0, 0}};
  const std::vector identities{merlin::render::GpuInstanceIdentity{17, 23}};
  const std::vector bindings{merlin::render::GpuMaterialBinding{}};
  std::uint64_t completion{};
  const auto render = [&](merlin::render::OutputTransformSettings settings) {
    request.output_transform = settings;
    if (scene) {
      request.gpu_scene_update = std::make_shared<merlin::render::GpuScenePackedFrameUpdate>(
          packing.Apply(*request.snapshot, completion, completion,
              {placements, identities, bindings}));
    }
    const auto result = renderer->Resolve(renderer->Submit(request));
    completion = result.completion_value;
    if (scene && result.counters.gpu_scene_draw_count != 1) {
      throw std::runtime_error("output fixture fell back from GPU Scene");
    }
    if (mode == "gpu-driven" && result.counters.gpu_driven_indirect_draw_count != 1) {
      throw std::runtime_error("output fixture fell back from GPU-driven Forward");
    }
    return result;
  };
  merlin::tests::VerifyForwardOutputTransforms(render, material.parameters.base_color);
  const auto before = renderer->statistics().frames_submitted;
  for (const auto invalid : {
           merlin::render::OutputTransformSettings{33, merlin::render::ToneMapping::None},
           merlin::render::OutputTransformSettings{0, merlin::render::ToneMapping::Aces},
           merlin::render::OutputTransformSettings{
               std::numeric_limits<float>::quiet_NaN(), merlin::render::ToneMapping::Reinhard}}) {
    request.output_transform = invalid;
    bool rejected{};
    try {
      (void)renderer->Submit(request);
    } catch (const merlin::vulkan::RendererError& error) {
      rejected = error.code() == merlin::vulkan::RendererErrorCode::InvalidRequest;
    }
    if (!rejected || renderer->statistics().frames_submitted != before) {
      throw std::runtime_error("invalid output settings reached GPU submission");
    }
  }
  (void)render({});
  if (renderer->statistics().validation_messages != 0)
    return 1;
  std::cout << mode << ": output transforms, alpha/depth/IDs, repeats and rejection passed\n";

  // Surface geometry views on the same Forward entry. Each fixture case is a
  // fresh source, so a fresh renderer keeps its geometry at arena offset 0.
  std::uint64_t surface_source{};
  std::optional<merlin::render::GpuScenePackingState> surface_packing;
  std::uint64_t validation_messages{};
  const auto surface = merlin::tests::VerifySurfaceGeometryViews(
      [&](std::shared_ptr<const merlin::extraction::FrameSnapshot> snapshot,
          merlin::render::DebugView view) {
        if (snapshot->source_id != surface_source) {
          validation_messages += renderer->statistics().validation_messages;
          renderer.reset();
          renderer.emplace(options);
          surface_packing.emplace(capacities);
          surface_source = snapshot->source_id;
          completion = 0;
        }
        request.snapshot = snapshot;
        request.output_transform = {};
        request.debug_view = view;
        if (scene) {
          request.gpu_scene_update =
              std::make_shared<merlin::render::GpuScenePackedFrameUpdate>(
                  surface_packing->Apply(*snapshot, completion, completion,
                      {placements, identities, bindings}));
        }
        const auto result = renderer->Resolve(renderer->Submit(request));
        completion = result.completion_value;
        if (scene && result.counters.gpu_scene_draw_count != 1) {
          throw std::runtime_error("surface fixture fell back from GPU Scene");
        }
        if (mode == "gpu-driven" &&
            result.counters.gpu_driven_indirect_draw_count != 1) {
          throw std::runtime_error("surface fixture fell back from GPU-driven Forward");
        }
        return result;
      });
  validation_messages += renderer->statistics().validation_messages;
  request.debug_view = merlin::render::DebugView::Depth;
  bool debug_view_rejected{};
  try {
    (void)renderer->Submit(request);
  } catch (const merlin::vulkan::RendererError& error) {
    debug_view_rejected =
        error.code() == merlin::vulkan::RendererErrorCode::InvalidRequest;
  }
  if (!debug_view_rejected || validation_messages != 0)
    return 1;
  std::cout << mode << ": surface geometry cases=" << surface.cases
            << " samples=" << surface.samples
            << " max_channel_error=" << surface.maximum_channel_error << '\n';
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
