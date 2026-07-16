if(NOT DEFINED MERLIN_SHADER_MANIFEST OR
   NOT DEFINED MERLIN_SHADER_SCHEMA_VERSION OR
   NOT DEFINED MERLIN_SHADER_ABI_VERSION OR
   NOT DEFINED MERLIN_SLANG_VERSION OR
   NOT DEFINED MERLIN_SLANG_REQUIRED_SERIES OR
   NOT DEFINED MERLIN_VULKAN_SDK_VERSION OR
   NOT DEFINED MERLIN_CMAKE_GENERATOR OR
   NOT DEFINED MERLIN_FORWARD_SOURCE OR
   NOT DEFINED MERLIN_BINDLESS_SOURCE OR
   NOT DEFINED MERLIN_COMMON_SOURCE)
  message(FATAL_ERROR "Missing shader manifest generation argument")
endif()

foreach(_required_file
    MERLIN_FORWARD_VERTEX_SPV MERLIN_FORWARD_FRAGMENT_SPV
    MERLIN_BINDLESS_VERTEX_SPV MERLIN_BINDLESS_FRAGMENT_SPV
    MERLIN_FORWARD_VERTEX_METAL MERLIN_FORWARD_FRAGMENT_METAL
    MERLIN_FORWARD_VERTEX_REFLECTION MERLIN_FORWARD_FRAGMENT_REFLECTION
    MERLIN_BINDLESS_VERTEX_REFLECTION MERLIN_BINDLESS_FRAGMENT_REFLECTION
    MERLIN_FORWARD_VERTEX_METAL_REFLECTION
    MERLIN_FORWARD_FRAGMENT_METAL_REFLECTION
    MERLIN_FORWARD_SOURCE MERLIN_BINDLESS_SOURCE MERLIN_COMMON_SOURCE)
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

file(SHA256 "${MERLIN_COMMON_SOURCE}" _common_hash)
file(SHA256 "${MERLIN_FORWARD_SOURCE}" _forward_hash)
file(SHA256 "${MERLIN_BINDLESS_SOURCE}" _bindless_hash)
string(SHA256 _forward_dependency_hash
  "forward-common.slang:${_common_hash}|forward.slang:${_forward_hash}")
string(SHA256 _bindless_dependency_hash
  "forward-common.slang:${_common_hash}|forward-bindless.slang:${_bindless_hash}")

function(_merlin_artifact_json _output _artifact _reflection _source
         _dependency_hash _target _profile _capabilities _entry _stage
         _permutation _features)
  file(SHA256 "${_artifact}" _artifact_hash)
  file(SHA256 "${_reflection}" _reflection_hash)
  get_filename_component(_artifact_name "${_artifact}" NAME)
  get_filename_component(_reflection_name "${_reflection}" NAME)
  string(SHA256 _cache_key
    "schema=${MERLIN_SHADER_SCHEMA_VERSION}|abi=${MERLIN_SHADER_ABI_VERSION}|compiler=slangc-${MERLIN_SLANG_VERSION}|target=${_target}|profile=${_profile}|capabilities=${_capabilities}|matrix=column-major|optimization=O2|entry=${_entry}|stage=${_stage}|permutation=${_permutation}|dependencies=${_dependency_hash}")
  set(_json
    "    {\n      \"path\": \"${_artifact_name}\",\n      \"reflection\": \"${_reflection_name}\",\n      \"source\": \"${_source}\",\n      \"entry_point\": \"${_entry}\",\n      \"stage\": \"${_stage}\",\n      \"target\": \"${_target}\",\n      \"profile\": \"${_profile}\",\n      \"capabilities\": \"${_capabilities}\",\n      \"permutation\": \"${_permutation}\",\n      \"features\": ${_features},\n      \"dependency_sha256\": \"${_dependency_hash}\",\n      \"artifact_sha256\": \"${_artifact_hash}\",\n      \"reflection_sha256\": \"${_reflection_hash}\",\n      \"cache_key\": \"${_cache_key}\"\n    }")
  set(${_output} "${_json}" PARENT_SCOPE)
endfunction()

set(_conventional_features
  "[\"material_constants\", \"base_color_texture\"]")
set(_bindless_features
  "[\"material_constants\", \"base_color_texture\", \"bindless_resources\", \"non_uniform_resource_indexing\"]")

_merlin_artifact_json(_item "${MERLIN_FORWARD_VERTEX_SPV}"
  "${MERLIN_FORWARD_VERTEX_REFLECTION}" "forward.slang"
  "${_forward_dependency_hash}" "spirv" "sm_6_6+spirv_1_5"
  "spirv_1_5" "forward_vertex" "vertex" "forward-conventional"
  "${_conventional_features}")
