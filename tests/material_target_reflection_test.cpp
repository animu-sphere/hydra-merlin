// Cross-target ABI evidence for a generated material.
//
// The compiler test proves one MaterialX document produces one deterministic
// Slang module. This test proves the artifacts built from that one module still
// describe the material the module declared: it recompiles the document, reads
// the reflection the Slang compiler emitted for each target, restates it in the
// Core vocabulary, and checks it through `merlin/core/material_abi.hpp`.
//
// SPIR-V and Metal report different native bindings for the same parameter --
// a descriptor slot against a buffer index -- so the agreement being checked is
// deliberately semantic. Both targets agreeing with one module is what makes
// them agree with each other, which is the property v0.10.0 claims.
//
// The reflection JSON is parsed here rather than in Core or in
// `Merlin::MaterialX`: reading a particular compiler's reflection format is the
// job of whichever layer invoked that compiler, and neither of those two may
// grow a dependency on it.

#include <merlin/core/material_abi.hpp>
#include <merlin/core/material_diagnostic.hpp>
#include <merlin/core/types.hpp>
#include <merlin/materialx/compiler.hpp>

#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

std::string ReadFile(const char* path) {
  std::ifstream stream(path, std::ios::binary);
  assert(stream);
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

// A JSON value, only as much of one as a Slang reflection document needs. The
// repository carries no JSON dependency and this is the only consumer, so the
// parser stays here with its consumer rather than becoming shared surface.
struct Json {
  enum class Kind { Null, Boolean, Number, String, Array, Object };

  Kind kind{Kind::Null};
  bool boolean{};
  double number{};
  std::string text;
  std::vector<Json> items;
  std::vector<std::pair<std::string, Json>> members;

  [[nodiscard]] const Json* Member(std::string_view name) const {
    for (const auto& member : members) {
      if (member.first == name) {
        return &member.second;
      }
    }
    return nullptr;
  }

  [[nodiscard]] const Json& Require(std::string_view name) const {
    const auto* member = Member(name);
    assert(member != nullptr && "reflection JSON is missing a required member");
    return *member;
  }

  [[nodiscard]] std::string Text(std::string_view name) const {
    const auto* member = Member(name);
    return member != nullptr && member->kind == Kind::String ? member->text
                                                             : std::string{};
  }
};

void SkipSpace(std::string_view text, std::size_t& at) {
  while (at < text.size() &&
         std::isspace(static_cast<unsigned char>(text[at])) != 0) {
    ++at;
  }
}

std::string ParseString(std::string_view text, std::size_t& at) {
  assert(text[at] == '"');
  ++at;
  std::string value;
  while (at < text.size() && text[at] != '"') {
    if (text[at] != '\\') {
      value.push_back(text[at++]);
      continue;
    }
    ++at;
    switch (text[at]) {
      case 'n': value.push_back('\n'); break;
      case 't': value.push_back('\t'); break;
      case 'r': value.push_back('\r'); break;
      case 'b': value.push_back('\b'); break;
      case 'f': value.push_back('\f'); break;
      // Every name this document carries is a shader identifier, so a \u escape
      // would mean the format changed rather than that a name needed one.
      case 'u': assert(false && "unexpected \\u escape in reflection JSON"); break;
      default: value.push_back(text[at]); break;
    }
    ++at;
  }
  ++at;
  return value;
}

Json ParseValue(std::string_view text, std::size_t& at) {
  SkipSpace(text, at);
  assert(at < text.size());
  Json value;
  const char character = text[at];
  if (character == '"') {
    value.kind = Json::Kind::String;
    value.text = ParseString(text, at);
    return value;
  }
  if (character == '{') {
    value.kind = Json::Kind::Object;
    ++at;
    SkipSpace(text, at);
    while (at < text.size() && text[at] != '}') {
      SkipSpace(text, at);
      auto name = ParseString(text, at);
      SkipSpace(text, at);
      assert(text[at] == ':');
      ++at;
      value.members.emplace_back(std::move(name), ParseValue(text, at));
      SkipSpace(text, at);
      if (at < text.size() && text[at] == ',') {
        ++at;
        SkipSpace(text, at);
      }
    }
    ++at;
    return value;
  }
  if (character == '[') {
    value.kind = Json::Kind::Array;
    ++at;
    SkipSpace(text, at);
    while (at < text.size() && text[at] != ']') {
      value.items.push_back(ParseValue(text, at));
      SkipSpace(text, at);
      if (at < text.size() && text[at] == ',') {
        ++at;
        SkipSpace(text, at);
      }
    }
    ++at;
    return value;
  }
  if (text.compare(at, 4, "true") == 0) {
    at += 4;
    value.kind = Json::Kind::Boolean;
    value.boolean = true;
    return value;
  }
  if (text.compare(at, 5, "false") == 0) {
    at += 5;
    value.kind = Json::Kind::Boolean;
    return value;
  }
  if (text.compare(at, 4, "null") == 0) {
    at += 4;
    return value;
  }
  const auto begin = at;
  while (at < text.size() &&
         (std::isdigit(static_cast<unsigned char>(text[at])) != 0 ||
          text[at] == '-' || text[at] == '+' || text[at] == '.' ||
          text[at] == 'e' || text[at] == 'E')) {
    ++at;
  }
  assert(at > begin && "reflection JSON contains an unparsable value");
  value.kind = Json::Kind::Number;
  value.number = std::stod(std::string(text.substr(begin, at - begin)));
  return value;
}

Json ParseJson(std::string_view text) {
  std::size_t at{};
  return ParseValue(text, at);
}

// The Slang reflection type vocabulary, restated in the Core one. An
// unrecognized shape stays `Unknown` on purpose: it then fails the agreement
// check against whatever the module declared instead of being guessed into
// something plausible.
merlin::MaterialValueType ToValueType(const Json& type,
                                      std::uint32_t& array_size) {
  const auto kind = type.Text("kind");
  if (kind == "array") {
    const auto* count = type.Member("elementCount");
    array_size = count != nullptr
                     ? static_cast<std::uint32_t>(count->number)
                     : 1U;
    return ToValueType(type.Require("elementType"), array_size);
  }
  if (kind == "scalar") {
    const auto scalar = type.Text("scalarType");
    if (scalar == "float32") {
      return merlin::MaterialValueType::Float;
    }
    if (scalar == "int32" || scalar == "uint32") {
      return merlin::MaterialValueType::Integer;
    }
    if (scalar == "bool") {
      return merlin::MaterialValueType::Boolean;
    }
    return merlin::MaterialValueType::Unknown;
  }
  if (kind == "vector") {
    std::uint32_t element_array_size{1};
    if (ToValueType(type.Require("elementType"), element_array_size) !=
        merlin::MaterialValueType::Float) {
      return merlin::MaterialValueType::Unknown;
    }
    const auto* count = type.Member("elementCount");
    switch (count != nullptr ? static_cast<int>(count->number) : 0) {
      case 2: return merlin::MaterialValueType::Float2;
      case 3: return merlin::MaterialValueType::Float3;
      case 4: return merlin::MaterialValueType::Float4;
      default: return merlin::MaterialValueType::Unknown;
    }
  }
  if (kind == "struct" && type.Text("name") == "SamplerTexture2D") {
    // MaterialXGenSlang pairs a texture with its sampler in one generated
    // struct, which is what the logical `CombinedTextureSampler` names.
    return merlin::MaterialValueType::CombinedTextureSampler;
  }
  if (kind == "resource" && type.Text("baseShape") == "texture2D") {
    return merlin::MaterialValueType::Texture2D;
  }
  if (kind == "samplerState") {
    return merlin::MaterialValueType::Sampler;
  }
  return merlin::MaterialValueType::Unknown;
}

merlin::MaterialTargetReflection ReadTargetReflection(const char* path,
                                                      std::string target) {
  const auto text = ReadFile(path);
  const auto document = ParseJson(text);

  merlin::MaterialTargetReflection reflection;
  reflection.target = std::move(target);
  for (const auto& entry_point : document.Require("entryPoints").items) {
    reflection.entry_points.push_back(entry_point.Text("name"));
  }

  int constant_buffers{};
  for (const auto& parameter : document.Require("parameters").items) {
    const auto& type = parameter.Require("type");
    if (type.Text("kind") == "constantBuffer") {
      ++constant_buffers;
      for (const auto& field :
           type.Require("elementType").Require("fields").items) {
        std::uint32_t array_size{1};
        const auto value_type = ToValueType(field.Require("type"), array_size);
        reflection.parameters.entries.push_back(
            {field.Text("name"), value_type, array_size});
      }
      continue;
    }
    std::uint32_t array_size{1};
    const auto value_type = ToValueType(type, array_size);
    reflection.resources.entries.push_back(
        {parameter.Text("name"), value_type, array_size});
  }
  // These wrappers declare no renderer uniforms of their own, so the material's
  // block is the artifact's only constant buffer and needs no separating. A
  // composed Forward artifact will carry the renderer's as well, and the
  // backend that composes it owns that separation.
  assert(constant_buffers == 1);
  return reflection;
}

merlin::materialx::CompileResult Compile(const char* document_path,
                                         const char* data_root,
                                         std::string renderable_path,
                                         std::string source_document) {
  merlin::materialx::CompileOptions options;
  options.renderable_path = std::move(renderable_path);
  options.library_search_paths.emplace_back(data_root);
  options.source_document = std::move(source_document);
  auto result = merlin::materialx::CompileMaterialFunction(
      ReadFile(document_path), options);
  if (!result) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << "generation failed at " << diagnostic.element_path << ": "
                << diagnostic.message << '\n';
    }
  }
  assert(result);
  assert(result.diagnostics.empty());
  return result;
}

