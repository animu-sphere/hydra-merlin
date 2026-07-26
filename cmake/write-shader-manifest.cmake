# Every field describing how an artifact was compiled arrives in
# MERLIN_SHADER_RECORDS_FILE, which _merlin_compile_shader writes from the same
# values it passes to slangc, and the sources it compiled arrive in the depfile
# slangc itself emitted. Nothing about the compile is restated here, so a
# manifest entry cannot describe a compile that did not happen.
#
# The module and artifact identities below mirror
# merlin/core/shader_artifact.hpp, which is the normative definition.
# merlin-shader-artifact-key recomputes every key in the emitted manifest
# through that header, so the two cannot drift.

if(NOT DEFINED MERLIN_SHADER_MANIFEST OR
   NOT DEFINED MERLIN_SHADER_SCHEMA_VERSION OR
   NOT DEFINED MERLIN_SHADER_MODULE_IDENTITY_SCHEMA OR
   NOT DEFINED MERLIN_SHADER_ARTIFACT_KEY_SCHEMA OR
   NOT DEFINED MERLIN_SHADER_ABI_VERSION OR
   NOT DEFINED MERLIN_SHADER_RECORDS_FILE OR
   NOT DEFINED MERLIN_SHADER_SOURCE_DIR OR
   NOT DEFINED MERLIN_SLANG_VERSION OR
   NOT DEFINED MERLIN_SLANG_REQUIRED_SERIES OR
   NOT DEFINED MERLIN_SLANG_MATRIX_LAYOUT OR
   NOT DEFINED MERLIN_SLANG_OPTIMIZATION OR
   NOT DEFINED MERLIN_SLANG_DEBUG_INFO OR
   NOT DEFINED MERLIN_VULKAN_SDK_VERSION OR
   NOT DEFINED MERLIN_CMAKE_GENERATOR OR
   NOT DEFINED MERLIN_ENVIRONMENT_HDR)
  message(FATAL_ERROR "Missing shader manifest generation argument")
endif()

foreach(_required_file MERLIN_SHADER_RECORDS_FILE MERLIN_ENVIRONMENT_HDR)
  if(NOT EXISTS "${${_required_file}}")
    message(FATAL_ERROR
      "Shader manifest input ${_required_file} is missing: ${${_required_file}}")
  endif()
endforeach()
if(NOT IS_DIRECTORY "${MERLIN_SHADER_SOURCE_DIR}")
  message(FATAL_ERROR
    "Shader source directory is missing: ${MERLIN_SHADER_SOURCE_DIR}")
endif()
if(NOT MERLIN_SLANG_DEBUG_INFO MATCHES "^(true|false)$")
  message(FATAL_ERROR
    "Shader debug policy must be true or false: ${MERLIN_SLANG_DEBUG_INFO}")
endif()

function(_merlin_json_escape _value _output)
  set(_escaped "${_value}")
  string(REPLACE "\\" "\\\\" _escaped "${_escaped}")
  string(REPLACE "\"" "\\\"" _escaped "${_escaped}")
  string(REPLACE "\n" "\\n" _escaped "${_escaped}")
  set(${_output} "${_escaped}" PARENT_SCOPE)
endfunction()

# Length-prefixed name and value, so neither can imitate a separator. This is
# the CMake spelling of merlin::AppendIdentityField.
macro(_merlin_identity_field _record_variable _name _value)
  string(LENGTH "${_name}" _merlin_identity_name_length)
  string(LENGTH "${_value}" _merlin_identity_value_length)
  string(APPEND ${_record_variable}
    "${_merlin_identity_name_length}:${_name}"
    "=${_merlin_identity_value_length}:${_value}\n")
endmacro()

file(SHA256 "${MERLIN_ENVIRONMENT_HDR}" _environment_hash)

