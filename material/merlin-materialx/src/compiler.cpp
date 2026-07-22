#include <merlin/materialx/compiler.hpp>

#include "sha256.hpp"

#include <MaterialXCore/Node.h>
#include <MaterialXCore/Value.h>
#include <MaterialXCore/Util.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXFormat/XmlIo.h>
#include <MaterialXGenHw/HwConstants.h>
#include <MaterialXGenShader/GenContext.h>
#include <MaterialXGenShader/Shader.h>
#include <MaterialXGenShader/Util.h>
#include <MaterialXGenSlang/SlangShaderGenerator.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace merlin::materialx {
namespace {

namespace mx = MaterialX;

constexpr std::string_view kEntryPoint = "evaluateMaterial";
constexpr std::string_view kModuleKeySchema =
    "animu-sphere.hdmerlin.material-module.v1";
constexpr std::string_view kInstanceKeySchema =
    "animu-sphere.hdmerlin.material-instance.v1";
constexpr std::string_view kResourceKeySchema =
    "animu-sphere.hdmerlin.material-resources.v1";
constexpr std::string_view kStandardLibraryFingerprintSchema =
    "animu-sphere.hdmerlin.materialx-standard-library.v1";
constexpr std::string_view kSourceDependencyFingerprintSchema =
    "animu-sphere.hdmerlin.materialx-source-dependencies.v1";

class MaterialFunctionGenerator final : public mx::SlangShaderGenerator {
 public:
  explicit MaterialFunctionGenerator(bool standard_surface)
      : mx::SlangShaderGenerator(mx::TypeSystem::create()),
        standard_surface_(standard_surface) {}

  static std::shared_ptr<MaterialFunctionGenerator> Create(
      bool standard_surface) {
    return std::make_shared<MaterialFunctionGenerator>(standard_surface);
  }

  [[nodiscard]] bool standard_surface() const noexcept {
    return standard_surface_;
  }

  [[nodiscard]] bool emittedUniform(std::string_view variable) const {
    return !standard_surface_ ||
           emitted_uniforms_.contains(std::string(variable));
  }

 protected:
  void emitVertexStage(const mx::ShaderGraph&, mx::GenContext&,
                       mx::ShaderStage& stage) const override {
    // A material function has no render-pass vertex stage. The renderer owns
    // geometry transforms, varyings, and stage entry points.
    stage.setSourceCode({});
  }

  void emitPixelStage(const mx::ShaderGraph& graph, mx::GenContext& context,
                      mx::ShaderStage& stage) const override {
    emitDirectives(context, stage);
    emitLineBreak(stage);
    emitLibraryInclude("stdlib/genslang/lib/mx_texture.slang", context, stage);
    emitTypeDefinitions(context, stage);
    emitConstants(context, stage);
    emitMaterialUniforms(graph, context, stage);

    const auto& vertex_data = stage.getInputBlock(mx::HW::VERTEX_DATA);
    emitLine("struct MaterialInputs", stage, false);
    emitScopeBegin(stage);
    // Fragment-coordinate consumers use vd.SV_Position in upstream
    // MaterialX code. It is ordinary material input here, not an AOV or pass
    // declaration.
    emitLine("float4 SV_Position", stage);
    for (std::size_t index = 0; index < vertex_data.size(); ++index) {
      auto port = *vertex_data[index];
      port.setSemantic({});
      emitVariableDeclaration(&port, {}, context, stage, false);
      emitLineEnd(stage);
    }
    emitScopeEnd(stage, true);
    emitLine("static MaterialInputs vd", stage);
    emitLineBreak(stage);

    emitLibraryInclude("stdlib/genslang/lib/mx_math.slang", context, stage);
    emitLineBreak(stage);

    _tokenSubstitutions[mx::ShaderGenerator::T_FILE_TRANSFORM_UV] =
        context.getOptions().fileTextureVerticalFlip
            ? "mx_transform_uv_vflip.glsl"
            : "mx_transform_uv.glsl";
    _tokenSubstitutions[mx::HW::T_TEX_SAMPLER_SIGNATURE] =
        "SamplerTexture2D tex_sampler";

    if (standard_surface_) {
      emitStandardSurfaceFunction(graph, context, stage);
      return;
    }

    emitFunctionDefinitions(graph, context, stage);

    const auto* output = graph.getOutputSocket();
    if (output == nullptr) {
      throw std::runtime_error("MaterialX graph has no output socket");
    }
    const auto return_type = _syntax->getTypeName(output->getType());
    if (return_type.empty()) {
      throw std::runtime_error("MaterialX graph output has no Slang type");
    }

    setFunctionName(std::string(kEntryPoint), stage);
    emitLine(return_type + " " + std::string(kEntryPoint) +
                 "(MaterialInputs inputs)",
             stage, false);
    emitFunctionBodyBegin(graph, context, stage);
    emitLine("vd = inputs", stage);
    emitFunctionCalls(graph, context, stage);

    const auto* connection = output->getConnection();
    if (connection != nullptr) {
      emitLine("return " + connection->getVariable(), stage);
    } else if (output->getValue() != nullptr) {
      emitLine("return " +
                   _syntax->getValue(output->getType(), *output->getValue()),
               stage);
    } else {
      emitLine("return " + _syntax->getDefaultValue(output->getType()), stage);
    }
    emitFunctionBodyEnd(graph, context, stage);
  }

