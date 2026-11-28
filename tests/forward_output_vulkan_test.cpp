#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/render/specular_environment.hpp>
#include <merlin/vulkan/descriptor_indexing.hpp>
#include <merlin/vulkan/renderer.hpp>
#include "forward_output_fixture.hpp"
#include "surface_geometry_fixture.hpp"

#include <iostream>
#include <limits>
#include <numbers>

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
  const merlin::render::GpuScenePackingCapacities capacities{1, 2, 2, 2};
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
  std::vector identities{merlin::render::GpuInstanceIdentity{17, 23}};
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
    if (scene && result.counters.gpu_scene_draw_count != request.snapshot->draws.size()) {
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
  request.forward_lighting = {};
  // Reuse the first resident image and sampler to preserve the table indices.
  world.UpdateTexture(emissive_texture, merlin::tests::ForwardOpacityTexture());
  merlin::tests::VerifyForwardOpacityTexture(render, [&](bool textured, float cutoff) {
    const bool changed = material.opacity_texture.has_value() != textured;
    material.parameters.alpha_cutoff = cutoff;
    if (textured) {
      material.features |= merlin::MaterialFeature::OpacityTexture;
      material.alpha_mode = merlin::AlphaMode::Masked;
      material.opacity_texture = merlin::TextureBinding{emissive_texture, emissive_sampler, 0};
      bindings.front().opacity_texture_index = merlin::vulkan::kReservedBindlessTextureSlots;
      bindings.front().opacity_sampler_index = 0;
    } else {
      material.features = merlin::MaterialFeature::DirectionalLight;
      material.alpha_mode = merlin::AlphaMode::Opaque;
      material.opacity_texture.reset();
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
  std::cout << mode << ": emissive and opacity UV0 images passed\n";

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
  std::cout << mode << ": dielectric specular weight, IOR and tint passed\n";

  // Rotate the HDR emitter from the reflection direction (+Z) to the
  // transmission direction (-Z). At normal incidence its RGB is
  // (1.5, 0.75, 0.375), which the tint below equalizes to 0.375.
  auto& transmission_dome = *request.forward_lighting.dome;
  transmission_dome.world_to_environment.values[0] = -1.0F;
  transmission_dome.world_to_environment.values[10] = -1.0F;
  const auto sample_transmission = [&](float weight, float roughness, float ior) {
    material.parameters.transmission_weight = weight;
    material.parameters.transmission_color = {0.25F, 0.5F, 1.0F};
    material.parameters.specular_ior = ior;
    material.parameters.roughness = roughness;
    world.UpdateMaterial(instance.material, material, merlin::ChangeAspect::MaterialParameters);
    const auto frame = render_specular(roughness);
    if (!same_geometry(unlit, frame) || center(frame)[3] != center(unlit)[3] ||
        frame.counters.upload_bytes != frame.counters.gpu_scene_upload_bytes ||
        frame.counters.pipeline_creation_count != 0) {
      throw std::runtime_error("transmission edit changed coverage or rebuilt GPU resources");
    }
    return center(frame);
  };
  const auto no_transmission = sample_transmission(0.0F, 0.045F, 1.5F);
  const auto transmitted = sample_transmission(1.0F, 0.045F, 1.5F);
  const auto half_transmitted = sample_transmission(0.5F, 0.045F, 1.5F);
  const auto rough_transmitted = sample_transmission(1.0F, 0.75F, 1.5F);
  const auto matched = sample_transmission(1.0F, 0.045F, 1.0F);
  for (int channel = 0; channel < 3; ++channel) {
    if (no_transmission[channel] != 0 ||
        std::abs(int(transmitted[channel]) - 92) > 2 ||
        std::abs(int(half_transmitted[channel]) - 46) > 2 ||
        std::abs(int(matched[channel]) - 96) > 2 ||
        rough_transmitted[channel] == 0 || rough_transmitted[channel] >= transmitted[channel] / 2) {
      throw std::runtime_error("dome transmission differs from the tint and Fresnel oracle");
    }
  }
  // A tilted normal gives a 60-degree incident angle. Place the emitter at
  // the refracted direction predicted by Snell's law for IOR 1.5.
  constexpr double incident = std::numbers::pi / 3.0;
  const auto bend = incident - std::asin(std::sin(incident) / 1.5);
  const auto cosine = static_cast<float>(-std::cos(bend));
  const auto sine = static_cast<float>(std::sin(bend));
  transmission_dome.world_to_environment.values[0] = cosine;
  transmission_dome.world_to_environment.values[2] = -sine;
  transmission_dome.world_to_environment.values[8] = sine;
  transmission_dome.world_to_environment.values[10] = cosine;
  mesh.normals.assign(4, {static_cast<float>(std::sin(incident)), 0.0F, 0.5F});
  world.UpdateMesh(instance.mesh, mesh);
  (void)render_specular(0.045F);
  const auto bent = sample_transmission(1.0F, 0.045F, 1.5F);
  const auto straight = sample_transmission(1.0F, 0.045F, 1.0F);
  if (std::abs(int(bent[0]) - 89) > 2 || straight != no_transmission) {
    throw std::runtime_error("dome transmission did not follow the refracted ray");
  }
  request.forward_lighting.dome.reset();
  if (sample_transmission(1.0F, 0.045F, 1.5F) != no_transmission) {
    throw std::runtime_error("disabled dome still illuminated transmission");
  }
  request.forward_lighting = {};
  std::cout << mode << ": dome transmission, tint, Fresnel and roughness passed\n";

  // Share geometry with an emissive backdrop. With exposure and Reinhard in
  // the request, the expected result depends on capturing radiance before
  // clipping and applying the output transform once, after tint and Fresnel.
  request.forward_lighting.explicit_lights = true;
  mesh.normals.assign(4, {0.0F, 0.0F, 1.0F});
  world.UpdateMesh(instance.mesh, mesh);
  merlin::MaterialDescriptor backdrop_material;
  backdrop_material.features = merlin::MaterialFeature::DirectionalLight;
  backdrop_material.parameters.emissive_radiance = {4.0F, 0.5F, 0.0F};
  merlin::InstanceDescriptor backdrop;
  backdrop.mesh = instance.mesh;
  backdrop.material = world.CreateMaterial(backdrop_material);
  backdrop.transform.values[14] = -0.25F;
  const auto backdrop_instance = world.CreateInstance(backdrop);
  identities.push_back({29, 31});
  bindings.emplace_back();
  const auto render_scene_refraction = [&] {
    extractor.Apply(world, world.Commit());
    request.snapshot = extractor.snapshot();
    return render({-1, merlin::render::ToneMapping::Reinhard});
  };
  const auto refracted_scene = render_scene_refraction();
  const auto refracted_rgb = center(refracted_scene);
  if (!same_geometry(unlit, refracted_scene) || refracted_rgb[3] != center(unlit)[3] ||
      std::abs(int(refracted_rgb[0]) - 83) > 2 || std::abs(int(refracted_rgb[1]) - 27) > 2 ||
      refracted_rgb[2] != 0) {
    throw std::runtime_error("scene transmission differs from the HDR tint and depth/ID oracle");
  }
  backdrop.transform.values[0] = 0.07F;
  backdrop.transform.values[12] = -0.1F;
  world.UpdateInstance(backdrop_instance, backdrop);
  mesh.normals.assign(4, {static_cast<float>(std::sin(incident)), 0.0F, 0.5F});
  world.UpdateMesh(instance.mesh, mesh);
  const auto bent_scene = render_scene_refraction();
  material.parameters.specular_ior = 1.0F;
  world.UpdateMaterial(instance.material, material, merlin::ChangeAspect::MaterialParameters);
  const auto straight_scene = render_scene_refraction();
  if (center(bent_scene)[0] < 70 || center(straight_scene) != no_transmission ||
      !same_geometry(bent_scene, straight_scene) ||
      straight_scene.counters.pipeline_creation_count != 0) {
    throw std::runtime_error("scene transmission did not follow the refracted ray or reuse its pipeline");
  }
  std::cout << mode << ": opaque scene refraction, HDR color, miss and AOVs passed\n";
  identities.pop_back();

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
