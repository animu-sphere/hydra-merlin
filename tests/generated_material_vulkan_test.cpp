// Executes a MaterialXGenSlang module through renderer-owned Vulkan Forward.
// Requires a Vulkan device and exits 77 when the backend is unavailable.

#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/materialx/compiler.hpp>
#include <merlin/vulkan/renderer.hpp>

#include <cassert>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

std::string ReadFile(const char* path) {
  std::ifstream stream(path, std::ios::binary);
  assert(stream);
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

std::uint8_t CenterChannel(const merlin::vulkan::RenderResult& result,
                           std::uint32_t channel) {
  const auto x = result.color.product.width / 2U;
  const auto y = result.color.product.height / 2U;
  const auto index = static_cast<std::size_t>(y) * result.color.row_pitch_bytes +
                     static_cast<std::size_t>(x) * 4U + channel;
  return result.color.pixels[index];
}

bool Near(std::uint8_t value, std::uint8_t expected,
          std::uint8_t tolerance = 2) {
  const auto difference =
      value > expected ? value - expected : expected - value;
  return difference <= tolerance;
}

void Require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 8) {
    std::cerr << "usage: generated_material_vulkan_test "
                 "SHADER_DIR DOCUMENT DATA_ROOT ARTIFACT_SPV "
                 "STANDARD_DOCUMENT STANDARD_ARTIFACT_SPV ENVIRONMENT\n";
    return 1;
  }

  merlin::materialx::CompileOptions compile_options;
  compile_options.renderable_path = "NG_prototype/out";
  compile_options.library_search_paths.emplace_back(argv[3]);
  compile_options.source_document = "prototype.mtlx";
  const auto compiled = merlin::materialx::CompileMaterialFunction(
      ReadFile(argv[2]), compile_options);
  assert(compiled);
  assert(compiled.diagnostics.empty());
  merlin::materialx::CompileOptions standard_options;
  standard_options.renderable_path = "NG_standard_surface/surface";
  standard_options.library_search_paths.emplace_back(argv[3]);
  standard_options.source_document = "standard-surface.mtlx";
  const auto standard = merlin::materialx::CompileMaterialFunction(
      ReadFile(argv[5]), standard_options);
  assert(standard);
  assert(standard.diagnostics.empty());

  merlin::vulkan::GeneratedMaterialArtifact artifact;
  artifact.module_key = compiled.module->module_key;
  artifact.fragment = argv[4];
  artifact.fragment_entry_point = "merlin_materialx_forward_fragment";
  artifact.parameter_buffer_size = 32;
  artifact.parameter_bindings = {
      {"tint_in1", merlin::MaterialValueType::Float3, 1, 0, 0},
      {"tint_in2", merlin::MaterialValueType::Float3, 1, 16, 0},
  };
  artifact.reflection.target = "spirv";
  artifact.reflection.entry_points = {
      "merlin_materialx_forward_fragment"};
  artifact.reflection.parameters =
      compiled.module->logical_module.parameters;

  merlin::vulkan::GeneratedMaterialArtifact standard_artifact;
  standard_artifact.module_key = standard.module->module_key;
  standard_artifact.fragment = argv[6];
  standard_artifact.fragment_entry_point =
      "merlin_materialx_standard_surface_forward_fragment";
  standard_artifact.parameter_binding = 0;
  standard_artifact.material_constants_binding = 31;
  standard_artifact.parameter_buffer_size = 112;
  standard_artifact.parameter_bindings = {
      {"base", merlin::MaterialValueType::Float, 1, 0, 0},
      {"metalness", merlin::MaterialValueType::Float, 1, 4, 0},
      {"specular_roughness", merlin::MaterialValueType::Float, 1, 8, 0},
      {"uv_index", merlin::MaterialValueType::Integer, 1, 12, 0},
      {"albedo_layer", merlin::MaterialValueType::Integer, 1, 16, 0},
      {"albedo_default", merlin::MaterialValueType::Float3, 1, 32, 0},
      {"albedo_uaddressmode", merlin::MaterialValueType::Integer, 1, 44, 0},
      {"albedo_vaddressmode", merlin::MaterialValueType::Integer, 1, 48, 0},
      {"albedo_filtertype", merlin::MaterialValueType::Integer, 1, 52, 0},
      {"albedo_framerange", merlin::MaterialValueType::Integer, 1, 56, 0},
      {"albedo_frameoffset", merlin::MaterialValueType::Integer, 1, 60, 0},
      {"albedo_frameendaction", merlin::MaterialValueType::Integer, 1, 64, 0},
      {"albedo_uv_scale", merlin::MaterialValueType::Float2, 1, 72, 0},
      {"albedo_uv_offset", merlin::MaterialValueType::Float2, 1, 80, 0},
      {"tinted_albedo_in2", merlin::MaterialValueType::Float3, 1, 96, 0},
  };
  standard_artifact.resource_bindings = {
      {"albedo_file", merlin::MaterialValueType::CombinedTextureSampler,
       1, 1, 2},
  };
  standard_artifact.reflection.target = "spirv";
  standard_artifact.reflection.entry_points = {
      standard_artifact.fragment_entry_point};
  standard_artifact.reflection.parameters =
      standard.module->logical_module.parameters;
  standard_artifact.reflection.resources =
      standard.module->logical_module.resources;

  merlin::vulkan::RendererOptions renderer_options;
  renderer_options.enable_validation = true;
  renderer_options.descriptor_backend =
      merlin::vulkan::DescriptorBackendRequest::Conventional;
  renderer_options.generated_material_artifacts.push_back(artifact);
  renderer_options.generated_material_artifacts.push_back(standard_artifact);
  std::optional<merlin::vulkan::Renderer> renderer;
  try {
    renderer.emplace(std::move(renderer_options));
  } catch (const std::exception& error) {
    std::cerr << "skip: Vulkan renderer unavailable: " << error.what() << '\n';
    return 77;
  }
  assert(renderer->capabilities().generated_materials);

  const std::filesystem::path shader_dir = argv[1];
  const merlin::vulkan::ShaderPaths shaders{
      shader_dir / "triangle.vert.spv", shader_dir / "triangle.frag.spv",
      shader_dir / "triangle.bindless.vert.spv",
      shader_dir / "triangle.bindless.frag.spv", argv[7]};

  merlin::RenderWorld world;
  merlin::extraction::SceneExtractor extractor;
  merlin::MeshDescriptor mesh;
  mesh.label = "generated-material-quad";
  mesh.positions = {{-0.8F, -0.8F, 0.2F}, {0.8F, -0.8F, 0.2F},
                    {0.8F, 0.8F, 0.2F}, {-0.8F, 0.8F, 0.2F}};
  mesh.normals.assign(4, {0.0F, 0.0F, 1.0F});
  mesh.texcoords.assign(4, {0.0F, 0.0F});
  mesh.indices = {0, 1, 2, 0, 2, 3};
  const auto mesh_handle = world.CreateMesh(mesh);

  merlin::MaterialDescriptor material;
  material.label = "generated-prototype";
  material.features = merlin::MaterialFeature::None;
  material.module = compiled.module->logical_module;
  material.generated_parameters = compiled.module->parameter_defaults;
  material.generated_resources.key = compiled.module->resource_key;
  const auto material_handle = world.CreateMaterial(material);
  merlin::InstanceDescriptor instance;
  instance.mesh = mesh_handle;
  instance.material = material_handle;
  const auto instance_handle = world.CreateInstance(instance);

  const auto render = [&] {
    extractor.Apply(world, world.Commit());
    return renderer->Render(*extractor.snapshot(), 64, 64, shaders);
  };

  const auto first = render();
  assert(first.counters.generated_material_draw_count == 1);
  assert(first.counters.generated_material_fallback_count == 0);
  assert(!first.counters.material_fallbacks.fallback_taken());
  assert(first.material_diagnostics.empty());
  // (0.8, 0.25, 0.1) * (0.5, 0.75, 1.0), converted to RGBA8.
  assert(Near(CenterChannel(first, 0), 102));
  assert(Near(CenterChannel(first, 1), 48));
  assert(Near(CenterChannel(first, 2), 26));

  auto& tint =
      material.generated_parameters.entries.at(1).values.at(0);
  tint = merlin::Vec3{0.25F, 0.5F, 0.5F};
  material.generated_parameters.key = "sha256:edited-parameter-state";
  world.UpdateMaterial(material_handle, material,
                       merlin::ChangeAspect::MaterialParameters);
  const auto edited = render();
  assert(edited.counters.pipeline_creation_count == 0);
  assert(edited.counters.generated_material_draw_count == 1);
  assert(Near(CenterChannel(edited, 0), 51));
  assert(Near(CenterChannel(edited, 1), 32));
  assert(Near(CenterChannel(edited, 2), 13));

  const auto render_with_artifact =
      [&](merlin::vulkan::GeneratedMaterialArtifact candidate) {
        merlin::vulkan::RendererOptions options;
        options.descriptor_backend =
            merlin::vulkan::DescriptorBackendRequest::Conventional;
        options.generated_material_artifacts.push_back(std::move(candidate));
        merlin::vulkan::Renderer candidate_renderer(std::move(options));
        return candidate_renderer.Render(*extractor.snapshot(), 64, 64,
                                         shaders);
      };

  auto invalid_layout_artifact = artifact;
  invalid_layout_artifact.parameter_bindings.front().offset =
      invalid_layout_artifact.parameter_buffer_size;
  const auto invalid_layout =
      render_with_artifact(std::move(invalid_layout_artifact));
  Require(invalid_layout.counters.generated_material_draw_count == 0,
          "invalid concrete layout selected a generated pipeline");
  Require(invalid_layout.counters.generated_material_fallback_count == 1,
          "invalid concrete layout did not record a draw fallback");
  Require(invalid_layout.material_diagnostics.size() == 1,
          "invalid concrete layout did not emit one diagnostic");
  Require(invalid_layout.material_diagnostics.front().category ==
              merlin::MaterialDiagnosticCategory::ReflectionMismatch,
          "invalid concrete layout used the wrong diagnostic category");

  const auto corrupt_path =
      std::filesystem::path(argv[4]).parent_path() /
      "materialx-forward-prototype.corrupt.spv";
  {
    const std::array<std::uint32_t, 5> corrupt_code{};
    std::ofstream stream(corrupt_path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(corrupt_code.data()),
                 static_cast<std::streamsize>(sizeof(corrupt_code)));
    Require(static_cast<bool>(stream), "could not write corrupt SPIR-V fixture");
  }
  auto corrupt_artifact = artifact;
  corrupt_artifact.fragment = corrupt_path;
  const auto corrupt = render_with_artifact(std::move(corrupt_artifact));
  std::error_code remove_error;
  std::filesystem::remove(corrupt_path, remove_error);
  Require(corrupt.counters.generated_material_draw_count == 0,
          "corrupt SPIR-V selected a generated pipeline");
  Require(corrupt.counters.generated_material_fallback_count == 1,
          "corrupt SPIR-V did not record a draw fallback");
  Require(corrupt.material_diagnostics.size() == 1,
          "corrupt SPIR-V did not emit one diagnostic");
  Require(corrupt.material_diagnostics.front().category ==
              merlin::MaterialDiagnosticCategory::CacheCorrupt,
          "corrupt SPIR-V used the wrong diagnostic category");

  auto incompatible_artifact = artifact;
  incompatible_artifact.fragment_entry_point =
      "missing_generated_material_entry_point";
  const auto incompatible =
      render_with_artifact(std::move(incompatible_artifact));
  Require(incompatible.counters.generated_material_draw_count == 0,
          "incompatible SPIR-V selected a generated pipeline");
  Require(incompatible.counters.generated_material_fallback_count == 1,
          "incompatible SPIR-V did not record a draw fallback");
  Require(incompatible.material_diagnostics.size() == 1,
          "incompatible SPIR-V did not emit one diagnostic");
  Require(incompatible.material_diagnostics.front().category ==
              merlin::MaterialDiagnosticCategory::TargetFailure,
          "incompatible SPIR-V used the wrong diagnostic category");

  merlin::TextureDescriptor generated_texture;
  generated_texture.label = "generated-checker";
  generated_texture.width = 1;
  generated_texture.height = 1;
  generated_texture.pixels = {128, 64, 255, 255};
  const auto generated_texture_handle =
      world.CreateTexture(generated_texture);
  merlin::SamplerDescriptor generated_sampler;
  generated_sampler.label = "generated-nearest";
  generated_sampler.min_filter = merlin::FilterMode::Nearest;
  generated_sampler.mag_filter = merlin::FilterMode::Nearest;
  const auto generated_sampler_handle =
      world.CreateSampler(generated_sampler);
  merlin::MaterialDescriptor standard_material;
  standard_material.label = "generated-standard-surface";
  standard_material.features = merlin::MaterialFeature::None;
  standard_material.module = standard.module->logical_module;
  standard_material.generated_parameters = standard.module->parameter_defaults;
  standard_material.generated_resources.key = standard.module->resource_key;
  standard_material.generated_resources.entries = {
      {"albedo_file", merlin::MaterialValueType::CombinedTextureSampler,
       {{generated_texture_handle, generated_sampler_handle}}},
  };
  const auto standard_material_handle =
      world.CreateMaterial(standard_material);
  instance.material = standard_material_handle;
  world.UpdateInstance(instance_handle, instance,
                       merlin::ChangeAspect::MaterialBinding);
  const auto textured = render();
  assert(textured.counters.generated_material_draw_count == 1);
  assert(textured.counters.generated_material_fallback_count == 0);
  assert(textured.material_diagnostics.empty());
  // texture * tint * Standard Surface base:
  // (128/255, 64/255, 1) * (0.8, 0.6, 0.4) * 0.9.
  assert(Near(CenterChannel(textured, 0), 92));
  assert(Near(CenterChannel(textured, 1), 35));
  assert(Near(CenterChannel(textured, 2), 92));

  generated_texture.pixels = {255, 128, 64, 255};
  world.UpdateTexture(generated_texture_handle, generated_texture);
  const auto texture_edited = render();
  assert(texture_edited.counters.pipeline_creation_count == 0);
  assert(texture_edited.counters.generated_material_draw_count == 1);
  assert(Near(CenterChannel(texture_edited, 0), 184));
  assert(Near(CenterChannel(texture_edited, 1), 69));
  assert(Near(CenterChannel(texture_edited, 2), 23));

  world.Remove(generated_texture_handle);
  const auto missing_resource = render();
  assert(missing_resource.counters.generated_material_draw_count == 0);
  assert(missing_resource.counters.generated_material_fallback_count == 1);
  assert(missing_resource.counters.material_fallbacks.effective_fallback ==
         merlin::MaterialFallback::BasicMaterial);
  assert(missing_resource.material_diagnostics.size() == 1);
  assert(missing_resource.material_diagnostics.front().category ==
         merlin::MaterialDiagnosticCategory::MissingTexture);

  instance.material = material_handle;
  world.UpdateInstance(instance_handle, instance,
                       merlin::ChangeAspect::MaterialBinding);
  material.module->key = "sha256:missing-vulkan-artifact";
  world.UpdateMaterial(material_handle, material,
                       merlin::ChangeAspect::MaterialModule);
  const auto fallback = render();
  assert(fallback.counters.generated_material_draw_count == 0);
  assert(fallback.counters.generated_material_fallback_count == 1);
  assert(fallback.counters.material_fallbacks.fallback_taken());
  assert(fallback.counters.material_fallbacks.effective_fallback ==
         merlin::MaterialFallback::BasicMaterial);
  assert(fallback.material_diagnostics.size() == 1);
  assert(fallback.material_diagnostics.front().category ==
         merlin::MaterialDiagnosticCategory::CacheIncompatible);

  assert(renderer->statistics().validation_messages == 0);
  return 0;
}