 private:
  static const mx::ShaderNode& StandardSurfaceRoot(
      const mx::ShaderGraph& graph) {
    const auto* output = graph.getOutputSocket();
    const auto* connection = output ? output->getConnection() : nullptr;
    const auto* root = connection ? connection->getNode() : nullptr;
    if (root == nullptr || root == &graph) {
      throw std::runtime_error(
          "Standard Surface graph has no root shader node");
    }
    return *root;
  }

  static const std::set<std::string_view>& StandardSurfaceInputs() {
    static const std::set<std::string_view> inputs = {
        "base", "base_color", "metalness", "specular_roughness", "normal"};
    return inputs;
  }

  static std::set<const mx::ShaderNode*> SelectedNodes(
      const mx::ShaderGraph& graph) {
    const auto& root = StandardSurfaceRoot(graph);
    std::set<const mx::ShaderNode*> selected;
    std::function<void(const mx::ShaderInput*)> visit =
        [&](const mx::ShaderInput* input) {
          const auto* connection = input ? input->getConnection() : nullptr;
          const auto* node = connection ? connection->getNode() : nullptr;
          if (node == nullptr || node == &graph || node == &root ||
              !selected.insert(node).second) {
            return;
          }
          for (const auto* upstream_input : node->getInputs()) {
            visit(upstream_input);
          }
        };
    for (const auto name : StandardSurfaceInputs()) {
      visit(root.getInput(std::string(name)));
    }
    return selected;
  }

  std::set<std::string> SelectedExpressions(
      const mx::ShaderGraph& graph, mx::GenContext& context) const {
    const auto& root = StandardSurfaceRoot(graph);
    const auto selected = SelectedNodes(graph);
    std::set<std::string> expressions;
    for (const auto name : StandardSurfaceInputs()) {
      const auto* input = root.getInput(std::string(name));
      if (input == nullptr) {
        throw std::runtime_error("Standard Surface input is missing: " +
                                 std::string(name));
      }
      expressions.insert(getUpstreamResult(input, context));
    }
    for (const auto* node : selected) {
      for (const auto* input : node->getInputs()) {
        expressions.insert(getUpstreamResult(input, context));
      }
    }
    return expressions;
  }

  void emitMaterialUniforms(const mx::ShaderGraph& graph,
                            mx::GenContext& context,
                            mx::ShaderStage& stage) const {
    const auto expressions = standard_surface_
                                 ? SelectedExpressions(graph, context)
                                 : std::set<std::string>{};
    const auto selected = [&](const mx::ShaderPort& uniform) {
      return !standard_surface_ ||
             expressions.contains(uniform.getVariable());
    };
    emitLine("cbuffer " + stage.getName() + "CB", stage, false);
    emitScopeBegin(stage);
    for (const auto& [block_name, block] : stage.getUniformBlocks()) {
      if (block->getName() == mx::HW::LIGHT_DATA) {
        continue;
      }
      bool wrote_comment = false;
      for (const auto* uniform : block->getVariableOrder()) {
        if (!selected(*uniform) ||
            uniform->getType() == mx::Type::FILENAME) {
          continue;
        }
        if (!wrote_comment) {
          emitComment("Uniform block: " + block_name, stage);
          wrote_comment = true;
        }
        emitVariableDeclaration(uniform, _syntax->getUniformQualifier(),
                                context, stage, false);
        emitLineEnd(stage);
        emitted_uniforms_.insert(uniform->getVariable());
      }
      if (wrote_comment) {
        emitLineBreak(stage);
      }
    }
    emitScopeEnd(stage);
    emitLineBreak(stage);

    // Resource types cannot reside in a constant buffer. Keep logical block
    // ownership in reflection while emitting each resource as its own global
    // parameter so Slang does not silently leak it into another binding slot.
    for (const auto& [block_name, block] : stage.getUniformBlocks()) {
      if (block->getName() == mx::HW::LIGHT_DATA) {
        continue;
      }
      bool wrote_comment = false;
      for (const auto* uniform : block->getVariableOrder()) {
        if (!selected(*uniform) ||
            uniform->getType() != mx::Type::FILENAME) {
          continue;
        }
        if (!wrote_comment) {
          emitComment("Resource block: " + block_name, stage);
          wrote_comment = true;
        }
        emitVariableDeclaration(uniform, _syntax->getUniformQualifier(),
                                context, stage, false);
        emitLineEnd(stage);
        emitted_uniforms_.insert(uniform->getVariable());
      }
      if (wrote_comment) {
        emitLineBreak(stage);
      }
    }
  }