# The include closure comes from the depfile slangc emitted for the artifact, so
# an added `#include` reaches the module identity without anyone restating it
# here. Logical paths are bare filenames: the package resolves every reference
# relative to itself, and an absolute build path would not be reproducible.
function(_merlin_depfile_module_paths _depfile _output)
  if(NOT EXISTS "${_depfile}")
    message(FATAL_ERROR "Shader depfile is missing: ${_depfile}")
  endif()
  file(READ "${_depfile}" _text)
  # Make escaping, outermost first: a line continuation joins two lines, an
  # escaped backslash is a path separator, an escaped space belongs to the path,
  # and an escaped colon is a drive letter.
  string(ASCII 1 _separator_placeholder)
  string(ASCII 2 _space_placeholder)
  string(REGEX REPLACE "\\\\[\r]?\n" " " _text "${_text}")
  string(REPLACE "\\\\" "${_separator_placeholder}" _text "${_text}")
  string(REPLACE "\\ " "${_space_placeholder}" _text "${_text}")
  string(REPLACE "\\:" ":" _text "${_text}")
  string(REPLACE "${_separator_placeholder}" "/" _text "${_text}")
  string(REGEX REPLACE "[ \t\r\n]+" ";" _tokens "${_text}")

  set(_paths "")
  foreach(_token IN LISTS _tokens)
    string(REPLACE "${_space_placeholder}" " " _token "${_token}")
    # The compiled artifact is the depfile's target, never one of its sources.
    if(NOT _token MATCHES "[.]slang$")
      continue()
    endif()
    get_filename_component(_name "${_token}" NAME)
    if(NOT EXISTS "${MERLIN_SHADER_SOURCE_DIR}/${_name}")
      message(FATAL_ERROR
        "Shader depfile ${_depfile} names a source outside "
        "${MERLIN_SHADER_SOURCE_DIR}: ${_token}")
    endif()
    list(APPEND _paths "${_name}")
  endforeach()
  if(_paths STREQUAL "")
    message(FATAL_ERROR "Shader depfile lists no sources: ${_depfile}")
  endif()

  list(REMOVE_DUPLICATES _paths)
  # Ordered by logical path, exactly as merlin::MakeShaderModuleIdentity orders
  # its fingerprints, so the caller's traversal order cannot leak into the key.
  list(SORT _paths)
  set(${_output} "${_paths}" PARENT_SCOPE)
endfunction()

# A handwritten module is identified by its own source plus every include it
# compiles with. One traversal produces both the identity and the evidence the
# manifest records, so the two cannot disagree about what was hashed.
function(_merlin_module_identity _paths _identity_output _sources_json_output)
  set(_record "")
  _merlin_identity_field(_record "schema"
    "${MERLIN_SHADER_MODULE_IDENTITY_SCHEMA}")
  set(_json "")
  foreach(_path IN LISTS _paths)
    file(SHA256 "${MERLIN_SHADER_SOURCE_DIR}/${_path}" _hash)
    _merlin_identity_field(_record "path" "${_path}")
    _merlin_identity_field(_record "content-sha256" "${_hash}")
    if(NOT _json STREQUAL "")
      string(APPEND _json ", ")
    endif()
    string(APPEND _json "{\"path\": \"${_path}\", \"sha256\": \"${_hash}\"}")
  endforeach()
  string(SHA256 _identity "${_record}")
  set(${_identity_output} "sha256:${_identity}" PARENT_SCOPE)
  set(${_sources_json_output} "[${_json}]" PARENT_SCOPE)
endfunction()

function(_merlin_features_json _features _output)
  string(REPLACE "+" ";" _feature_list "${_features}")
  set(_json "")
  foreach(_feature IN LISTS _feature_list)
    if(_json STREQUAL "")
      set(_json "\"${_feature}\"")
    else()
      string(APPEND _json ", \"${_feature}\"")
    endif()
  endforeach()
  set(${_output} "[${_json}]" PARENT_SCOPE)
endfunction()

