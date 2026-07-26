# Every field describing how an artifact was compiled arrives in
# MERLIN_SHADER_RECORDS_FILE, which _merlin_compile_shader writes from the same
# values it passes to slangc. Nothing about the compile is restated here, so a
# manifest entry cannot describe a compile that did not happen.
#
# The module and artifact identities below mirror merlin/core/shader_artifact.hpp,
# which is the normative definition. merlin-shader-artifact-key recomputes every
# key in the emitted manifest through that header, so the two cannot drift.

if(NOT DEFINED MERLIN_SHADER_MANIFEST OR
   NOT DEFINED MERLIN_SHADER_SCHEMA_VERSION OR
   NOT DEFINED MERLIN_SHADER_MODULE_IDENTITY_SCHEMA OR
   NOT DEFINED MERLIN_SHADER_ARTIFACT_KEY_SCHEMA OR
   NOT DEFINED MERLIN_SHADER_ABI_VERSION OR
   NOT DEFINED MERLIN_SHADER_RECORDS_FILE OR
   NOT DEFINED MERLIN_SLANG_VERSION OR
   NOT DEFINED MERLIN_SLANG_REQUIRED_SERIES OR
   NOT DEFINED MERLIN_SLANG_MATRIX_LAYOUT OR
   NOT DEFINED MERLIN_SLANG_OPTIMIZATION OR
   NOT DEFINED MERLIN_VULKAN_SDK_VERSION OR
   NOT DEFINED MERLIN_CMAKE_GENERATOR OR
   NOT DEFINED MERLIN_FORWARD_SOURCE OR
   NOT DEFINED MERLIN_BINDLESS_SOURCE OR
   NOT DEFINED MERLIN_COMMON_SOURCE OR
   NOT DEFINED MERLIN_ENVIRONMENT_HDR)
  message(FATAL_ERROR "Missing shader manifest generation argument")
endif()

foreach(_required_file
    MERLIN_SHADER_RECORDS_FILE
    MERLIN_FORWARD_SOURCE MERLIN_BINDLESS_SOURCE MERLIN_COMMON_SOURCE
    MERLIN_ENVIRONMENT_HDR)
  if(NOT EXISTS "${${_required_file}}")
    message(FATAL_ERROR
      "Shader manifest input ${_required_file} is missing: ${${_required_file}}")
  endif()
endforeach()

function(_merlin_json_escape _value _output)
  set(_escaped "${_value}")
  string(REPLACE "\\" "\\\\" _escaped "${_escaped}")
  string(REPLACE "\"" "\\\"" _escaped "${_escaped}")
  string(REPLACE "\n" "\\n" _escaped "${_escaped}")
  set(${_output} "${_escaped}" PARENT_SCOPE)
endfunction()

# Length-prefixed field, so no value can imitate a separator. This is the CMake
# spelling of merlin::AppendIdentityField.
macro(_merlin_identity_field _record_variable _name _value)
  string(LENGTH "${_value}" _merlin_identity_field_length)
  string(APPEND ${_record_variable}
    "${_name}=${_merlin_identity_field_length}:${_value}\n")
endmacro()

file(SHA256 "${MERLIN_COMMON_SOURCE}" _common_hash)
file(SHA256 "${MERLIN_FORWARD_SOURCE}" _forward_hash)
file(SHA256 "${MERLIN_BINDLESS_SOURCE}" _bindless_hash)
file(SHA256 "${MERLIN_ENVIRONMENT_HDR}" _environment_hash)

# A handwritten module is identified by its own source plus every include it
# compiles with, ordered by logical path.
function(_merlin_module_identity _output)
  set(_record "")
  _merlin_identity_field(_record "schema"
    "${MERLIN_SHADER_MODULE_IDENTITY_SCHEMA}")
  set(_sources ${ARGN})
  list(SORT _sources)
  foreach(_source IN LISTS _sources)
    string(REPLACE ":" ";" _fields "${_source}")
    list(GET _fields 0 _path)
    list(GET _fields 1 _hash)
    _merlin_identity_field(_record "path" "${_path}")
    _merlin_identity_field(_record "content-sha256" "${_hash}")
  endforeach()
  string(SHA256 _identity "${_record}")
  set(${_output} "sha256:${_identity}" PARENT_SCOPE)
endfunction()

set(_forward_module_sources
  "forward-common.slang:${_common_hash}" "forward.slang:${_forward_hash}")
set(_bindless_module_sources
  "forward-bindless.slang:${_bindless_hash}"
  "forward-common.slang:${_common_hash}")
_merlin_module_identity(_forward_module_identity ${_forward_module_sources})
_merlin_module_identity(_bindless_module_identity ${_bindless_module_sources})

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