  void emitStandardSurfaceFunction(const mx::ShaderGraph& graph,
                                   mx::GenContext& context,
                                   mx::ShaderStage& stage) const {
    const auto& root = StandardSurfaceRoot(graph);
    const auto selected = SelectedNodes(graph);
    for (const auto* node : graph.getNodes()) {
      if (selected.contains(node)) {
        emitFunctionDefinition(*node, context, stage);
      }
    }

    emitLine("struct MaterialResult", stage, false);
    emitScopeBegin(stage);
    emitLine("float3 base_color", stage);
    emitLine("float metalness", stage);
    emitLine("float specular_roughness", stage);
    emitLine("float3 shading_normal", stage);
    emitScopeEnd(stage, true);
    emitLineBreak(stage);

    const auto expression = [&](std::string_view name) {
      const auto* input = root.getInput(std::string(name));
      if (input == nullptr) {
        throw std::runtime_error("Standard Surface input is missing: " +
                                 std::string(name));
      }
      return getUpstreamResult(input, context);
    };

    setFunctionName(std::string(kEntryPoint), stage);
    emitLine("MaterialResult " + std::string(kEntryPoint) +
                 "(MaterialInputs inputs)",
             stage, false);
    emitFunctionBodyBegin(graph, context, stage);
    emitLine("vd = inputs", stage);
    for (const auto* node : graph.getNodes()) {
      if (selected.contains(node)) {
        emitFunctionCall(*node, context, stage);
      }
    }
    emitLine("MaterialResult result", stage);
    emitLine("result.base_color = " + expression("base") + " * " +
                 expression("base_color"),
             stage);
    emitLine("result.metalness = " + expression("metalness"), stage);
    emitLine("result.specular_roughness = " +
                 expression("specular_roughness"),
             stage);
    emitLine("result.shading_normal = normalize(" + expression("normal") +
                 ")",
             stage);
    emitLine("return result", stage);
    emitFunctionBodyEnd(graph, context, stage);
  }

