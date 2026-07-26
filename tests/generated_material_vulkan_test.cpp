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
  if (argc != 6) {
    std::cerr << "usage: generated_material_vulkan_test "
                 "SHADER_DIR DOCUMENT DATA_ROOT ARTIFACT_SPV ENVIRONMENT\n";
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

  merlin::vulkan::GeneratedMaterialArtifact artifact;
  artifact.module_key = compiled.module->module_key;
  artifact.fragment = argv[4];
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

  merlin::vulkan::RendererOptions renderer_options;
  renderer_options.enable_validation = true;
  renderer_options.descriptor_backend =
      merlin::vulkan::DescriptorBackendRequest::Conventional;
  renderer_options.generated_material_artifacts.push_back(artifact);
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
      shader_dir / "triangle.bindless.frag.spv", argv[5]};

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
  world.CreateInstance(instance);

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