set(_artifacts "${_item}")
_merlin_artifact_json(_item "${MERLIN_FORWARD_FRAGMENT_SPV}"
  "${MERLIN_FORWARD_FRAGMENT_REFLECTION}" "forward.slang"
  "${_forward_dependency_hash}" "spirv" "sm_6_6+spirv_1_5"
  "spirv_1_5" "forward_fragment" "fragment" "forward-conventional"
  "${_conventional_features}")
string(APPEND _artifacts ",\n${_item}")
_merlin_artifact_json(_item "${MERLIN_BINDLESS_VERTEX_SPV}"
  "${MERLIN_BINDLESS_VERTEX_REFLECTION}" "forward-bindless.slang"
  "${_bindless_dependency_hash}" "spirv" "sm_6_6+spirv_1_5"
  "spirv_1_5+spvShaderNonUniformEXT" "forward_bindless_vertex" "vertex"
  "forward-bindless" "${_bindless_features}")
string(APPEND _artifacts ",\n${_item}")
_merlin_artifact_json(_item "${MERLIN_BINDLESS_FRAGMENT_SPV}"
  "${MERLIN_BINDLESS_FRAGMENT_REFLECTION}" "forward-bindless.slang"
  "${_bindless_dependency_hash}" "spirv" "sm_6_6+spirv_1_5"
  "spirv_1_5+spvShaderNonUniformEXT" "forward_bindless_fragment" "fragment"
  "forward-bindless" "${_bindless_features}")
string(APPEND _artifacts ",\n${_item}")
_merlin_artifact_json(_item "${MERLIN_FORWARD_VERTEX_METAL}"
  "${MERLIN_FORWARD_VERTEX_METAL_REFLECTION}" "forward.slang"
  "${_forward_dependency_hash}" "metal" "metallib_2_4" "metal"
  "forward_vertex" "vertex" "forward-conventional"
  "${_conventional_features}")
string(APPEND _artifacts ",\n${_item}")
_merlin_artifact_json(_item "${MERLIN_FORWARD_FRAGMENT_METAL}"
  "${MERLIN_FORWARD_FRAGMENT_METAL_REFLECTION}" "forward.slang"
  "${_forward_dependency_hash}" "metal" "metallib_2_4" "metal"
  "forward_fragment" "fragment" "forward-conventional"
  "${_conventional_features}")
string(APPEND _artifacts ",\n${_item}")

_merlin_json_escape("${MERLIN_CMAKE_GENERATOR}" _generator)
_merlin_json_escape("${MERLIN_VULKAN_SDK_VERSION}" _sdk_version)
set(_manifest
"{\n  \"schema_version\": ${MERLIN_SHADER_SCHEMA_VERSION},\n  \"shader_abi_version\": ${MERLIN_SHADER_ABI_VERSION},\n  \"cache_compatibility\": {\n    \"algorithm\": \"sha256\",\n    \"rule\": \"all cache-key inputs must match exactly\"\n  },\n  \"toolchain\": {\n    \"compiler\": \"slangc\",\n    \"compiler_version\": \"${MERLIN_SLANG_VERSION}\",\n    \"required_series\": \"${MERLIN_SLANG_REQUIRED_SERIES}\",\n    \"vulkan_sdk_version\": \"${_sdk_version}\",\n    \"generator\": \"CMake ${CMAKE_VERSION} / ${_generator}\"\n  },\n  \"policy\": {\n    \"matrix_layout\": \"column-major\",\n    \"optimization\": \"O2\",\n    \"debug_info\": false\n  },\n  \"sources\": [\n    {\"path\": \"forward-common.slang\", \"sha256\": \"${_common_hash}\"},\n    {\"path\": \"forward.slang\", \"sha256\": \"${_forward_hash}\"},\n    {\"path\": \"forward-bindless.slang\", \"sha256\": \"${_bindless_hash}\"}\n  ],\n  \"artifacts\": [\n${_artifacts}\n  ],\n  \"unsupported_features\": [\n    {\n      \"target\": \"metal\",\n      \"feature\": \"non_uniform_resource_indexing\",\n      \"diagnostic\": \"Slang reports NonUniformResourceIndex unavailable for the Metal fragment target\",\n      \"fallback\": \"forward-conventional\"\n    }\n  ]\n}\n")

get_filename_component(_manifest_dir "${MERLIN_SHADER_MANIFEST}" DIRECTORY)
file(MAKE_DIRECTORY "${_manifest_dir}")
file(WRITE "${MERLIN_SHADER_MANIFEST}" "${_manifest}")
