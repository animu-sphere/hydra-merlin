#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/render/specular_environment.hpp>
#include <merlin/vulkan/descriptor_indexing.hpp>
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
  mesh.texcoords = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
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
  std::vector bindings{merlin::render::GpuMaterialBinding{}};
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

  request.forward_lighting.explicit_lights = true;
  merlin::tests::VerifyForwardEmission(render, [&](merlin::Vec3 emission) {
    material.parameters.emissive_radiance = emission;
    world.UpdateMaterial(instance.material, material, merlin::ChangeAspect::MaterialParameters);
    extractor.Apply(world, world.Commit());
    request.snapshot = extractor.snapshot();
  });
  std::cout << mode << ": emissive radiance without lights and output transforms passed\n";

  const auto emissive_texture = world.CreateTexture(merlin::tests::ForwardEmissiveTexture());
  merlin::SamplerDescriptor sampling;
  sampling.min_filter = sampling.mag_filter = merlin::FilterMode::Nearest;
  const auto emissive_sampler = world.CreateSampler(sampling);
  merlin::tests::VerifyForwardEmissiveTexture(render, [&](bool textured, float intensity) {
    const bool changed = material.emissive_texture.has_value() != textured;
    material.parameters.emissive_radiance = {intensity, intensity, intensity};
    if (textured) {
      material.features |= merlin::MaterialFeature::EmissiveTexture;
      material.emissive_texture = merlin::TextureBinding{emissive_texture, emissive_sampler, 0};
      // This is the first sampled image and sampler in this renderer.
      bindings.front().emissive_texture_index = merlin::vulkan::kReservedBindlessTextureSlots;
      bindings.front().emissive_sampler_index = 0;
    } else {
      material.features = merlin::MaterialFeature::DirectionalLight;
      material.emissive_texture.reset();
      bindings.front() = {};
    }
    world.UpdateMaterial(instance.material, material, changed
        ? merlin::ChangeAspect::MaterialParameters | merlin::ChangeAspect::MaterialFeatures
        : merlin::ChangeAspect::MaterialParameters);
    extractor.Apply(world, world.Commit());
    request.snapshot = extractor.snapshot();
  });
  world.Remove(emissive_sampler);
  world.Remove(emissive_texture);
  request.forward_lighting = {};
  std::cout << mode << ": emissive UV0 image, linear decode and live intensity passed\n";

  // A small HDR emitter centered on +Z. A face-on metal reflects its
  // authored RGB at low roughness, even with the panorama hidden.
  merlin::TextureDescriptor hdr;
  hdr.width = 32;
  hdr.height = 16;
  hdr.pixels.resize(hdr.width * hdr.height * 4);
  for (const auto y : {7U, 8U}) {
    for (const auto x : {7U, 8U}) {
      const std::array<std::uint8_t, 4> rgbe{192, 96, 48, 129};
      std::copy(rgbe.begin(), rgbe.end(), hdr.pixels.begin() + (y * hdr.width + x) * 4);
    }
  }
  const auto atlas = world.CreateTexture(merlin::render::BuildSpecularEnvironment(
      hdr.width, hdr.height, hdr.pixels));
  const auto panorama = world.CreateTexture(std::move(hdr));
  merlin::CameraDescriptor camera;
  camera.projection.values[10] = -1.0F;
  camera.projection.values[14] = 1.0F;
  extractor.SetActiveCamera(world.CreateCamera(camera));
  material.features |= merlin::MaterialFeature::ForwardSpecular;
  material.parameters.metallic = 1.0F;
  material.parameters.roughness = 0.045F;
  world.UpdateMaterial(instance.material, material);
  const auto render_specular = [&](float roughness) {
    if (material.parameters.roughness != roughness) {
      material.parameters.roughness = roughness;
      world.UpdateMaterial(instance.material, material);
    }
    extractor.Apply(world, world.Commit());
    request.snapshot = extractor.snapshot();
    return render({0, merlin::render::ToneMapping::None});
  };
  const auto center = [](const auto& frame) {
    const auto offset = 32 * frame.color.row_pitch_bytes + 32 * 4;
    return std::array{frame.color.pixels[offset], frame.color.pixels[offset + 1],
        frame.color.pixels[offset + 2], frame.color.pixels[offset + 3]};
  };
  const auto same_geometry = [](const auto& a, const auto& b) {
    return a.depth.pixels == b.depth.pixels && a.prim_id.pixels == b.prim_id.pixels &&
           a.instance_id.pixels == b.instance_id.pixels;
  };
  request.forward_lighting.explicit_lights = true;
  const auto unlit = render_specular(0.045F);
  auto& dome = request.forward_lighting.dome.emplace();
  dome.rgbe_texture = panorama;
  dome.specular_texture = atlas;
  dome.camera_visible = false;
  const auto sharp = render_specular(0.045F);
  const auto rough = render_specular(0.75F);
  dome.world_to_environment.values[0] = -1.0F;
  dome.world_to_environment.values[10] = -1.0F;
  const auto rotated = render_specular(0.045F);
  dome.world_to_environment = {};
  dome.camera_visible = true;
  const auto visible = render_specular(0.045F);
  dome.camera_visible = false;
  dome.radiance_scale.x = 0.5F;
  const auto half_red = render_specular(0.045F);
  dome.radiance_scale.x = 1.0F;
  const auto restored = render_specular(0.045F);
  const auto rgb = center(sharp);
  if (center(unlit)[0] != 0 || std::abs(int(rgb[0]) - 153) > 2 ||
      std::abs(int(rgb[1]) - 36) > 2 || std::abs(int(rgb[2]) - 10) > 2 ||
      center(rough)[0] == 0 || center(rough)[0] >= rgb[0] / 2 ||
      center(rotated) != center(unlit) || center(visible) != rgb ||
      std::abs(int(center(half_red)[0]) * 2 - rgb[0]) > 2 ||
      center(half_red)[1] != rgb[1] || center(half_red)[2] != rgb[2] ||
      sharp.color.pixels != restored.color.pixels ||
      !same_geometry(unlit, sharp) || !same_geometry(sharp, visible) ||
      !same_geometry(sharp, restored) || restored.counters.upload_bytes != 0 ||
      restored.counters.pipeline_creation_count != 0) {
    std::cerr << "sharp RGB=" << int(rgb[0]) << ',' << int(rgb[1]) << ',' << int(rgb[2])
              << " rough/rotated/half-red=" << int(center(rough)[0]) << '/'
              << int(center(rotated)[0]) << '/' << int(center(half_red)[0])
              << " restore uploads/pipelines=" << restored.counters.upload_bytes
              << '/' << restored.counters.pipeline_creation_count << '\n';
    throw std::runtime_error("Forward HDR dome reflection, filtering, or restoration differs");
  }
  std::cout << mode << ": HDR dome reflection, roughness and orientation passed\n";

  const auto reflection_dome = dome;
  request.forward_lighting.dome.reset();
  const auto directional = world.CreateLight({});
  material.parameters.base_color = {0.0F, 0.0F, 0.0F, 0.75F};
  material.parameters.metallic = 0.0F;
  for (const bool dome_only : {false, true}) {
    if (dome_only) {
      world.Remove(directional);
      request.forward_lighting.dome = reflection_dome;
    }
    const auto sample = [&](float weight, float ior, merlin::Vec3 tint) {
      material.parameters.roughness = dome_only ? 0.045F : 0.4F;
      material.parameters.specular_weight = weight;
      material.parameters.specular_ior = ior;
      material.parameters.specular_color = tint;
      world.UpdateMaterial(instance.material, material, merlin::ChangeAspect::MaterialParameters);
      const auto frame = render_specular(material.parameters.roughness);
      if (!same_geometry(unlit, frame) || center(frame)[3] != center(unlit)[3] ||
          frame.counters.upload_bytes != frame.counters.gpu_scene_upload_bytes ||
          frame.counters.pipeline_creation_count != 0) {
        throw std::runtime_error("specular value edit changed geometry or rebuilt GPU resources");
      }
      return center(frame);
    };
    const merlin::Vec3 white{1.0F, 1.0F, 1.0F};
    const auto full = sample(1.0F, 1.5F, white);
    const auto half = sample(0.5F, 1.5F, white);
    const auto ior = sample(1.0F, 2.0F, white);
    const auto tint = sample(1.0F, 1.5F, {1.0F, 0.0F, 0.0F});
    const auto zero_weight = sample(0.0F, 1.5F, white);
    const auto matched_ior = sample(1.0F, 1.0F, white);
    if (full[0] < 8 || std::abs(2 * int(half[0]) - full[0]) > 2 ||
        std::abs(int(ior[0]) - full[0] * (25.0 / 9.0)) > 4 ||
        tint[0] != full[0] || tint[1] != 0 || tint[2] != 0 ||
        zero_weight != center(unlit) || matched_ior != center(unlit) ||
        sample(1.0F, 1.5F, white) != full) {
      throw std::runtime_error("dielectric specular controls differ from the Fresnel oracle");
    }
  }
  request.forward_lighting = {};
  std::cout << mode << ": dielectric specular weight, IOR and tint passed\n";

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
          // A fresh renderer places the fixture's normal map at the first
          // non-reserved bindless texture slot and the first sampler slot.
          const auto surface_bindings =
              merlin::tests::SurfaceGeometryMaterialBindings(*snapshot,
                  merlin::vulkan::kReservedBindlessTextureSlots, 0);
          request.gpu_scene_update =
              std::make_shared<merlin::render::GpuScenePackedFrameUpdate>(
                  surface_packing->Apply(*snapshot, completion, completion,
                      {placements, identities, surface_bindings}));
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
  if (!debug_view_rejected || validation_messages != 0 ||
      surface.cases != 21)
    return 1;
  std::cout << mode << ": surface geometry cases=" << surface.cases
            << " samples=" << surface.samples
            << " max_channel_error=" << surface.maximum_channel_error << '\n';
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