  bool standard_surface_{};
  mutable std::set<std::string> emitted_uniforms_;
};

void AddError(CompileResult& result, DiagnosticCode code,
              std::string element_path, std::string message) {
  result.diagnostics.push_back(Diagnostic{DiagnosticSeverity::Error, code,
                                          std::move(element_path),
                                          std::move(message)});
}

std::string NormalizeNewlines(std::string text) {
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

void AppendCacheField(std::string& record, std::string_view name,
                      std::string_view value) {
  record.append(name);
  record.push_back('=');
  record.append(std::to_string(value.size()));
  record.push_back(':');
  record.append(value);
  record.push_back('\n');
}

std::filesystem::path NormalizedAbsolutePath(
    const std::filesystem::path& path) {
  std::error_code error;
  auto absolute = std::filesystem::absolute(path, error);
  return (error ? path : absolute).lexically_normal();
}

bool IsPortableRelativePath(const std::filesystem::path& path) {
  if (path.empty() || path.is_absolute()) {
    return false;
  }
  const auto first = *path.begin();
  return first != "..";
}

std::string MakeLogicalDependencyPath(
    const std::filesystem::path& path,
    const mx::FileSearchPath& search_path) {
  const auto normalize_library_path = [](std::string candidate) {
    static const std::string_view library_roots[] = {
        "targets/", "stdlib/", "pbrlib/", "bxdf/"};
    for (const auto root : library_roots) {
      if (candidate.starts_with(root)) {
        return "libraries/" + candidate;
      }
    }
    return candidate;
  };
  const auto absolute = NormalizedAbsolutePath(path);
  std::optional<std::string> best;
  for (const auto& root : search_path) {
    const auto relative = absolute.lexically_relative(
        NormalizedAbsolutePath(root.asString()));
    if (!IsPortableRelativePath(relative)) {
      continue;
    }
    const auto candidate = relative.generic_string();
    if (!best || candidate.size() < best->size()) {
      best = candidate;
    }
  }
  if (best) {
    return normalize_library_path(*best);
  }

  const auto generic = absolute.generic_string();
  constexpr std::string_view marker = "/libraries/";
  const auto marker_position = generic.rfind(marker);
  if (marker_position != std::string::npos) {
    return generic.substr(marker_position + 1U);
  }
  throw std::runtime_error(
      "MaterialX dependency is outside every registered data root: " +
      path.generic_string());
}

std::string ReadDependency(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("Could not read MaterialX dependency: " +
                             path.generic_string());
  }
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

std::string FingerprintDependencies(
    std::string_view schema, const mx::StringSet& dependency_paths,
    const mx::FileSearchPath& search_path,
    std::vector<MaterialDependencyFingerprint>& dependencies) {
  std::map<std::string, std::string> sorted;
  for (const auto& dependency_path : dependency_paths) {
    const std::filesystem::path path(dependency_path);
    const auto logical_path = MakeLogicalDependencyPath(path, search_path);
    const auto content_sha256 = detail::Sha256(ReadDependency(path));
    const auto [entry, inserted] =
        sorted.emplace(logical_path, content_sha256);
    if (!inserted && entry->second != content_sha256) {
      throw std::runtime_error(
          "MaterialX dependencies resolve to the same logical path with "
          "different content: " +
          logical_path);
    }
  }

  std::string record;
  AppendCacheField(record, "schema", schema);
  dependencies.reserve(sorted.size());
  for (const auto& [path, content_sha256] : sorted) {
    AppendCacheField(record, "path", path);
    AppendCacheField(record, "content-sha256", content_sha256);
    dependencies.push_back({path, content_sha256});
  }
  return "sha256:" + detail::Sha256(record);
}

bool IsSupportedOutput(std::string_view type) {
  static const std::set<std::string_view> supported = {
      "float",   "color3",       "color4", "vector2",
      "vector3", "vector4",      "surfaceshader"};
  return supported.contains(type);
}

MaterialValueType ToMaterialValueType(std::string_view type) {
  if (type == "float") {
    return MaterialValueType::Float;
  }
  if (type == "vector2") {
    return MaterialValueType::Float2;
  }
  if (type == "color3" || type == "vector3") {
    return MaterialValueType::Float3;
  }
  if (type == "color4" || type == "vector4") {
    return MaterialValueType::Float4;
  }
  if (type == "integer" || type == "string") {
    return MaterialValueType::Integer;
  }
  if (type == "boolean") {
    return MaterialValueType::Boolean;
  }
  if (type == "filename") {
    return MaterialValueType::CombinedTextureSampler;
  }
  return MaterialValueType::Unknown;
}

bool IsResourceType(MaterialValueType type) {
  return type == MaterialValueType::Texture2D ||
         type == MaterialValueType::Sampler ||
         type == MaterialValueType::CombinedTextureSampler;
}

std::string Lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](char c) {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(c)));
  });
  return value;
}