# The sources each module identity was built from, so a consumer can recompute
# the identity from the manifest instead of trusting it.
function(_merlin_module_sources_json _sources _output)
  set(_sorted ${_sources})
  list(SORT _sorted)
  set(_json "")
  foreach(_source IN LISTS _sorted)
    string(REPLACE ":" ";" _fields "${_source}")
    list(GET _fields 0 _path)
    list(GET _fields 1 _hash)
    if(NOT _json STREQUAL "")
      string(APPEND _json ", ")
    endif()
    string(APPEND _json "{\"path\": \"${_path}\", \"sha256\": \"${_hash}\"}")
  endforeach()
  set(${_output} "[${_json}]" PARENT_SCOPE)
endfunction()

set(_artifacts "")
file(STRINGS "${MERLIN_SHADER_RECORDS_FILE}" _records)
foreach(_record IN LISTS _records)
  string(REPLACE "|" ";" _fields "${_record}")
  list(LENGTH _fields _field_count)
  if(NOT _field_count EQUAL 10)
    message(FATAL_ERROR "Malformed shader record: ${_record}")
  endif()
  list(GET _fields 0 _artifact)
  list(GET _fields 1 _reflection)
  list(GET _fields 2 _source)
  list(GET _fields 3 _entry)
  list(GET _fields 4 _stage)
  list(GET _fields 5 _target)
  list(GET _fields 6 _profile)
  list(GET _fields 7 _capabilities)
  list(GET _fields 8 _permutation)
  list(GET _fields 9 _features)

  foreach(_input "${_artifact}" "${_reflection}")
    if(NOT EXISTS "${_input}")
      message(FATAL_ERROR "Shader manifest input is missing: ${_input}")
    endif()
  endforeach()

  if(_source STREQUAL "forward.slang")
    set(_module_identity "${_forward_module_identity}")
    set(_module_sources ${_forward_module_sources})
  elseif(_source STREQUAL "forward-bindless.slang")
    set(_module_identity "${_bindless_module_identity}")
    set(_module_sources ${_bindless_module_sources})
  else()
    message(FATAL_ERROR "Shader record names an unknown source: ${_source}")
  endif()

  file(SHA256 "${_artifact}" _artifact_hash)
  file(SHA256 "${_reflection}" _reflection_hash)
  get_filename_component(_artifact_name "${_artifact}" NAME)
  get_filename_component(_reflection_name "${_reflection}" NAME)
  _merlin_features_json("${_features}" _features_json)
  _merlin_module_sources_json("${_module_sources}" _module_sources_json)

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
  _merlin_identity_field(_key_record "debug-info" "false")
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

_merlin_json_escape("${MERLIN_CMAKE_GENERATOR}" _generator)
_merlin_json_escape("${MERLIN_VULKAN_SDK_VERSION}" _sdk_version)
_merlin_json_escape("${MERLIN_SLANG_VERSION}" _slang_version)
set(_manifest
"{\n  \"schema_version\": ${MERLIN_SHADER_SCHEMA_VERSION},\n  \"shader_abi_version\": ${MERLIN_SHADER_ABI_VERSION},\n  \"cache_compatibility\": {\n    \"algorithm\": \"sha256\",\n    \"rule\": \"all cache-key inputs must match exactly\",\n    \"module_identity_schema\": \"${MERLIN_SHADER_MODULE_IDENTITY_SCHEMA}\",\n    \"artifact_key_schema\": \"${MERLIN_SHADER_ARTIFACT_KEY_SCHEMA}\"\n  },\n  \"toolchain\": {\n    \"compiler\": \"slangc\",\n    \"compiler_version\": \"${_slang_version}\",\n    \"required_series\": \"${MERLIN_SLANG_REQUIRED_SERIES}\",\n    \"vulkan_sdk_version\": \"${_sdk_version}\",\n    \"generator\": \"CMake ${CMAKE_VERSION} / ${_generator}\"\n  },\n  \"policy\": {\n    \"matrix_layout\": \"${MERLIN_SLANG_MATRIX_LAYOUT}\",\n    \"optimization\": \"${MERLIN_SLANG_OPTIMIZATION}\",\n    \"debug_info\": false\n  },\n  \"sources\": [\n    {\"path\": \"forward-common.slang\", \"sha256\": \"${_common_hash}\"},\n    {\"path\": \"forward.slang\", \"sha256\": \"${_forward_hash}\"},\n    {\"path\": \"forward-bindless.slang\", \"sha256\": \"${_bindless_hash}\"}\n  ],\n  \"environment\": {\n    \"path\": \"environment.hdr\",\n    \"sha256\": \"${_environment_hash}\",\n    \"representation\": \"diffuse-sh-l2\"\n  },\n  \"artifacts\": [\n${_artifacts}\n  ],\n  \"unsupported_features\": [\n    {\n      \"target\": \"metal\",\n      \"feature\": \"non_uniform_resource_indexing\",\n      \"diagnostic\": \"Slang reports NonUniformResourceIndex unavailable for the Metal fragment target\",\n      \"fallback\": \"forward-conventional\"\n    }\n  ]\n}\n")

get_filename_component(_manifest_dir "${MERLIN_SHADER_MANIFEST}" DIRECTORY)
file(MAKE_DIRECTORY "${_manifest_dir}")
file(WRITE "${MERLIN_SHADER_MANIFEST}" "${_manifest}")
