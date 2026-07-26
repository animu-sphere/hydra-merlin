#include <merlin/core/material_abi.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <tuple>
#include <utility>

namespace merlin {
namespace {

std::uint32_t Bits(MaterialResultField value) noexcept {
  return static_cast<std::uint32_t>(value);
}

std::uint32_t Bits(MaterialInputRequirement value) noexcept {
  return static_cast<std::uint32_t>(value);
}

// The flags walked one at a time, so a message can name the single one that is
// missing. Each list is static_asserted against the mask `types.hpp` folds next
// to the enum: a flag added there and forgotten here would otherwise make this
// check silently stop covering it, which is the one failure mode a checker must
// not have.
constexpr std::array<MaterialResultField, 4> kResultFields{
    MaterialResultField::BaseColor, MaterialResultField::Metalness,
    MaterialResultField::SpecularRoughness,
    MaterialResultField::ShadingNormal};

constexpr std::array<MaterialInputRequirement, 5> kInputRequirements{
    MaterialInputRequirement::PositionObject,
    MaterialInputRequirement::PositionWorld,
    MaterialInputRequirement::NormalObject,
    MaterialInputRequirement::NormalWorld,
    MaterialInputRequirement::Texcoord0};

template <typename Flag, std::size_t Count>
constexpr Flag FoldFlags(const std::array<Flag, Count>& flags) noexcept {
  auto folded = static_cast<std::uint32_t>(Flag::None);
  for (const auto flag : flags) {
    folded |= static_cast<std::uint32_t>(flag);
  }
  return static_cast<Flag>(folded);
}

static_assert(FoldFlags(kResultFields) == kAllMaterialResultFields,
              "a material result field was added without extending the ABI "
              "check that walks them");
static_assert(FoldFlags(kInputRequirements) == kAllMaterialInputRequirements,
              "a material geometry input was added without extending the ABI "
              "check that walks them");

MaterialDiagnostic MakeRecord(MaterialDiagnosticCategory category,
                              std::string message,
                              MaterialDiagnosticContext context,
                              const MaterialFallbackPolicy& policy) {
  MaterialDiagnostic record;
  record.category = category;
  record.severity = DiagnosticSeverity::Error;
  // An interface disagreement has no partial graph to fall back onto, so the
  // ladder resolves the same way whether or not a simplification was possible.
  record.fallback = ResolveMaterialFallback(category, false, policy);
  record.message = std::move(message);
  record.context = std::move(context);
  return record;
}

// A name the module declared twice cannot be bound: nothing downstream can say
// which of the two a consumer would get. That is the module's own fault rather
// than any consumer's or any target's, so it is an `AbiMismatch` wherever it is
// found, and both entry points into this contract look for it.
template <typename Entry>
void VerifyDeclaredNamesUnique(const std::vector<Entry>& declared,
                               std::string_view noun,
                               const MaterialDiagnosticContext& base,
                               const MaterialFallbackPolicy& policy,
                               std::vector<MaterialDiagnostic>& records) {
  for (auto index = std::size_t{}; index < declared.size(); ++index) {
    const auto begin = declared.begin();
    const auto here = begin + static_cast<std::ptrdiff_t>(index);
    const auto same_name = [&](const Entry& other) {
      return other.name == here->name;
    };
    // Reported at the first of the duplicates, so one repeated name costs one
    // record however many times the module declared it.
    if (std::any_of(begin, here, same_name) ||
        std::none_of(here + 1, declared.end(), same_name)) {
      continue;
    }
    auto context = base;
    context.input_name = here->name;
    records.push_back(MakeRecord(
        MaterialDiagnosticCategory::AbiMismatch,
        "Module declares " + std::string(noun) + " '" + here->name +
            "' more than once; which one a consumer would bind is not knowable",
        std::move(context), policy));
  }
}

// Parameter and resource layout entries carry the same three fields and are
// checked by the same rule; only the noun in the message differs.
template <typename Entry>
void VerifyLayout(const std::vector<Entry>& declared,
                  const std::vector<Entry>& reported, std::string_view noun,
                  const std::string& target,
                  const MaterialDiagnosticContext& base,
                  const MaterialFallbackPolicy& policy,
                  std::vector<MaterialDiagnostic>& records) {
  const auto record = [&](std::string message, const std::string& name) {
    auto context = base;
    // A reflected variable is attributable to a name but never to an authored
    // node: this failure was detected against a compiled interface, and the
    // document element that produced it is not knowable from here.
    context.input_name = name;
    records.push_back(MakeRecord(MaterialDiagnosticCategory::ReflectionMismatch,
                                 std::move(message), std::move(context),
                                 policy));
  };
  const std::string quoted_target = "Target '" + target + "' ";

  for (const auto& expected : declared) {
    if (std::count_if(declared.begin(), declared.end(),
                      [&](const Entry& other) {
                        return other.name == expected.name;
                      }) > 1) {
      // `VerifyDeclaredNamesUnique` has already reported this name. Nothing the
      // target reported can be attributed to one of two declarations, so
      // comparing would blame the target for disagreeing with whichever
      // declaration happened to be listed last.
      continue;
    }
    const Entry* found{};
    std::size_t matches{};
    for (const auto& actual : reported) {
      if (actual.name == expected.name) {
        ++matches;
        found = &actual;
      }
    }
    if (matches == 0U) {
      record(quoted_target + "does not report " + std::string(noun) + " '" +
                 expected.name + "', which the module declares",
             expected.name);
      continue;
    }
    if (matches > 1U) {
      // Which of the two a consumer would bind is not knowable, so there is
      // nothing further to say about this entry.
      record(quoted_target + "reports " + std::string(noun) + " '" +
                 expected.name + "' more than once",
             expected.name);
      continue;
    }
    if (found->type != expected.type) {
      record(quoted_target + "reports " + std::string(noun) + " '" +
                 expected.name + "' as " +
                 std::string(MaterialValueTypeName(found->type)) +
                 "; the module declares " +
                 std::string(MaterialValueTypeName(expected.type)),
             expected.name);
    }
    if (found->array_size != expected.array_size) {
      record(quoted_target + "reports " + std::string(noun) + " '" +
                 expected.name + "' with array size " +
                 std::to_string(found->array_size) +
                 "; the module declares " + std::to_string(expected.array_size),
             expected.name);
    }
  }

  for (auto index = std::size_t{}; index < reported.size(); ++index) {
    const auto& actual = reported[index];
    const bool declared_here = std::any_of(
        declared.begin(), declared.end(),
        [&](const Entry& expected) { return expected.name == actual.name; });
    if (declared_here) {
      continue;
    }
    // A name the target reported more than once is still one entry the module
    // does not declare; a second identical record would tell a host nothing the
    // first did not.
    const auto reported_before =
        reported.begin() + static_cast<std::ptrdiff_t>(index);
    if (std::any_of(reported.begin(), reported_before,
                    [&](const Entry& earlier) {
                      return earlier.name == actual.name;
                    })) {
      continue;
    }
    // A renderer-owned uniform that landed in the material block reaches the
    // consumer as material state it never agreed to own.
    record(quoted_target + "reports " + std::string(noun) + " '" + actual.name +
               "', which the module does not declare",
           actual.name);
  }
}

bool IsIdentifierCharacter(char character) noexcept {
  const auto value = static_cast<unsigned char>(character);
  return std::isalnum(value) != 0 || character == '_';
}

// Comments and string literals are blanked rather than removed so every
// remaining character keeps its original offset and line. A construct named in
// prose ("we never discard here") is then not mistaken for one declared.
std::string StripCommentsAndStrings(std::string_view source) {
  std::string stripped(source);
  const auto blank = [&stripped](std::size_t begin, std::size_t end) {
    for (auto index = begin; index < end && index < stripped.size(); ++index) {
      if (stripped[index] != '\n') {
        stripped[index] = ' ';
      }
    }
  };
  for (std::size_t index = 0; index < stripped.size();) {
    const char character = stripped[index];
    if (character == '/' && index + 1 < stripped.size()) {
      if (stripped[index + 1] == '/') {
        auto end = stripped.find('\n', index);
        end = end == std::string::npos ? stripped.size() : end;
        blank(index, end);
        index = end;
        continue;
      }
      if (stripped[index + 1] == '*') {
        auto end = stripped.find("*/", index + 2);
        end = end == std::string::npos ? stripped.size() : end + 2;
        blank(index, end);
        index = end;
        continue;
      }
    }
    // Only double quotes: Slang has no character literal, so treating a stray
    // apostrophe as one would blank real declarations after it.
    //
    // The run also stops at a newline, because Slang has no multi-line string
    // literal. An unbalanced quote then costs the rest of one line instead of
    // blanking every declaration after it, which would leave the scan below
    // finding nothing and this whole check passing by seeing no source at all.
    if (character == '"') {
      auto end = index + 1;
      while (end < stripped.size() && stripped[end] != character &&
             stripped[end] != '\n') {
        // An escape consumes the next character, but never the newline that
        // ends the run.
        if (stripped[end] == '\\' && end + 1 < stripped.size() &&
            stripped[end + 1] != '\n') {
          ++end;
        }
        ++end;
      }
      const bool closed = end < stripped.size() && stripped[end] == '"';
      end = closed ? end + 1 : end;
      blank(index, end);
      index = end;
      continue;
    }
    ++index;
  }
  return stripped;
}

std::size_t LineOf(std::string_view source, std::size_t offset) {
  return static_cast<std::size_t>(
             std::count(source.begin(), source.begin() +
                                            static_cast<std::ptrdiff_t>(offset),
                        '\n')) +
         1U;
}

struct PassDeclaration {
  std::string_view text;
  std::string_view description;
};

// Declaration forms that make a module part of a render pass. Slang spellings,
// because Slang is the one shading language every backend here compiles.
//
// `[[vk::` is matched as a whole family rather than as the three attributes
// seen so far. Every member of it decides a binding, and a material function
// needs none of them, so matching the prefix costs nothing a correct generator
// would have emitted and closes the gap that listing members leaves open.
constexpr std::array<PassDeclaration, 8> kPassDeclarations{{
    {"[shader(", "a shader entry point"},
    {"[numthreads(", "a compute entry point"},
    {"[earlydepthstencil", "a depth-stencil pass mode"},
    {"[[vk::", "an explicit Vulkan binding attribute"},
    {"register(", "an explicit register binding"},
    {"packoffset(", "an explicit constant-buffer packing offset"},
    {"groupshared", "a group-shared allocation"},
    {"discard", "a fragment discard"},
}};

struct Finding {
  std::size_t offset{};
  std::string description;
};

void FindPassDeclarations(const std::string& source,
                          std::vector<Finding>& findings) {
  for (const auto& declaration : kPassDeclarations) {
    const bool leading_boundary = IsIdentifierCharacter(declaration.text.front());
    const bool trailing_boundary = IsIdentifierCharacter(declaration.text.back());
    std::size_t position = source.find(declaration.text);
    while (position != std::string::npos) {
      const auto end = position + declaration.text.size();
      const bool bounded =
          (!leading_boundary || position == 0 ||
           !IsIdentifierCharacter(source[position - 1])) &&
          (!trailing_boundary || end >= source.size() ||
           !IsIdentifierCharacter(source[end]));
      if (bounded) {
        findings.push_back({position, std::string(declaration.description)});
      }
      position = source.find(declaration.text, position + 1);
    }
  }
}

// Any bound system value, not an enumerated few: a material function that reads
// or writes one is participating in a stage it does not own, whichever one it
// picked. Only the semantic form counts, so a generated struct field merely
// *named* after one is left alone.
//
// A `:` also closes a conditional expression, and `c ? a : sv_scale` binds
// nothing. The two are told apart by what precedes the colon within the same
// statement: a declarator never contains a `?`, and a conditional operator
// always does. One forward pass carries that as a depth count, which nests
// correctly and costs no second scan per colon.
void FindSystemValueSemantics(const std::string& source,
                              std::vector<Finding>& findings) {
  std::size_t conditional_depth{};
  for (std::size_t position = 0; position < source.size(); ++position) {
    const char character = source[position];
    if (character == ';' || character == '{' || character == '}') {
      conditional_depth = 0;
      continue;
    }
    if (character == '?') {
      ++conditional_depth;
      continue;
    }
    if (character != ':') {
      continue;
    }
    // A qualified name is neither a semantic nor a conditional, so it leaves
    // the depth alone; both of its colons are skipped by this same test.
    if ((position > 0 && source[position - 1] == ':') ||
        (position + 1 < source.size() && source[position + 1] == ':')) {
      continue;
    }
    if (conditional_depth > 0) {
      --conditional_depth;
      continue;
    }
    auto start = position + 1;
    while (start < source.size() &&
           std::isspace(static_cast<unsigned char>(source[start])) != 0) {
      ++start;
    }
    auto end = start;
    while (end < source.size() && IsIdentifierCharacter(source[end])) {
      ++end;
    }
    const std::string_view semantic(source.data() + start, end - start);
    if (semantic.size() < 3U) {
      continue;
    }
    const bool system_value =
        std::tolower(static_cast<unsigned char>(semantic[0])) == 's' &&
        std::tolower(static_cast<unsigned char>(semantic[1])) == 'v' &&
        semantic[2] == '_';
    if (system_value) {
      findings.push_back(
          {position, "the system-value semantic '" + std::string(semantic) +
                         "'"});
    }
  }
}

}  // namespace

std::string_view MaterialValueTypeName(MaterialValueType type) noexcept {
  switch (type) {
    case MaterialValueType::Float:
      return "float";
    case MaterialValueType::Float2:
      return "float2";
    case MaterialValueType::Float3:
      return "float3";
    case MaterialValueType::Float4:
      return "float4";
    case MaterialValueType::Integer:
      return "int";
    case MaterialValueType::Boolean:
      return "bool";
    case MaterialValueType::Texture2D:
      return "texture2d";
    case MaterialValueType::Sampler:
      return "sampler";
    case MaterialValueType::CombinedTextureSampler:
      return "texture2d-sampler";
    case MaterialValueType::Unknown:
      return "unknown";
  }
  return "unknown";
}

std::string_view MaterialResultFieldName(MaterialResultField field) noexcept {
  switch (field) {
    case MaterialResultField::None:
      return "none";
    case MaterialResultField::BaseColor:
      return "base-color";
    case MaterialResultField::Metalness:
      return "metalness";
    case MaterialResultField::SpecularRoughness:
      return "specular-roughness";
    case MaterialResultField::ShadingNormal:
      return "shading-normal";
  }
  return "unknown";
}

std::string_view MaterialInputRequirementName(
    MaterialInputRequirement input) noexcept {
  switch (input) {
    case MaterialInputRequirement::None:
      return "none";
    case MaterialInputRequirement::PositionObject:
      return "position-object";
    case MaterialInputRequirement::PositionWorld:
      return "position-world";
    case MaterialInputRequirement::NormalObject:
      return "normal-object";
    case MaterialInputRequirement::NormalWorld:
      return "normal-world";
    case MaterialInputRequirement::Texcoord0:
      return "texcoord-0";
  }
  return "unknown";
}

std::vector<MaterialDiagnostic> VerifyMaterialAbi(
    const MaterialModule& module, const MaterialAbiExpectation& expectation,
    const MaterialFallbackPolicy& policy) {
  std::vector<MaterialDiagnostic> records;
  MaterialDiagnosticContext base;
  base.material_identity = module.key;

  const auto mismatch = [&](std::string message,
                            MaterialDiagnosticContext context) {
    records.push_back(MakeRecord(MaterialDiagnosticCategory::AbiMismatch,
                                 std::move(message), std::move(context),
                                 policy));
  };

  if (module.abi_version != expectation.abi_version) {
    mismatch("Module declares material ABI version " +
                 std::to_string(module.abi_version) +
                 "; this consumer implements " +
                 std::to_string(expectation.abi_version),
             base);
  }
  if (module.reflection_schema_version !=
      expectation.reflection_schema_version) {
    mismatch("Module declares reflection schema version " +
                 std::to_string(module.reflection_schema_version) +
                 "; this consumer implements " +
                 std::to_string(expectation.reflection_schema_version),
             base);
  }
  if (module.entry_point != expectation.entry_point) {
    mismatch("Module entry point '" + module.entry_point +
                 "' does not match the required '" + expectation.entry_point +
                 "'",
             base);
  }
  // Checked here as well as against a reflection, because a consumer deciding
  // whether it may draw with a module has no artifact in hand yet.
  VerifyDeclaredNamesUnique(module.parameters.entries, "parameter", base, policy,
                            records);
  VerifyDeclaredNamesUnique(module.resources.entries, "resource", base, policy,
                            records);
  for (const auto field : kResultFields) {
    const bool required = (Bits(expectation.required_results) & Bits(field)) != 0U;
    const bool produced =
        (Bits(module.requirements.results) & Bits(field)) != 0U;
    if (required && !produced) {
      // The missing thing is a result field, and a record has no field for one:
      // naming it as an input would point a host at the wrong half of the ABI.
      mismatch("Consumer reads material result '" +
                   std::string(MaterialResultFieldName(field)) +
                   "', which the module does not produce",
               base);
    }
  }
  for (const auto input : kInputRequirements) {
    const bool needed = (Bits(module.requirements.inputs) & Bits(input)) != 0U;
    const bool available =
        (Bits(expectation.available_inputs) & Bits(input)) != 0U;
    if (needed && !available) {
      auto context = base;
      context.input_name = std::string(MaterialInputRequirementName(input));
      mismatch("Module requires geometry input '" + context.input_name +
                   "', which this consumer cannot supply",
               std::move(context));
    }
  }
  return records;
}

std::vector<MaterialDiagnostic> VerifyMaterialTargetReflection(
    const MaterialModule& module, const MaterialTargetReflection& reflection,
    const MaterialFallbackPolicy& policy) {
  std::vector<MaterialDiagnostic> records;
  MaterialDiagnosticContext base;
  base.material_identity = module.key;
  base.backend_target = reflection.target;

  if (reflection.entry_points.empty()) {
    records.push_back(
        MakeRecord(MaterialDiagnosticCategory::AbiMismatch,
                   "Target '" + reflection.target +
                       "' reports no entry point; nothing calls the material",
                   base, policy));
  }
  for (const auto& entry_point : reflection.entry_points) {
    if (entry_point != module.entry_point) {
      continue;
    }
    records.push_back(MakeRecord(
        MaterialDiagnosticCategory::AbiMismatch,
        "Target '" + reflection.target + "' reports '" + entry_point +
            "' as an artifact entry point; the material is composed into a "
            "renderer-owned one rather than becoming the pass",
        base, policy));
  }
  VerifyDeclaredNamesUnique(module.parameters.entries, "parameter", base, policy,
                            records);
  VerifyDeclaredNamesUnique(module.resources.entries, "resource", base, policy,
                            records);
  VerifyLayout(module.parameters.entries, reflection.parameters.entries,
               "parameter", reflection.target, base, policy, records);
  VerifyLayout(module.resources.entries, reflection.resources.entries,
               "resource", reflection.target, base, policy, records);
  return records;
}

std::vector<MaterialDiagnostic> VerifyMaterialSourcePassNeutral(
    std::string_view source, const MaterialDiagnosticContext& context,
    const MaterialFallbackPolicy& policy) {
  const auto stripped = StripCommentsAndStrings(source);
  std::vector<Finding> findings;
  FindPassDeclarations(stripped, findings);
  FindSystemValueSemantics(stripped, findings);
  // Source order, so a reader walks the module the way it is written rather
  // than the order the scanner happened to look for things.
  std::sort(findings.begin(), findings.end(),
            [](const Finding& left, const Finding& right) {
              return std::tie(left.offset, left.description) <
                     std::tie(right.offset, right.description);
            });

  std::vector<MaterialDiagnostic> records;
  records.reserve(findings.size());
  for (const auto& finding : findings) {
    records.push_back(
        MakeRecord(MaterialDiagnosticCategory::GenerationFailure,
                   "Generated material source declares " + finding.description +
                       " at line " +
                       std::to_string(LineOf(stripped, finding.offset)) +
                       "; the renderer owns the pass",
                   context, policy));
  }
  return records;
}

}  // namespace merlin