std::optional<MaterialValue> ParseParameterDefault(
    const MaterialFunctionPort& port, std::string& error) {
  try {
    if (port.type == "float") {
      return mx::fromValueString<float>(port.default_value);
    }
    if (port.type == "vector2") {
      const auto value = mx::fromValueString<mx::Vector2>(port.default_value);
      return Vec2{value[0], value[1]};
    }
    if (port.type == "color3") {
      const auto value = mx::fromValueString<mx::Color3>(port.default_value);
      return Vec3{value[0], value[1], value[2]};
    }
    if (port.type == "vector3") {
      const auto value = mx::fromValueString<mx::Vector3>(port.default_value);
      return Vec3{value[0], value[1], value[2]};
    }
    if (port.type == "color4") {
      const auto value = mx::fromValueString<mx::Color4>(port.default_value);
      return Vec4{value[0], value[1], value[2], value[3]};
    }
    if (port.type == "vector4") {
      const auto value = mx::fromValueString<mx::Vector4>(port.default_value);
      return Vec4{value[0], value[1], value[2], value[3]};
    }
    if (port.type == "integer") {
      return static_cast<std::int32_t>(
          mx::fromValueString<int>(port.default_value));
    }
    if (port.type == "string") {
      // MaterialXGenSlang lowers strings to integer placeholders. Enumerated
      // options are remapped before reflection; non-enumerated strings such as
      // the v0.10 image-layer default lower to zero because Slang has no string
      // value type.
      return std::int32_t{0};
    }
    if (port.type == "boolean") {
      return mx::fromValueString<bool>(port.default_value);
    }
  } catch (const std::exception& exception) {
    error = "Could not parse default for reflected parameter '" +
            port.variable + "': " + exception.what();
    return std::nullopt;
  }
  error = "Unsupported reflected parameter type '" + port.type +
          "' for '" + port.variable + "'";
  return std::nullopt;
}

std::optional<std::string> PopulateLogicalModule(
    MaterialFunctionModule& module) {
  auto& logical = module.logical_module;
  logical.entry_point = module.entry_point;
  for (const auto& input : module.inputs) {
    const auto semantic = Lowercase(input.name + " " + input.variable);
    if (semantic.find("texcoord") != std::string::npos) {
      if (semantic.find("texcoord_0") == std::string::npos) {
        return "Only texture coordinate set 0 is supported by the material "
               "contract; reflected input was '" +
               input.variable + "'";
      }
      logical.requirements.inputs |= MaterialInputRequirement::Texcoord0;
    }
    if (semantic.find("normalworld") != std::string::npos) {
      logical.requirements.inputs |= MaterialInputRequirement::NormalWorld;
    } else if (semantic.find("normalobject") != std::string::npos) {
      logical.requirements.inputs |= MaterialInputRequirement::NormalObject;
    } else if (semantic.find("normal") != std::string::npos) {
      return "Unsupported normal input semantic '" + input.variable + "'";
    }
    if (semantic.find("positionworld") != std::string::npos) {
      logical.requirements.inputs |= MaterialInputRequirement::PositionWorld;
    } else if (semantic.find("positionobject") != std::string::npos) {
      logical.requirements.inputs |= MaterialInputRequirement::PositionObject;
    } else if (semantic.find("position") != std::string::npos) {
      return "Unsupported position input semantic '" + input.variable + "'";
    }
  }
  for (const auto& uniform : module.uniforms) {
    const auto type = ToMaterialValueType(uniform.type);
    const auto name =
        uniform.variable.empty() ? uniform.name : uniform.variable;
    if (IsResourceType(type)) {
      logical.resources.entries.push_back({name, type, 1});
      module.resource_defaults.entries.push_back(
          {name, type, {uniform.default_value}});
    } else {
      std::string error;
      auto value = ParseParameterDefault(uniform, error);
      if (!value) {
        return error;
      }
      logical.parameters.entries.push_back({name, type, 1});
      module.parameter_defaults.entries.push_back(
          {name, type, {std::move(*value)}});
    }
  }
  if (module.output_type == "color3" || module.output_type == "color4") {
    logical.requirements.results = MaterialResultField::BaseColor;
  } else if (module.output_type == "material_result") {
    logical.requirements.results =
        MaterialResultField::BaseColor | MaterialResultField::Metalness |
        MaterialResultField::SpecularRoughness |
        MaterialResultField::ShadingNormal;
  }
  return std::nullopt;
}

void AppendPortInterface(std::string& record, std::string_view kind,
                         const MaterialFunctionPort& port) {
  AppendCacheField(record, std::string(kind) + "-block", port.block);
  AppendCacheField(record, std::string(kind) + "-name", port.name);
  AppendCacheField(record, std::string(kind) + "-variable", port.variable);
  AppendCacheField(record, std::string(kind) + "-type", port.type);
}

std::string MakeIdentity(std::string_view schema,
                         const MaterialFunctionModule& module,
                         bool include_parameters, bool include_resources) {
  std::string record;
  AppendCacheField(record, "schema", schema);
  AppendCacheField(record, "module", module.module_key);
  for (const auto& uniform : module.uniforms) {
    const bool resource = IsResourceType(ToMaterialValueType(uniform.type));
    if ((resource && include_resources) ||
        (!resource && include_parameters)) {
      AppendPortInterface(record, resource ? "resource" : "parameter",
                          uniform);
      AppendCacheField(record, "value", uniform.default_value);
    }
  }
  return "sha256:" + detail::Sha256(record);
}

