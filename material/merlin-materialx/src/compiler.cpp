#include <merlin/materialx/compiler.hpp>

#include "sha256.hpp"

#include <MaterialXCore/Node.h>
#include <MaterialXCore/Util.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXFormat/XmlIo.h>
#include <MaterialXGenHw/HwConstants.h>
#include <MaterialXGenShader/GenContext.h>
#include <MaterialXGenShader/Shader.h>
#include <MaterialXGenShader/Util.h>
#include <MaterialXGenSlang/SlangShaderGenerator.h>

#include <algorithm>
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
constexpr std::string_view kCacheSchema =
    "animu-sphere.hdmerlin.material-function-cache.v1";

class MaterialFunctionGenerator final : public mx::SlangShaderGenerator {
 public:
  MaterialFunctionGenerator()
      : mx::SlangShaderGenerator(mx::TypeSystem::create()) {}

  static std::shared_ptr<MaterialFunctionGenerator> Create() {
    return std::make_shared<MaterialFunctionGenerator>();
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
    emitUniforms(context, stage, false);

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

bool IsSupportedOutput(std::string_view type) {
  static const std::set<std::string_view> supported = {
      "float", "color3", "color4", "vector2", "vector3", "vector4"};
  return supported.contains(type);
}

std::map<std::string, std::string> FindUnsupportedNodes(
    const mx::ElementPtr& renderable) {
  static const std::set<std::string_view> supported = {
      "add", "constant", "convert", "image", "mix", "multiply", "normal",
      "texcoord"};
  std::map<std::string, std::string> unsupported;
  const auto inspect = [&unsupported](const mx::ElementPtr& element) {
    const auto node = element ? element->asA<mx::Node>() : nullptr;
    if (node != nullptr && !supported.contains(node->getCategory())) {
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
                       MaterialFunctionModule& module) {
  const auto& inputs = stage.getInputBlock(mx::HW::VERTEX_DATA);
  for (const auto* input : inputs.getVariableOrder()) {
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
      module.uniforms.push_back(MakePort(block_name, *uniform));
    }
  }
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
  static const std::string_view required[] = {"libraries/targets",
                                               "libraries/stdlib",
                                               "libraries/pbrlib"};
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
             "libraries/stdlib, and libraries/pbrlib");
    return result;
  }

  try {
    auto library = mx::createDocument();
    mx::loadLibraries(
        {mx::FilePath("libraries/targets"), mx::FilePath("libraries/stdlib"),
         mx::FilePath("libraries/pbrlib")},
        search_path, library);

    auto document = mx::createDocument();
    mx::readFromXmlString(document, std::string(document_xml), search_path);
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
               "The prototype accepts float, color, and vector graph outputs; "
               "surface and closure outputs remain diagnosed until the "
               "Standard Surface parameter adapter is connected");
      return result;
    }

    const auto unsupported = FindUnsupportedNodes(renderable);
    for (const auto& [path, category] : unsupported) {
      AddError(result, DiagnosticCode::UnsupportedNode, path,
               "Unsupported MaterialX node category: " + category);
    }
    if (!unsupported.empty()) {
      return result;
    }

    auto generator = MaterialFunctionGenerator::Create();
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
    module.output_type = typed->getType();
    module.materialx_version = mx::getVersionString();
    module.generator_version = generator->getVersion();
    module.generator_revision = MERLIN_MATERIALX_GENSLANG_REVISION;
    CollectReflection(stage, module);

    const auto canonical_document =
        NormalizeNewlines(mx::writeToXmlString(document));
    std::string cache_record;
    AppendCacheField(cache_record, "schema", kCacheSchema);
    AppendCacheField(cache_record, "materialx", module.materialx_version);
    AppendCacheField(cache_record, "generator", module.generator_version);
    AppendCacheField(cache_record, "revision", module.generator_revision);
    AppendCacheField(cache_record, "entry", module.entry_point);
    AppendCacheField(cache_record, "renderable", renderable->getNamePath());
    AppendCacheField(cache_record, "document", canonical_document);
    AppendCacheField(cache_record, "source", module.source);
    module.cache_key = "sha256:" + detail::Sha256(cache_record);
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
