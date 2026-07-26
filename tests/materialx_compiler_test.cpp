#include <merlin/materialx/compiler.hpp>
#include <merlin/materialx/diagnostic_bridge.hpp>
#include <merlin/core/identity.hpp>
#include <merlin/core/material_diagnostic.hpp>
#include <merlin/core/render_world.hpp>
#include <merlin/core/shader_artifact.hpp>

#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

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

class CollectingDiagnosticSink final : public merlin::DiagnosticSink {
 public:
  void Report(const merlin::Diagnostic& diagnostic) override {
    reported.push_back(diagnostic);
  }

  std::vector<merlin::Diagnostic> reported;
};

const merlin::materialx::Diagnostic& FindDiagnostic(
    const merlin::materialx::CompileResult& result,
    merlin::materialx::DiagnosticCode code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return diagnostic;
    }
  }
  assert(false && "expected diagnostic code was not reported");
  return result.diagnostics.front();
}

bool HasDependency(
    const std::vector<merlin::materialx::MaterialDependencyFingerprint>&
        dependencies,
    const std::string& suffix) {
  for (const auto& dependency : dependencies) {
    if (dependency.path.ends_with(suffix) &&
        dependency.content_sha256.size() == 64U) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  assert(argc == 6);
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
  assert(first.module->module_key == second.module->module_key);
  assert(first.module->instance_key == second.module->instance_key);
  assert(first.module->resource_key == second.module->resource_key);
  assert(first.module->source == second.module->source);
  assert(merlin::IsIdentity(first.module->module_key));
  assert(merlin::IsIdentity(first.module->instance_key));
  assert(merlin::IsIdentity(first.module->resource_key));
  assert(!first.module->materialx_version.empty());
  assert(!first.module->generator_version.empty());
  assert(first.module->generator_revision ==
         "38368ee04da84ce1f8837ecba7322dd6d81291f8");
  assert(first.module->standard_library_fingerprint.starts_with("sha256:"));
  assert(first.module->source_dependency_fingerprint.starts_with("sha256:"));
  assert(first.module->standard_library_fingerprint ==
         second.module->standard_library_fingerprint);
  assert(first.module->source_dependency_fingerprint ==
         second.module->source_dependency_fingerprint);
  assert(HasDependency(first.module->standard_library_dependencies,
                       "libraries/stdlib/stdlib_defs.mtlx"));
  assert(HasDependency(first.module->source_dependencies,
                       "libraries/stdlib/genslang/lib/mx_math.slang"));
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

  constexpr auto image_document = R"mtlx(<?xml version="1.0"?>
<materialx version="1.39">
  <nodegraph name="NG_image">
    <texcoord name="uv" type="vector2" />
    <image name="albedo" type="color3">
      <input name="file" type="filename" value="checker.png" />
      <input name="texcoord" type="vector2" nodename="uv" />
    </image>
    <output name="out" type="color3" nodename="albedo" />
  </nodegraph>
</materialx>)mtlx";
  options.renderable_path = "NG_image/out";
  const auto image = merlin::materialx::CompileMaterialFunction(
      image_document, options);
  if (!image) {
    for (const auto& diagnostic : image.diagnostics) {
      std::cerr << "Image generation failed at " << diagnostic.element_path
                << ": " << diagnostic.message << '\n';
    }
  }
  assert(image);
  assert(image.diagnostics.empty());
  assert(image.module->logical_module.requirements.inputs ==
         merlin::MaterialInputRequirement::Texcoord0);
  assert(image.module->logical_module.requirements.results ==
         merlin::MaterialResultField::BaseColor);
  assert(image.module->logical_module.resources.entries.size() == 1U);
  assert(image.module->logical_module.resources.entries[0].type ==
         merlin::MaterialValueType::CombinedTextureSampler);
  assert(image.module->resource_defaults.entries.size() == 1U);
  assert(image.module->resource_defaults.entries[0].values.size() == 1U);
  assert(image.module->resource_defaults.entries[0].values[0] ==
         "checker.png");

  auto changed_image_document = std::string(image_document);
  const auto filename = changed_image_document.find("checker.png");
  assert(filename != std::string::npos);
  changed_image_document.replace(filename, 11U, "changed.png");
  const auto changed_image = merlin::materialx::CompileMaterialFunction(
      changed_image_document, options);
  assert(changed_image);
  assert(changed_image.module->source == image.module->source);
  assert(changed_image.module->module_key == image.module->module_key);
  assert(changed_image.module->instance_key == image.module->instance_key);
  assert(changed_image.module->resource_key != image.module->resource_key);

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

  const auto standard_surface_document = ReadFile(argv[4]);
  options.renderable_path = "NG_standard_surface/surface";
  const auto standard_surface = merlin::materialx::CompileMaterialFunction(
      standard_surface_document, options);
  if (!standard_surface) {
    for (const auto& diagnostic : standard_surface.diagnostics) {
      std::cerr << "Standard Surface generation failed at "
                << diagnostic.element_path << ": " << diagnostic.message
                << '\n';
    }
    return 1;
  }
  assert(standard_surface.diagnostics.empty());
  assert(standard_surface.module->output_type == "material_result");
  assert(standard_surface.module->source.find(
             "MaterialResult evaluateMaterial(MaterialInputs inputs)") !=
         std::string::npos);
  assert(standard_surface.module->source.find("fragmentMain") ==
         std::string::npos);
  assert(standard_surface.module->source.find("LightData") ==
         std::string::npos);
  assert(standard_surface.module->source.find("SV_Position") ==
         std::string::npos);
  assert(standard_surface.module->source.find("tangentWorld") ==
         std::string::npos);
  assert(standard_surface.module->source.find("positionWorld") ==
         std::string::npos);
  assert(standard_surface.module->logical_module.requirements.inputs ==
         (merlin::MaterialInputRequirement::Texcoord0 |
          merlin::MaterialInputRequirement::NormalWorld));
  assert(standard_surface.module->logical_module.requirements.results ==
         (merlin::MaterialResultField::BaseColor |
          merlin::MaterialResultField::Metalness |
          merlin::MaterialResultField::SpecularRoughness |
          merlin::MaterialResultField::ShadingNormal));
  assert(standard_surface.module->logical_module.resources.entries.size() ==
         1U);
  assert(standard_surface.module->resource_defaults.entries.size() == 1U);
  assert(standard_surface.module->resource_defaults.entries[0].values[0] ==
         "checker.png");

  auto changed_standard_surface_document = standard_surface_document;
  const auto metalness = changed_standard_surface_document.find("0.35");
  assert(metalness != std::string::npos);
  changed_standard_surface_document.replace(metalness, 4U, "0.55");
  const auto changed_standard_surface =
      merlin::materialx::CompileMaterialFunction(
          changed_standard_surface_document, options);
  assert(changed_standard_surface);
  assert(changed_standard_surface.module->source ==
         standard_surface.module->source);
  assert(changed_standard_surface.module->module_key ==
         standard_surface.module->module_key);
  assert(changed_standard_surface.module->instance_key !=
         standard_surface.module->instance_key);
  assert(changed_standard_surface.module->resource_key ==
         standard_surface.module->resource_key);
  {
    std::ofstream generated(argv[5], std::ios::binary);
    assert(generated);
    generated << standard_surface.module->source;
  }

  // The generated module carries a target-neutral identity, so it enters the
  // same artifact key contract that keys handwritten Slang. Two targets, one
  // module: only the artifact key moves.
  merlin::ShaderArtifactKeyInputs artifact;
  artifact.module_identity = standard_surface.module->module_key;
  artifact.entry_point = standard_surface.module->entry_point;
  artifact.stage = "fragment";
  artifact.permutation = "materialx-standard-surface";
  artifact.abi_version = standard_surface.module->logical_module.abi_version;
  artifact.policy.compiler_version = "2026.8";
  artifact.policy.matrix_layout = "column-major";
  artifact.policy.optimization = "O2";
  artifact.policy.target = "spirv";
  artifact.policy.profile = "sm_6_6";
  artifact.policy.capabilities = "spirv_1_5";
  const auto spirv_key = merlin::MakeShaderArtifactKey(artifact);
  auto metal_artifact = artifact;
  metal_artifact.policy.target = "metal";
  metal_artifact.policy.profile = "metallib_2_4";
  metal_artifact.policy.capabilities = "none";
  const auto metal_key = merlin::MakeShaderArtifactKey(metal_artifact);
  assert(merlin::IsIdentity(spirv_key));
  assert(merlin::IsIdentity(metal_key));
  assert(spirv_key != metal_key);
  assert(merlin::IsIdentity(standard_surface.module->module_key));

  // A parameter-only edit already leaves the module key alone; it must also
  // leave every target artifact alone, or retinting would recompile.
  auto reparameterized = artifact;
  reparameterized.module_identity =
      changed_standard_surface.module->module_key;
  assert(merlin::MakeShaderArtifactKey(reparameterized) == spirv_key);

  constexpr auto unsupported_standard_surface_document =
      R"mtlx(<?xml version="1.0"?>
<materialx version="1.39">
  <standard_surface name="surface" type="surfaceshader">
    <input name="coat" type="float" value="0.5" />
  </standard_surface>
</materialx>)mtlx";
  options.renderable_path = "surface";
  const auto unsupported_standard_surface =
      merlin::materialx::CompileMaterialFunction(
          unsupported_standard_surface_document, options);
  assert(!unsupported_standard_surface);
  assert(HasDiagnostic(unsupported_standard_surface,
                       merlin::materialx::DiagnosticCode::UnsupportedInput));

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

  // An image node the document never gave a filename produces a resource no
  // host adapter could resolve, so it is diagnosed here rather than surfacing
  // later as an unexplained missing texture.
  constexpr auto missing_texture_document = R"mtlx(<?xml version="1.0"?>