void Report(const std::vector<merlin::MaterialDiagnostic>& records,
            std::string_view what) {
  for (const auto& record : records) {
    std::cerr << what << ": " << merlin::ToDiagnostic(record).message << '\n';
  }
}

bool SameInterface(const merlin::MaterialTargetReflection& left,
                   const merlin::MaterialTargetReflection& right) {
  const auto same = [](const auto& first, const auto& second) {
    if (first.size() != second.size()) {
      return false;
    }
    for (const auto& entry : first) {
      bool matched{};
      for (const auto& other : second) {
        matched = matched || (entry.name == other.name &&
                              entry.type == other.type &&
                              entry.array_size == other.array_size);
      }
      if (!matched) {
        return false;
      }
    }
    return true;
  };
  return same(left.parameters.entries, right.parameters.entries) &&
         same(left.resources.entries, right.resources.entries);
}

// A module and the artifacts built from it, checked in both directions: the
// artifacts agree, and a single spoiled entry is caught.
void VerifyModuleAgainstTargets(const merlin::MaterialModule& module,
                                const merlin::MaterialTargetReflection& spirv,
                                const merlin::MaterialTargetReflection& metal) {
  for (const auto* reflection : {&spirv, &metal}) {
    const auto records =
        merlin::VerifyMaterialTargetReflection(module, *reflection);
    Report(records, reflection->target);
    assert(records.empty());
    // The material was inlined into the renderer-owned entry point the wrapper
    // declared, so it never reached the artifact as one of its own.
    assert(reflection->entry_points.size() == 1U);
    assert(reflection->entry_points[0] != module.entry_point);
  }
  assert(!spirv.parameters.entries.empty());
  assert(SameInterface(spirv, metal));

  auto dropped = spirv;
  dropped.parameters.entries.pop_back();
  assert(!merlin::VerifyMaterialTargetReflection(module, dropped).empty());

  auto retyped = metal;
  retyped.parameters.entries.front().type = merlin::MaterialValueType::Float4;
  const auto retyped_records =
      merlin::VerifyMaterialTargetReflection(module, retyped);
  assert(retyped_records.size() == 1U);
  assert(retyped_records[0].category ==
         merlin::MaterialDiagnosticCategory::ReflectionMismatch);
  assert(retyped_records[0].context.backend_target == "metal");
  assert(retyped_records[0].context.material_identity == module.key);
}

}  // namespace

