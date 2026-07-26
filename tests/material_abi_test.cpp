// The material ABI contract decides whether a renderer may draw with a
// generated module at all: whether the module produces what the consumer reads
// and needs only what it can supply, whether a compiled artifact reports the
// interface the module declared, and whether the generated source stayed on its
// side of the boundary and declared no part of the render pass.
//
// A check that never rejects anything is worse than no check, so every case
// here pins both directions: the agreeing input reports nothing, and a single
// spoiled field reports exactly one record with the right category and context.
//
// It links only Merlin::RenderWorld, so it runs in a MaterialX-disabled build
// as well; the rule belongs to Core, not to any one producer.

#include <merlin/core/material_abi.hpp>
#include <merlin/core/material_diagnostic.hpp>
#include <merlin/core/types.hpp>

#include <array>
#include <cassert>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using merlin::MaterialAbiExpectation;
using merlin::MaterialDiagnostic;
using merlin::MaterialDiagnosticCategory;
using merlin::MaterialFallback;
using merlin::MaterialFallbackPolicy;
using merlin::MaterialInputRequirement;
using merlin::MaterialModule;
using merlin::MaterialResultField;
using merlin::MaterialTargetReflection;
using merlin::MaterialValueType;

bool Mentions(const std::vector<MaterialDiagnostic>& records,
              std::string_view fragment) {
  for (const auto& record : records) {
    if (record.message.find(fragment) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// A minimum Standard Surface module: the shape the v0.10.0 slice generates.
MaterialModule MakeModule() {
  MaterialModule module;
  module.key = "sha256:module";
  module.parameters.entries = {
      {"base", MaterialValueType::Float, 1},
      {"metalness", MaterialValueType::Float, 1},
      {"tint", MaterialValueType::Float3, 1},
  };
  module.resources.entries = {
      {"albedo_file", MaterialValueType::CombinedTextureSampler, 1},
  };
  module.requirements.inputs =
      MaterialInputRequirement::Texcoord0 | MaterialInputRequirement::NormalWorld;
  module.requirements.results =
      MaterialResultField::BaseColor | MaterialResultField::Metalness |
      MaterialResultField::SpecularRoughness |
      MaterialResultField::ShadingNormal;
  return module;
}

// What a renderer-owned artifact that composed this module reports: the
// renderer's entry point, and the material's own block laid out by the target.
MaterialTargetReflection MakeReflection(const MaterialModule& module,
                                        std::string target) {
  MaterialTargetReflection reflection;
  reflection.target = std::move(target);
  reflection.entry_points = {"forward_fragment"};
  reflection.parameters = module.parameters;
  reflection.resources = module.resources;
  return reflection;
}

void VerifyNamesAreStableAndDistinct() {
  constexpr std::array kTypes{
      MaterialValueType::Float,     MaterialValueType::Float2,
      MaterialValueType::Float3,    MaterialValueType::Float4,
      MaterialValueType::Integer,   MaterialValueType::Boolean,
      MaterialValueType::Texture2D, MaterialValueType::Sampler,
      MaterialValueType::CombinedTextureSampler,
  };
  std::set<std::string_view> names;
  for (const auto type : kTypes) {
    const auto name = merlin::MaterialValueTypeName(type);
    assert(name != "unknown");
    assert(names.insert(name).second);
  }
  assert(merlin::MaterialValueTypeName(MaterialValueType::Unknown) ==
         "unknown");

  constexpr std::array kResults{
      MaterialResultField::BaseColor, MaterialResultField::Metalness,
      MaterialResultField::SpecularRoughness,
      MaterialResultField::ShadingNormal};
  names.clear();
  for (const auto field : kResults) {
    assert(names.insert(merlin::MaterialResultFieldName(field)).second);
  }
  constexpr std::array kInputs{MaterialInputRequirement::PositionObject,
                               MaterialInputRequirement::PositionWorld,
                               MaterialInputRequirement::NormalObject,
                               MaterialInputRequirement::NormalWorld,
                               MaterialInputRequirement::Texcoord0};
  names.clear();
  for (const auto input : kInputs) {
    assert(names.insert(merlin::MaterialInputRequirementName(input)).second);
  }

  // A combination names no single thing, so it is reported as unknown rather
  // than as whichever flag happened to be listed first.
  assert(merlin::MaterialResultFieldName(MaterialResultField::BaseColor |
                                         MaterialResultField::Metalness) ==
         "unknown");
  assert(merlin::MaterialInputRequirementName(
             MaterialInputRequirement::Texcoord0 |
             MaterialInputRequirement::NormalWorld) == "unknown");
}

void VerifyModuleAgainstConsumer() {
  const auto module = MakeModule();
  assert(merlin::VerifyMaterialAbi(module).empty());

  // A consumer that reads less than the module produces is still satisfied;
  // the extra fields simply go unread.
  MaterialAbiExpectation base_color_only;
  base_color_only.required_results = MaterialResultField::BaseColor;
  assert(merlin::VerifyMaterialAbi(module, base_color_only).empty());

  // A module that produces less than the consumer reads cannot drive it.
  auto color_only_module = module;
  color_only_module.requirements.results = MaterialResultField::BaseColor;
  MaterialAbiExpectation shading;
  shading.required_results =
      MaterialResultField::BaseColor | MaterialResultField::Metalness;
  const auto missing_result =
      merlin::VerifyMaterialAbi(color_only_module, shading);
  assert(missing_result.size() == 1U);
  assert(missing_result[0].category == MaterialDiagnosticCategory::AbiMismatch);
  assert(Mentions(missing_result, "'metalness'"));
  assert(missing_result[0].context.material_identity == module.key);
  // A result field is not an input; naming it as one would point a host at the
  // wrong half of the ABI.
  assert(missing_result[0].context.input_name.empty());
  assert(missing_result[0].context.node_category.empty());

  // A module needing an input the consumer cannot construct has no way to be
  // evaluated, whatever it would have produced.
  MaterialAbiExpectation no_uvs;
  no_uvs.available_inputs = MaterialInputRequirement::NormalWorld;
  const auto missing_input = merlin::VerifyMaterialAbi(module, no_uvs);
  assert(missing_input.size() == 1U);
  assert(missing_input[0].category == MaterialDiagnosticCategory::AbiMismatch);
  assert(missing_input[0].context.input_name == "texcoord-0");

  auto stale = module;
  stale.abi_version = merlin::kMaterialAbiVersion + 1;
  stale.reflection_schema_version =
      merlin::kMaterialReflectionSchemaVersion + 1;
  stale.entry_point = "evaluate";
  const auto stale_records = merlin::VerifyMaterialAbi(stale);
  assert(stale_records.size() == 3U);
  for (const auto& record : stale_records) {
    assert(record.category == MaterialDiagnosticCategory::AbiMismatch);
  }
  assert(Mentions(stale_records, "material ABI version"));
  assert(Mentions(stale_records, "reflection schema version"));
  assert(Mentions(stale_records, "'evaluate'"));

  // An ABI failure has no partial graph to simplify toward, so it descends to
  // the basic material, and to the error material when that is not permitted.
  assert(stale_records[0].fallback == MaterialFallback::BasicMaterial);
  const MaterialFallbackPolicy strict{true, false};
  const auto strict_records = merlin::VerifyMaterialAbi(stale, {}, strict);
  assert(strict_records[0].fallback == MaterialFallback::ErrorMaterial);
}

void VerifyTargetReflectionAgreement() {
  const auto module = MakeModule();
  assert(merlin::VerifyMaterialTargetReflection(
             module, MakeReflection(module, "spirv"))
             .empty());

  // Agreement is semantic, not positional: a target is free to lay the block
  // out however its own ABI requires.
  auto reordered = MakeReflection(module, "metal");
  std::swap(reordered.parameters.entries[0], reordered.parameters.entries[2]);
  assert(merlin::VerifyMaterialTargetReflection(module, reordered).empty());

  auto dropped = MakeReflection(module, "spirv");
  dropped.parameters.entries.erase(dropped.parameters.entries.begin() + 1);
  const auto dropped_records =
      merlin::VerifyMaterialTargetReflection(module, dropped);
  assert(dropped_records.size() == 1U);
  assert(dropped_records[0].category ==
         MaterialDiagnosticCategory::ReflectionMismatch);
  assert(dropped_records[0].context.backend_target == "spirv");
  // Detected against a compiled interface, so there is a reflected name but no
  // authored node to attribute it to.
  assert(dropped_records[0].context.input_name == "metalness");
  assert(dropped_records[0].context.node_category.empty());
  assert(dropped_records[0].context.element_path.empty());

  auto retyped = MakeReflection(module, "metal");
  retyped.parameters.entries[2].type = MaterialValueType::Float4;
  const auto retyped_records =
      merlin::VerifyMaterialTargetReflection(module, retyped);
  assert(retyped_records.size() == 1U);
  assert(Mentions(retyped_records, "as float4"));
  assert(Mentions(retyped_records, "declares float3"));

  auto resized = MakeReflection(module, "spirv");
  resized.parameters.entries[0].array_size = 4;
  const auto resized_records =
      merlin::VerifyMaterialTargetReflection(module, resized);
  assert(resized_records.size() == 1U);
  assert(Mentions(resized_records, "array size 4"));

  // A uniform the module never declared is material state the consumer never
  // agreed to own, which is how a renderer-owned block leaks into one.
  auto leaked = MakeReflection(module, "spirv");
  leaked.parameters.entries.push_back(
      {"light_direction", MaterialValueType::Float4, 1});
  const auto leaked_records =
      merlin::VerifyMaterialTargetReflection(module, leaked);
  assert(leaked_records.size() == 1U);
  assert(Mentions(leaked_records, "'light_direction'"));
  assert(Mentions(leaked_records, "does not declare"));

  auto duplicated = MakeReflection(module, "spirv");
  duplicated.parameters.entries.push_back(duplicated.parameters.entries[0]);
  const auto duplicated_records =
      merlin::VerifyMaterialTargetReflection(module, duplicated);
  assert(duplicated_records.size() == 1U);
  assert(Mentions(duplicated_records, "more than once"));

  auto missing_resource = MakeReflection(module, "metal");
  missing_resource.resources.entries.clear();
  const auto resource_records =
      merlin::VerifyMaterialTargetReflection(module, missing_resource);
  assert(resource_records.size() == 1U);
  assert(Mentions(resource_records, "resource 'albedo_file'"));

  // A material that reached the artifact as its own entry point was not
  // composed into the renderer's pass; it became one.
  auto promoted = MakeReflection(module, "spirv");
  promoted.entry_points.push_back(module.entry_point);
  const auto promoted_records =
      merlin::VerifyMaterialTargetReflection(module, promoted);
  assert(promoted_records.size() == 1U);
  assert(promoted_records[0].category ==
         MaterialDiagnosticCategory::AbiMismatch);
  assert(Mentions(promoted_records, "renderer-owned one"));

  auto headless = MakeReflection(module, "metal");
  headless.entry_points.clear();
  const auto headless_records =
      merlin::VerifyMaterialTargetReflection(module, headless);
  assert(headless_records.size() == 1U);
  assert(Mentions(headless_records, "reports no entry point"));
}

void VerifySourcePassNeutrality() {
  constexpr std::string_view kMaterialSource = R"(
struct MaterialInputs
{
    float2 texcoord_0;
    float3 normalWorld;
    // A field merely named after a system value is not one: the module reads it
    // as ordinary data, and never binds it. We do not discard here either.
    float4 SV_Position;
};

struct MaterialResult { float3 base_color; float metalness; };

MaterialResult evaluateMaterial(MaterialInputs inputs)
{
    MaterialResult result;
    result.base_color = inputs.normalWorld * 0.5f;
    result.metalness = 0.0f;
    return result;
}
)";
  assert(merlin::VerifyMaterialSourcePassNeutral(kMaterialSource).empty());

  merlin::MaterialDiagnosticContext context;
  context.material_identity = "sha256:module";
  context.source_document = "prototype.mtlx";

  const auto entry_point = merlin::VerifyMaterialSourcePassNeutral(
      "[shader(\"fragment\")]\nfloat4 main() { return 0; }\n", context);
  assert(entry_point.size() == 1U);
  assert(entry_point[0].category ==
         MaterialDiagnosticCategory::GenerationFailure);
  assert(entry_point[0].context.source_document == "prototype.mtlx");
  assert(Mentions(entry_point, "a shader entry point"));
  assert(Mentions(entry_point, "at line 1"));

  const auto semantic = merlin::VerifyMaterialSourcePassNeutral(
      "struct Out\n{\n    float4 color : SV_Target0;\n};\n");
  assert(semantic.size() == 1U);
  assert(Mentions(semantic, "'SV_Target0'"));
  assert(Mentions(semantic, "at line 3"));

  assert(merlin::VerifyMaterialSourcePassNeutral(
             "[[vk::binding(0, 0)]]\nSampler2D t;\n")
             .size() == 1U);
  assert(merlin::VerifyMaterialSourcePassNeutral(
             "Texture2D t : register(t0);\n")
             .size() == 1U);
  assert(Mentions(merlin::VerifyMaterialSourcePassNeutral(
                      "void f() { if (a < b) { discard; } }\n"),
                  "a fragment discard"));

  // A qualified name is not a semantic binding, and an identifier that merely
  // ends in one of the forbidden words is not that word.
  assert(merlin::VerifyMaterialSourcePassNeutral(
             "float v = merlin::scale;\nint discarded = 0;\n"
             "int my_register(int x) { return x; }\n")
             .empty());

  // Findings are reported in source order, whichever pattern found them.
  const auto ordered = merlin::VerifyMaterialSourcePassNeutral(
      "float4 c : SV_Target;\n[shader(\"fragment\")]\nvoid f() { discard; }\n");
  assert(ordered.size() == 3U);
  assert(ordered[0].message.find("SV_Target") != std::string::npos);
  assert(ordered[1].message.find("entry point") != std::string::npos);
  assert(ordered[2].message.find("discard") != std::string::npos);
}

}  // namespace

int main() {
  VerifyNamesAreStableAndDistinct();
  VerifyModuleAgainstConsumer();
  VerifyTargetReflectionAgreement();
  VerifySourcePassNeutrality();
  std::cout << "material ABI contract ok\n";
  return 0;
}