set(_artifacts "")
set(_package_sources "")
file(STRINGS "${MERLIN_SHADER_RECORDS_FILE}" _records)
foreach(_record IN LISTS _records)
  string(REPLACE "|" ";" _fields "${_record}")
  list(LENGTH _fields _field_count)
  if(NOT _field_count EQUAL 11)
    message(FATAL_ERROR "Malformed shader record: ${_record}")
  endif()
  list(GET _fields 0 _artifact)
  list(GET _fields 1 _reflection)
  list(GET _fields 2 _depfile)
  list(GET _fields 3 _source)
  list(GET _fields 4 _entry)
  list(GET _fields 5 _stage)
  list(GET _fields 6 _target)
  list(GET _fields 7 _profile)
  list(GET _fields 8 _capabilities)
  list(GET _fields 9 _permutation)
  list(GET _fields 10 _features)

  foreach(_input "${_artifact}" "${_reflection}")
    if(NOT EXISTS "${_input}")
      message(FATAL_ERROR "Shader manifest input is missing: ${_input}")
    endif()
  endforeach()

  _merlin_depfile_module_paths("${_depfile}" _module_paths)
  if(NOT "${_source}" IN_LIST _module_paths)
    message(FATAL_ERROR
      "Shader depfile for ${_artifact} does not name its own source ${_source}")
  endif()
  # Every artifact compiled from one source must have compiled one include
  # closure. A target-conditional include would otherwise let two genuinely
  # different modules share a module identity.
  string(MAKE_C_IDENTIFIER "${_source}" _module)
  if(DEFINED _module_paths_${_module})
    if(NOT "${_module_paths}" STREQUAL "${_module_paths_${_module}}")
      message(FATAL_ERROR
        "${_source} compiled a different include closure per target: "
        "${_module_paths} after ${_module_paths_${_module}}")
    endif()
  else()
    set(_module_paths_${_module} "${_module_paths}")
    _merlin_module_identity("${_module_paths}"
      _module_identity_${_module} _module_sources_json_${_module})
    list(APPEND _package_sources ${_module_paths})
  endif()
  set(_module_identity "${_module_identity_${_module}}")
  set(_module_sources_json "${_module_sources_json_${_module}}")

  file(SHA256 "${_artifact}" _artifact_hash)
  file(SHA256 "${_reflection}" _reflection_hash)
  get_filename_component(_artifact_name "${_artifact}" NAME)
  get_filename_component(_reflection_name "${_reflection}" NAME)
  _merlin_features_json("${_features}" _features_json)

  # merlin/core/shader_artifact.hpp field order and spelling.
  set(_key_record "")
  _merlin_identity_field(_key_record "schema"
    "${MERLIN_SHADER_ARTIFACT_KEY_SCHEMA}")
  _merlin_identity_field(_key_record "module" "${_module_identity}")
  _merlin_identity_field(_key_record "abi" "${MERLIN_SHADER_ABI_VERSION}")
  _merlin_identity_field(_key_record "entry" "${_entry}")
  _merlin_identity_field(_key_record "stage" "${_stage}")
  _merlin_identity_field(_key_record "permutation" "${_permutation}")
  _merlin_identity_field(_key_record "features" "${_features}")
  _merlin_identity_field(_key_record "compiler" "slangc")
  _merlin_identity_field(_key_record "compiler-version"
    "${MERLIN_SLANG_VERSION}")
  _merlin_identity_field(_key_record "target" "${_target}")
  _merlin_identity_field(_key_record "profile" "${_profile}")
  _merlin_identity_field(_key_record "capabilities" "${_capabilities}")
  _merlin_identity_field(_key_record "matrix-layout"
    "${MERLIN_SLANG_MATRIX_LAYOUT}")
  _merlin_identity_field(_key_record "optimization"
    "${MERLIN_SLANG_OPTIMIZATION}")
  _merlin_identity_field(_key_record "debug-info" "${MERLIN_SLANG_DEBUG_INFO}")
  string(SHA256 _artifact_key_hash "${_key_record}")
  set(_artifact_key "sha256:${_artifact_key_hash}")

  set(_item
    "    {\n      \"path\": \"${_artifact_name}\",\n      \"reflection\": \"${_reflection_name}\",\n      \"source\": \"${_source}\",\n      \"entry_point\": \"${_entry}\",\n      \"stage\": \"${_stage}\",\n      \"target\": \"${_target}\",\n      \"profile\": \"${_profile}\",\n      \"capabilities\": \"${_capabilities}\",\n      \"permutation\": \"${_permutation}\",\n      \"features\": ${_features_json},\n      \"module_sources\": ${_module_sources_json},\n      \"module_identity\": \"${_module_identity}\",\n      \"artifact_sha256\": \"${_artifact_hash}\",\n      \"reflection_sha256\": \"${_reflection_hash}\",\n      \"artifact_key\": \"${_artifact_key}\"\n    }")
  if(_artifacts STREQUAL "")
    set(_artifacts "${_item}")
  else()
    string(APPEND _artifacts ",\n${_item}")
  endif()