std::map<std::string, std::string> FindUnsupportedNodes(
    const mx::ElementPtr& renderable, bool standard_surface) {
  static const std::set<std::string_view> supported = {
      "add", "constant", "convert", "image", "mix", "multiply", "normal",
      "texcoord"};
  std::map<std::string, std::string> unsupported;
  const auto inspect = [&unsupported,
                        standard_surface](const mx::ElementPtr& element) {
    const auto node = element ? element->asA<mx::Node>() : nullptr;
    if (node != nullptr && !supported.contains(node->getCategory()) &&
        !(standard_surface &&
          node->getCategory() == "standard_surface")) {
      unsupported.emplace(node->getNamePath(), node->getCategory());
    }
  };

  inspect(renderable);
  for (const mx::Edge edge : renderable->traverseGraph()) {
    inspect(edge.getUpstreamElement());
  }
  return unsupported;
}

MaterialFunctionPort MakePort(std::string block,
                              const mx::ShaderPort& port) {
  return MaterialFunctionPort{std::move(block), port.getName(),
                              port.getVariable(), port.getType().getName(),
                              port.getValueString()};
}

void CollectReflection(const mx::ShaderStage& stage,
                       const MaterialFunctionGenerator& generator,
                       MaterialFunctionModule& module) {
  const auto& inputs = stage.getInputBlock(mx::HW::VERTEX_DATA);
  for (const auto* input : inputs.getVariableOrder()) {
    if (generator.standard_surface() &&
        module.source.find("vd." + input->getVariable()) ==
            std::string::npos) {
      continue;
    }
    module.inputs.push_back(MakePort("MaterialInputs", *input));
  }
  for (const auto& [block_name, block] : stage.getUniformBlocks()) {
    // emitUniforms(..., false) deliberately omits LightData because lighting
    // belongs to the renderer-owned pass wrapper, so reflection must omit it
    // as well.
    if (block->getName() == mx::HW::LIGHT_DATA) {
      continue;
    }
    for (const auto* uniform : block->getVariableOrder()) {
      if (!generator.emittedUniform(uniform->getVariable())) {
        continue;
      }
      module.uniforms.push_back(MakePort(block_name, *uniform));
    }
  }
}

std::string PruneUnusedStandardSurfaceInputs(
    std::string source, const mx::ShaderStage& stage) {
  const auto struct_begin = source.find("struct MaterialInputs");
  if (struct_begin == std::string::npos) {
    throw std::runtime_error("Generated source has no MaterialInputs struct");
  }

  std::vector<std::string> variables{"SV_Position"};
  const auto& inputs = stage.getInputBlock(mx::HW::VERTEX_DATA);
  for (const auto* input : inputs.getVariableOrder()) {
    variables.push_back(input->getVariable());
  }
  for (const auto& variable : variables) {
    if (source.find("vd." + variable) != std::string::npos) {
      continue;
    }
    const auto struct_end = source.find("};", struct_begin);
    const auto declaration =
        source.find(" " + variable + ";", struct_begin);
    if (struct_end == std::string::npos || declaration == std::string::npos ||
        declaration >= struct_end) {
      continue;
    }
    const auto line_begin_position = source.rfind('\n', declaration);
    const auto line_begin = line_begin_position == std::string::npos
                                ? declaration
                                : line_begin_position + 1U;
    const auto line_end_position = source.find('\n', declaration);
    const auto line_end = line_end_position == std::string::npos
                              ? source.size()
                              : line_end_position + 1U;
    source.erase(line_begin, line_end - line_begin);
  }
  return source;
}

std::optional<std::string> ValidateStandardSurfaceInputs(
    const mx::NodePtr& node) {
  static const std::set<std::string_view> supported = {
      "base", "base_color", "metalness", "specular_roughness", "normal"};
  for (const auto& input : node->getInputs()) {
    if (!supported.contains(input->getName())) {
      return "Unsupported Standard Surface input '" + input->getName() +
             "'; v0.10.0 supports base, base_color, metalness, "
             "specular_roughness, and normal";
    }
  }
  return std::nullopt;
}