int main(int argc, char** argv) {
  assert(argc == 8);
  const auto* prototype_document = argv[1];
  const auto* standard_surface_document = argv[2];
  const auto* data_root = argv[3];

  const auto prototype = Compile(prototype_document, data_root,
                                 "NG_prototype/out", "prototype.mtlx");
  const auto standard_surface =
      Compile(standard_surface_document, data_root,
              "NG_standard_surface/surface", "standard-surface.mtlx");
  const auto& prototype_module = prototype.module->logical_module;
  const auto& standard_surface_module = standard_surface.module->logical_module;

  // Neither generated module declares any part of a render pass. This is the
  // property that lets a renderer-owned fragment shader include one at all.
  merlin::MaterialDiagnosticContext context;
  context.material_identity = standard_surface_module.key;
  context.source_document = standard_surface.source_document;
  context.generator_version = standard_surface.generator_version;
  const auto contamination = merlin::VerifyMaterialSourcePassNeutral(
      standard_surface.module->source, context);
  Report(contamination, "pass neutrality");
  assert(contamination.empty());
  assert(merlin::VerifyMaterialSourcePassNeutral(prototype.module->source)
             .empty());

  // ... and the check would say so if one did. The wrapper that composes the
  // module supplies exactly this kind of declaration, which is why it lives in
  // the renderer's source and not in the generated module.
  const auto contaminated = merlin::VerifyMaterialSourcePassNeutral(
      standard_surface.module->source +
          "\n[shader(\"fragment\")]\nfloat4 main() : SV_Target { return 0; }\n",
      context);
  assert(contaminated.size() == 2U);
  assert(contaminated[0].context.source_document == "standard-surface.mtlx");

  // A Forward pass that shades reads the whole minimum Standard Surface result
  // and can build every input in the v0.10.0 slice.
  merlin::MaterialAbiExpectation forward;
  forward.required_results = merlin::MaterialResultField::BaseColor |
                             merlin::MaterialResultField::Metalness |
                             merlin::MaterialResultField::SpecularRoughness |
                             merlin::MaterialResultField::ShadingNormal;
  const auto standard_surface_abi =
      merlin::VerifyMaterialAbi(standard_surface_module, forward);
  Report(standard_surface_abi, "standard surface ABI");
  assert(standard_surface_abi.empty());

  // The graph-only prototype produces a color and nothing else, so the same
  // consumer must reject it rather than shade with three unwritten fields.
  const auto prototype_abi =
      merlin::VerifyMaterialAbi(prototype_module, forward);
  assert(prototype_abi.size() == 3U);
  for (const auto& record : prototype_abi) {
    assert(record.category == merlin::MaterialDiagnosticCategory::AbiMismatch);
    assert(record.fallback == merlin::MaterialFallback::BasicMaterial);
  }
  // It is a perfectly good module for a consumer that reads only a base color.
  assert(merlin::VerifyMaterialAbi(prototype_module).empty());

  VerifyModuleAgainstTargets(prototype_module,
                             ReadTargetReflection(argv[4], "spirv"),
                             ReadTargetReflection(argv[5], "metal"));
  VerifyModuleAgainstTargets(standard_surface_module,
                             ReadTargetReflection(argv[6], "spirv"),
                             ReadTargetReflection(argv[7], "metal"));

  std::cout << "material target reflection agreement ok\n";
  return 0;
}