endforeach()

if(_artifacts STREQUAL "")
  message(FATAL_ERROR "Shader manifest would contain no artifacts")
endif()

# The package inventory is the union of what the modules actually compiled, so
# it cannot name a source no artifact used or omit one every artifact did.
list(REMOVE_DUPLICATES _package_sources)
list(SORT _package_sources)
set(_sources "")
foreach(_path IN LISTS _package_sources)
  file(SHA256 "${MERLIN_SHADER_SOURCE_DIR}/${_path}" _hash)
  if(NOT _sources STREQUAL "")
    string(APPEND _sources ",\n")
  endif()
  string(APPEND _sources
    "    {\"path\": \"${_path}\", \"sha256\": \"${_hash}\"}")
endforeach()

_merlin_json_escape("${MERLIN_CMAKE_GENERATOR}" _generator)
_merlin_json_escape("${MERLIN_VULKAN_SDK_VERSION}" _sdk_version)
_merlin_json_escape("${MERLIN_SLANG_VERSION}" _slang_version)
set(_manifest
"{\n  \"schema_version\": ${MERLIN_SHADER_SCHEMA_VERSION},\n  \"shader_abi_version\": ${MERLIN_SHADER_ABI_VERSION},\n  \"cache_compatibility\": {\n    \"algorithm\": \"sha256\",\n    \"rule\": \"all artifact-key inputs must match exactly\",\n    \"module_identity_schema\": \"${MERLIN_SHADER_MODULE_IDENTITY_SCHEMA}\",\n    \"artifact_key_schema\": \"${MERLIN_SHADER_ARTIFACT_KEY_SCHEMA}\"\n  },\n  \"toolchain\": {\n    \"compiler\": \"slangc\",\n    \"compiler_version\": \"${_slang_version}\",\n    \"required_series\": \"${MERLIN_SLANG_REQUIRED_SERIES}\",\n    \"vulkan_sdk_version\": \"${_sdk_version}\",\n    \"generator\": \"CMake ${CMAKE_VERSION} / ${_generator}\"\n  },\n  \"policy\": {\n    \"matrix_layout\": \"${MERLIN_SLANG_MATRIX_LAYOUT}\",\n    \"optimization\": \"${MERLIN_SLANG_OPTIMIZATION}\",\n    \"debug_info\": ${MERLIN_SLANG_DEBUG_INFO}\n  },\n  \"sources\": [\n${_sources}\n  ],\n  \"environment\": {\n    \"path\": \"environment.hdr\",\n    \"sha256\": \"${_environment_hash}\",\n    \"representation\": \"diffuse-sh-l2\"\n  },\n  \"artifacts\": [\n${_artifacts}\n  ],\n  \"unsupported_features\": [\n    {\n      \"target\": \"metal\",\n      \"feature\": \"non_uniform_resource_indexing\",\n      \"diagnostic\": \"Slang reports NonUniformResourceIndex unavailable for the Metal fragment target\",\n      \"fallback\": \"forward-conventional\"\n    }\n  ]\n}\n")

get_filename_component(_manifest_dir "${MERLIN_SHADER_MANIFEST}" DIRECTORY)
file(MAKE_DIRECTORY "${_manifest_dir}")
file(WRITE "${MERLIN_SHADER_MANIFEST}" "${_manifest}")