mx::FileSearchPath MakeSearchPath(const CompileOptions& options) {
  mx::FileSearchPath search_path;
  for (const auto& path : options.library_search_paths) {
    search_path.append(mx::FilePath(path.generic_string()));
  }
  search_path.append(mx::getDefaultDataSearchPath());
  return search_path;
}

bool HasStandardLibraries(const mx::FileSearchPath& search_path) {
  static const std::string_view required[] = {
      "libraries/targets", "libraries/stdlib", "libraries/pbrlib",
      "libraries/bxdf"};
  return std::all_of(std::begin(required), std::end(required),
                     [&search_path](std::string_view path) {
                       return search_path.find(mx::FilePath(std::string(path)))
                           .exists();
                     });
}

}  // namespace

CompileResult CompileMaterialFunction(std::string_view document_xml,
                                      const CompileOptions& options) {
  CompileResult result;
  const auto search_path = MakeSearchPath(options);
  if (!HasStandardLibraries(search_path)) {
    AddError(result, DiagnosticCode::MissingStandardLibrary, {},
             "MaterialX data roots must contain libraries/targets, "
             "libraries/stdlib, libraries/pbrlib, and libraries/bxdf");
    return result;
  }

  try {
    auto library = mx::createDocument();
    const auto loaded_libraries = mx::loadLibraries(
        {mx::FilePath("libraries/targets"), mx::FilePath("libraries/stdlib"),
         mx::FilePath("libraries/pbrlib"), mx::FilePath("libraries/bxdf")},
        search_path, library);

    auto document = mx::createDocument();
    try {
      mx::readFromXmlString(document, std::string(document_xml), search_path);
    } catch (const mx::ExceptionParseError& error) {
      AddError(result, DiagnosticCode::InvalidDocument, {}, error.what());
      return result;
    }
    document->setDataLibrary(library);

    std::string validation_message;
    if (!document->validate(&validation_message)) {
      AddError(result, DiagnosticCode::InvalidDocument, {},
               NormalizeNewlines(std::move(validation_message)));
      return result;
    }

    mx::ElementPtr renderable;
    if (!options.renderable_path.empty()) {
      renderable = document->getDescendant(options.renderable_path);
      if (renderable == nullptr) {
        AddError(result, DiagnosticCode::RenderableNotFound,
                 options.renderable_path,
                 "MaterialX renderable path was not found");
        return result;
      }
    } else {
      const auto renderables = mx::findRenderableElements(document);
      if (renderables.empty()) {
        AddError(result, DiagnosticCode::RenderableNotFound, {},
                 "MaterialX document has no renderable element");
        return result;
      }
      if (renderables.size() != 1U) {
        AddError(result, DiagnosticCode::AmbiguousRenderable, {},
                 "MaterialX document has multiple renderable elements; set "
                 "CompileOptions::renderable_path");
        return result;
      }
      renderable = renderables.front();
    }

    const auto typed = renderable->asA<mx::TypedElement>();
    if (typed == nullptr || !IsSupportedOutput(typed->getType())) {
      AddError(result, DiagnosticCode::UnsupportedRenderable,
               renderable->getNamePath(),
               "The prototype accepts float, color, vector, and direct "
               "Standard Surface outputs; this renderable type is not "
               "supported");
      return result;
    }

    const auto renderable_node = renderable->asA<mx::Node>();
    const bool standard_surface =
        typed->getType() == "surfaceshader" && renderable_node != nullptr &&
        renderable_node->getCategory() == "standard_surface";
    if (typed->getType() == "surfaceshader" && !standard_surface) {
      AddError(result, DiagnosticCode::UnsupportedRenderable,
               renderable->getNamePath(),
               "Only a direct standard_surface node may cross the v0.10.0 "
               "material-result boundary");
      return result;
    }
    if (standard_surface) {
      if (const auto error =
              ValidateStandardSurfaceInputs(renderable_node)) {
        AddError(result, DiagnosticCode::UnsupportedInput,
                 renderable->getNamePath(), *error);
        return result;
      }
    }

    const auto unsupported =
        FindUnsupportedNodes(renderable, standard_surface);
    for (const auto& [path, category] : unsupported) {
      AddError(result, DiagnosticCode::UnsupportedNode, path,
               "Unsupported MaterialX node category: " + category);
    }
    if (!unsupported.empty()) {
      return result;
    }

    auto generator = MaterialFunctionGenerator::Create(standard_surface);
    mx::GenContext context(generator);
    context.registerSourceCodeSearchPath(search_path);
    generator->registerTypeDefs(document);
    context.getOptions().hwMaxActiveLightSources = 0;
    context.getOptions().hwSrgbEncodeOutput = false;

    const auto shader_name = mx::createValidName(renderable->getName());
    auto shader = generator->generate(shader_name, renderable, context);
    if (shader == nullptr) {
      AddError(result, DiagnosticCode::GenerationFailure,
               renderable->getNamePath(),
               "MaterialXGenSlang returned no shader");
      return result;
    }

    const auto& stage = shader->getStage(mx::Stage::PIXEL);
    MaterialFunctionModule module;
    module.source = NormalizeNewlines(stage.getSourceCode());
    if (standard_surface) {
      module.source =
          PruneUnusedStandardSurfaceInputs(std::move(module.source), stage);
    }
    module.output_type =
        standard_surface ? "material_result" : typed->getType();
    module.materialx_version = mx::getVersionString();
    module.generator_version = generator->getVersion();
    module.generator_revision = MERLIN_MATERIALX_GENSLANG_REVISION;
    module.standard_library_fingerprint = FingerprintDependencies(
        kStandardLibraryFingerprintSchema, loaded_libraries, search_path,
        module.standard_library_dependencies);
    auto source_dependencies = stage.getSourceDependencies();
    source_dependencies.insert(stage.getIncludes().begin(),
                               stage.getIncludes().end());
    module.source_dependency_fingerprint = FingerprintDependencies(
        kSourceDependencyFingerprintSchema, source_dependencies,
        search_path, module.source_dependencies);
    CollectReflection(stage, *generator, module);
    if (const auto error = PopulateLogicalModule(module)) {
      AddError(result, DiagnosticCode::UnsupportedInput,
               renderable->getNamePath(), *error);
      return result;
    }

    // Generated source and its logical interface encode graph topology and
    // compile-time specialization, while reflected defaults remain runtime
    // instance/resource state. This keeps parameter-only edits from forcing
    // shader regeneration.
    std::string module_record;
    AppendCacheField(module_record, "schema", kModuleKeySchema);
    AppendCacheField(module_record, "materialx", module.materialx_version);
    AppendCacheField(module_record, "generator", module.generator_version);
    AppendCacheField(module_record, "revision", module.generator_revision);
    AppendCacheField(module_record, "standard-library",
                     module.standard_library_fingerprint);
    AppendCacheField(module_record, "source-dependencies",
                     module.source_dependency_fingerprint);
    AppendCacheField(module_record, "generator-options",
                     "max-lights=0;srgb-output=false");
    AppendCacheField(module_record, "abi",
                     std::to_string(module.logical_module.abi_version));
    AppendCacheField(
        module_record, "reflection",
        std::to_string(module.logical_module.reflection_schema_version));
    AppendCacheField(module_record, "entry", module.entry_point);
    AppendCacheField(module_record, "output", module.output_type);
    AppendCacheField(
        module_record, "required-inputs",
        std::to_string(static_cast<std::uint32_t>(
            module.logical_module.requirements.inputs)));
    AppendCacheField(
        module_record, "required-results",
        std::to_string(static_cast<std::uint32_t>(
            module.logical_module.requirements.results)));
    for (const auto& input : module.inputs) {
      AppendPortInterface(module_record, "input", input);
    }
    for (const auto& uniform : module.uniforms) {
      AppendPortInterface(module_record, "uniform", uniform);
    }
    AppendCacheField(module_record, "source", module.source);
    module.module_key = "sha256:" + detail::Sha256(module_record);
    module.cache_key = module.module_key;
    module.logical_module.key = module.module_key;
    module.instance_key =
        MakeIdentity(kInstanceKeySchema, module, true, false);
    module.resource_key =
        MakeIdentity(kResourceKeySchema, module, false, true);
    module.parameter_defaults.key = module.instance_key;
    module.resource_defaults.key = module.resource_key;
    result.module = std::move(module);
  } catch (const std::exception& error) {
    AddError(result, DiagnosticCode::GenerationFailure,
             options.renderable_path, error.what());
  } catch (...) {
    AddError(result, DiagnosticCode::GenerationFailure,
             options.renderable_path,
             "MaterialX generation failed with an unknown exception");
  }
  return result;
}

}  // namespace merlin::materialx
