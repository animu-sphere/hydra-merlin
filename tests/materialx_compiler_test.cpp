#include <merlin/materialx/compiler.hpp>
#include <merlin/core/render_world.hpp>

#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

std::string ReadFile(const char* path) {
  std::ifstream stream(path, std::ios::binary);
  assert(stream);
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

bool HasDiagnostic(const merlin::materialx::CompileResult& result,
                   merlin::materialx::DiagnosticCode code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  assert(argc == 4);
  const auto document = ReadFile(argv[1]);
  merlin::materialx::CompileOptions options;
  options.renderable_path = "NG_prototype/out";
  options.library_search_paths.emplace_back(argv[2]);

  const auto first =
      merlin::materialx::CompileMaterialFunction(document, options);
  const auto second =
      merlin::materialx::CompileMaterialFunction(document, options);
  assert(first);
  assert(second);
  assert(first.diagnostics.empty());
  assert(first.module->entry_point == "evaluateMaterial");
  assert(first.module->output_type == "color3");
  assert(first.module->source.find(
             "float3 evaluateMaterial(MaterialInputs inputs)") !=
         std::string::npos);
  assert(first.module->source.find("fragmentMain") == std::string::npos);
  assert(first.module->source.find("shader(\"fragment\")") ==
         std::string::npos);
  assert(first.module->source.find("SV_Target") == std::string::npos);
  assert(first.module->cache_key == second.module->cache_key);
  assert(first.module->module_key == first.module->cache_key);
  assert(first.module->instance_key == second.module->instance_key);
  assert(first.module->resource_key == second.module->resource_key);
  assert(first.module->source == second.module->source);
  assert(first.module->cache_key.starts_with("sha256:"));
  assert(first.module->cache_key.size() == 71U);
  assert(!first.module->materialx_version.empty());
  assert(!first.module->generator_version.empty());
  assert(first.module->generator_revision ==
         "38368ee04da84ce1f8837ecba7322dd6d81291f8");
  assert(first.module->logical_module.key == first.module->module_key);
  assert(first.module->logical_module.entry_point == "evaluateMaterial");
  assert(first.module->logical_module.abi_version ==
         merlin::kMaterialAbiVersion);
  assert(first.module->logical_module.reflection_schema_version ==
         merlin::kMaterialReflectionSchemaVersion);
  assert(first.module->logical_module.requirements.results ==
         merlin::MaterialResultField::BaseColor);
  assert(first.module->parameter_defaults.key == first.module->instance_key);
  assert(first.module->parameter_defaults.entries.size() == 2U);
  assert(first.module->resource_defaults.key == first.module->resource_key);
  assert(first.module->resource_defaults.entries.empty());
  const auto& first_tint = std::get<merlin::Vec3>(
      first.module->parameter_defaults.entries[0].values[0]);
  assert(first_tint.y == 0.25F);
  if (!first.module->inputs.empty() || first.module->uniforms.size() != 2U ||
      first.module->uniforms[0].block != "PublicUniforms" ||
      first.module->uniforms[0].variable != "tint_in1" ||
      first.module->uniforms[1].block != "PublicUniforms" ||
      first.module->uniforms[1].variable != "tint_in2") {
    std::cerr << "Unexpected MaterialX logical reflection: "
              << first.module->inputs.size() << " inputs, "
              << first.module->uniforms.size() << " uniforms\n";
    for (const auto& uniform : first.module->uniforms) {
      std::cerr << "  " << uniform.block << ':' << uniform.variable << '\n';
    }
    return 1;
  }
  {
    std::ofstream generated(argv[3], std::ios::binary);
    assert(generated);
    generated << first.module->source;
  }

  auto changed_document = document;
  const auto value = changed_document.find("0.25");
  assert(value != std::string::npos);
  changed_document.replace(value, 4U, "0.35");
  const auto changed =
      merlin::materialx::CompileMaterialFunction(changed_document, options);
  assert(changed);
  assert(changed.module->module_key == first.module->module_key);
  assert(changed.module->cache_key == first.module->cache_key);
  assert(changed.module->source == first.module->source);
  assert(changed.module->instance_key != first.module->instance_key);
  assert(changed.module->resource_key == first.module->resource_key);
  assert(changed.module->logical_module.parameters.entries.size() == 2U);
  assert(changed.module->logical_module.resources.entries.empty());
  const auto& changed_tint = std::get<merlin::Vec3>(
      changed.module->parameter_defaults.entries[0].values[0]);
  assert(changed_tint.y == 0.35F);

  merlin::RenderWorld roundtrip_world;
  merlin::MaterialDescriptor roundtrip_material;
  roundtrip_material.module = first.module->logical_module;
  roundtrip_material.generated_parameters = first.module->parameter_defaults;
  roundtrip_material.generated_resources.key =
      first.module->resource_defaults.key;
  const auto roundtrip_handle =
      roundtrip_world.CreateMaterial(roundtrip_material);
  (void)roundtrip_world.Commit();
  roundtrip_material.generated_parameters =
      changed.module->parameter_defaults;
  roundtrip_world.UpdateMaterial(
      roundtrip_handle, roundtrip_material,
      merlin::ChangeAspect::MaterialParameters);
  const auto roundtrip_changes = roundtrip_world.Commit();
  assert(roundtrip_changes.changes.size() == 1U);
  const auto& roundtrip_tint = std::get<merlin::Vec3>(
      roundtrip_world.Get(roundtrip_handle)
          .generated_parameters.entries[0].values[0]);
  assert(roundtrip_tint.y == 0.35F);

  constexpr auto texcoord1_document = R"mtlx(<?xml version="1.0"?>
<materialx version="1.39">
  <nodegraph name="NG_texcoord1">
    <texcoord name="uv" type="vector2">
      <input name="index" type="integer" value="1" />
    </texcoord>
    <output name="out" type="vector2" nodename="uv" />
  </nodegraph>
</materialx>)mtlx";
  options.renderable_path = "NG_texcoord1/out";
  const auto texcoord1 = merlin::materialx::CompileMaterialFunction(
      texcoord1_document, options);
  assert(!texcoord1);
  assert(HasDiagnostic(texcoord1,
                       merlin::materialx::DiagnosticCode::UnsupportedInput));

  constexpr auto world_normal_document = R"mtlx(<?xml version="1.0"?>
<materialx version="1.39">
  <nodegraph name="NG_world_normal">
    <normal name="normal" type="vector3">
      <input name="space" type="string" value="world" />
    </normal>
    <output name="out" type="vector3" nodename="normal" />
  </nodegraph>
</materialx>)mtlx";
  options.renderable_path = "NG_world_normal/out";
  const auto world_normal = merlin::materialx::CompileMaterialFunction(
      world_normal_document, options);
  assert(world_normal);
  assert(world_normal.module->logical_module.requirements.inputs ==
         merlin::MaterialInputRequirement::NormalWorld);

  constexpr auto unsupported_document = R"mtlx(<?xml version="1.0"?>
<materialx version="1.39">
  <nodegraph name="NG_unsupported">
    <noise3d name="noise" type="color3" />
    <output name="out" type="color3" nodename="noise" />
  </nodegraph>
</materialx>)mtlx";
  options.renderable_path = "NG_unsupported/out";
  const auto unsupported = merlin::materialx::CompileMaterialFunction(
      unsupported_document, options);
  assert(!unsupported);
  assert(HasDiagnostic(unsupported,
                       merlin::materialx::DiagnosticCode::UnsupportedNode));

  options.renderable_path = "does/not/exist";
  const auto missing =
      merlin::materialx::CompileMaterialFunction(document, options);
  assert(!missing);
  assert(HasDiagnostic(
      missing, merlin::materialx::DiagnosticCode::RenderableNotFound));

  const auto malformed = merlin::materialx::CompileMaterialFunction(
      "<materialx>", options);
  assert(!malformed);
  assert(HasDiagnostic(malformed,
                       merlin::materialx::DiagnosticCode::InvalidDocument));
  assert(!HasDiagnostic(
      malformed, merlin::materialx::DiagnosticCode::GenerationFailure));
}