<materialx version="1.39">
  <nodegraph name="NG_missing_texture">
    <image name="albedo" type="color3" />
    <output name="out" type="color3" nodename="albedo" />
  </nodegraph>
</materialx>)mtlx";
  options.renderable_path = "NG_missing_texture/out";
  const auto missing_texture = merlin::materialx::CompileMaterialFunction(
      missing_texture_document, options);
  assert(!missing_texture);
  assert(HasDiagnostic(missing_texture,
                       merlin::materialx::DiagnosticCode::MissingTexture));
  const auto& texture_diagnostic = FindDiagnostic(
      missing_texture, merlin::materialx::DiagnosticCode::MissingTexture);
  // Detected against reflected interface, so the input can be named but the
  // authored node cannot. The category stays empty rather than naming the
  // renderable, which is not the node that failed.
  assert(!texture_diagnostic.input_name.empty());
  assert(texture_diagnostic.node_category.empty());

  // Integration-local records carry the context a host needs to locate the
  // failure, and the bridge classifies them against the Core contract without
  // either side learning the other's types.
  options.renderable_path = "NG_unsupported/out";
  options.source_document = "materials/unsupported.mtlx";
  const auto unsupported_with_source =
      merlin::materialx::CompileMaterialFunction(unsupported_document, options);
  assert(!unsupported_with_source);
  assert(unsupported_with_source.source_document ==
         "materials/unsupported.mtlx");
  // The MaterialX version is known before anything is generated, so even a
  // document that never reached the generator reports which library read it.
  assert(!unsupported_with_source.materialx_version.empty());
  const auto& node_diagnostic = FindDiagnostic(
      unsupported_with_source,
      merlin::materialx::DiagnosticCode::UnsupportedNode);
  assert(node_diagnostic.node_category == "noise3d");
  assert(node_diagnostic.element_path.find("noise") != std::string::npos);

  const auto bridged = merlin::materialx::ToMaterialDiagnostic(
      node_diagnostic, unsupported_with_source);
  assert(bridged.category ==
         merlin::MaterialDiagnosticCategory::UnsupportedNode);
  assert(bridged.severity == merlin::DiagnosticSeverity::Error);
  assert(bridged.context.node_category == "noise3d");
  assert(bridged.context.source_document == "materials/unsupported.mtlx");
  // This document never reached the generator, so the version reported is the
  // library that read it, and it says so rather than leaving a bare number a
  // host would have to guess the origin of.
  assert(bridged.context.generator_version ==
         "MaterialX/" + unsupported_with_source.materialx_version);
  // No module survived, so nothing was simplified. Reporting a simplification
  // here would claim MaterialX coverage that was never generated.
  assert(bridged.fallback == merlin::MaterialFallback::BasicMaterial);

  CollectingDiagnosticSink sink;
  const auto evidence = merlin::materialx::ReportCompileDiagnostics(
      unsupported_with_source, sink);
  assert(sink.reported.size() == unsupported_with_source.diagnostics.size());
  assert(!sink.reported.empty());
  assert(sink.reported.front().schema_version ==
         merlin::kDiagnosticSchemaVersion);
  assert(sink.reported.front().code == "material.node.unsupported");
  assert(sink.reported.front().recovery == "basic-material");
  assert(sink.reported.front().disposition ==
         merlin::DiagnosticDisposition::Fallback);
  assert(sink.reported.front().message.find("node=noise3d") !=
         std::string::npos);
  assert(sink.reported.front().message.find(
             "document=materials/unsupported.mtlx") != std::string::npos);
  assert(evidence.fallback_taken());
  assert(evidence.effective_fallback ==
         merlin::MaterialFallback::BasicMaterial);
  assert(evidence.basic_material_count == sink.reported.size());

  // A host that forbids substituting materials still gets a classified record,
  // and the explicit error material instead of a silent approximation.
  CollectingDiagnosticSink strict_sink;
  const merlin::MaterialFallbackPolicy strict{false, false};
  const auto strict_evidence = merlin::materialx::ReportCompileDiagnostics(
      unsupported_with_source, strict_sink, strict);
  assert(strict_evidence.effective_fallback ==
         merlin::MaterialFallback::ErrorMaterial);
  assert(strict_sink.reported.front().recovery == "error-material");

  // A warning is not a failure. Nothing was substituted, so the record claims
  // no rung of the ladder and reaches the host as an ignored record rather than
  // as a rejection or a fallback. The compiler emits only errors today; the
  // bridge must not turn the first warning it is handed into a recovery the
  // renderer never performed.
  const merlin::materialx::Diagnostic advisory{
      merlin::materialx::DiagnosticSeverity::Warning,
      merlin::materialx::DiagnosticCode::UnsupportedInput, "NG_prototype/mix",
      "authored input was ignored"};
  const auto bridged_advisory =
      merlin::materialx::ToMaterialDiagnostic(advisory, standard_surface);
  assert(bridged_advisory.severity == merlin::DiagnosticSeverity::Warning);
  assert(bridged_advisory.fallback == merlin::MaterialFallback::None);
  assert(merlin::ToDiagnostic(bridged_advisory).disposition ==
         merlin::DiagnosticDisposition::Ignored);
  // A module in hand reports the generator that produced it, told apart from
  // the library version a pre-generation failure reports.
  assert(bridged_advisory.context.generator_version ==
         "MaterialXGenSlang/" + standard_surface.generator_version);
  assert(bridged_advisory.context.material_identity ==
         standard_surface.module->module_key);

  // A generator that emitted part of a render pass produced a module no
  // renderer may compose, so the record is a generation failure and costs the
  // whole material. The compiler checks its own output for this against the
  // Core rule; the case cannot be reached from a document, so what is pinned
  // here is that the code classifies and bridges like the failure it is.
  const merlin::materialx::Diagnostic contaminated{
      merlin::materialx::DiagnosticSeverity::Error,
      merlin::materialx::DiagnosticCode::GeneratedPassDeclaration,
      "NG_prototype/out",
      "Generated material source declares a shader entry point at line 1; "
      "the renderer owns the pass"};
  assert(merlin::materialx::ToMaterialDiagnosticCategory(
             contaminated.code) ==
         merlin::MaterialDiagnosticCategory::GenerationFailure);
  const auto bridged_contamination =
      merlin::materialx::ToMaterialDiagnostic(contaminated,
                                              unsupported_with_source);
  assert(merlin::ToDiagnostic(bridged_contamination).code ==
         "material.generation.failed");
  assert(bridged_contamination.fallback ==
         merlin::MaterialFallback::BasicMaterial);

  // A clean compile reports nothing and claims no fallback, so evidence never
  // shows a recovery the renderer did not perform.
  CollectingDiagnosticSink clean_sink;
  const auto clean_evidence =
      merlin::materialx::ReportCompileDiagnostics(standard_surface, clean_sink);
  assert(clean_sink.reported.empty());
  assert(!clean_evidence.fallback_taken());
  assert(clean_evidence.effective_fallback == merlin::MaterialFallback::None);
  assert(clean_evidence.recorded_count == 0U);
}
