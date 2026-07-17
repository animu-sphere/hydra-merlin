find_package(Vulkan ${MERLIN_VULKAN_MIN_VERSION} REQUIRED)
set(MERLIN_VULKAN_DETECTED_VERSION "${Vulkan_VERSION}" PARENT_SCOPE)

find_program(MERLIN_SLANGC_EXECUTABLE
  NAMES slangc
  HINTS
    "$ENV{VULKAN_SDK}/Bin"
    "$ENV{VULKAN_SDK}/bin"
  DOC "Pinned Slang compiler used for Merlin shader artifacts"
  REQUIRED
)
execute_process(
  COMMAND "${MERLIN_SLANGC_EXECUTABLE}" -version
  RESULT_VARIABLE _merlin_slang_version_result
  OUTPUT_VARIABLE _merlin_slang_version
  ERROR_VARIABLE _merlin_slang_version_error
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_STRIP_TRAILING_WHITESPACE
)
if(NOT _merlin_slang_version_result EQUAL 0)
  message(FATAL_ERROR
    "slangc -version failed: ${_merlin_slang_version_error}")
endif()
if(_merlin_slang_version STREQUAL "")
  # Vulkan SDK builds of slangc currently print the version to stderr.
  set(_merlin_slang_version "${_merlin_slang_version_error}")
endif()
# Extract rather than match the whole output: slangc builds differ in whether
# they print a bare version, a "slangc version X" line, or a suffixed one.
if(NOT _merlin_slang_version MATCHES "([0-9]+[.][0-9]+([.][0-9]+)?)")
  message(FATAL_ERROR
    "could not read a version from 'slangc -version' at ${MERLIN_SLANGC_EXECUTABLE}: '${_merlin_slang_version}'")
endif()
set(_merlin_slang_version "${CMAKE_MATCH_1}")
string(REPLACE "." "[.]" _merlin_slang_series_regex
       "${MERLIN_SLANG_REQUIRED_SERIES}")
if(NOT _merlin_slang_version MATCHES
   "^${_merlin_slang_series_regex}([.][0-9]+)?$")
  message(FATAL_ERROR
    "hdMerlin requires slangc ${MERLIN_SLANG_REQUIRED_SERIES}.x; found '${_merlin_slang_version}' at ${MERLIN_SLANGC_EXECUTABLE}")
endif()
set(MERLIN_SLANG_DETECTED_VERSION
    "${_merlin_slang_version}" PARENT_SCOPE)

set(_merlin_vulkan_sdk_version "unknown")
if(DEFINED ENV{VULKAN_SDK})
  set(_merlin_vulkan_sdk_path "$ENV{VULKAN_SDK}")
  cmake_path(GET _merlin_vulkan_sdk_path FILENAME
             _merlin_vulkan_sdk_version)
endif()
set(MERLIN_VULKAN_SDK_DETECTED_VERSION
    "${_merlin_vulkan_sdk_version}" PARENT_SCOPE)

add_library(merlin-vulkan STATIC
  src/backend.cpp
  src/bindless_resource_table.cpp
  src/descriptor_indexing.cpp
  src/render_artifacts.cpp
  src/renderer.cpp
)

add_library(Merlin::Vulkan ALIAS merlin-vulkan)

set_target_properties(merlin-vulkan PROPERTIES
  EXPORT_NAME Vulkan
)

target_include_directories(merlin-vulkan
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(merlin-vulkan
  PUBLIC
    Merlin::RenderBackend
    Merlin::RenderExtraction
    Vulkan::Vulkan
)
target_compile_features(merlin-vulkan PUBLIC cxx_std_20)
target_compile_definitions(merlin-vulkan PRIVATE
  MERLIN_VULKAN_MIN_VERSION_MAJOR=${MERLIN_VULKAN_MIN_VERSION_MAJOR}
  MERLIN_VULKAN_MIN_VERSION_MINOR=${MERLIN_VULKAN_MIN_VERSION_MINOR}
  MERLIN_VULKAN_MIN_VERSION_STRING="${MERLIN_VULKAN_MIN_VERSION}"
  MERLIN_VULKAN_SDK_VERSION="${_merlin_vulkan_sdk_version}"
  MERLIN_SLANG_VERSION="${_merlin_slang_version}"
  MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION=${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}
)

set(MERLIN_SHADER_OUTPUT_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}")
set(MERLIN_VERTEX_SPV "${MERLIN_SHADER_OUTPUT_DIR}/triangle.vert.spv")
set(MERLIN_FRAGMENT_SPV "${MERLIN_SHADER_OUTPUT_DIR}/triangle.frag.spv")
set(MERLIN_BINDLESS_VERTEX_SPV
    "${MERLIN_SHADER_OUTPUT_DIR}/triangle.bindless.vert.spv")
set(MERLIN_BINDLESS_FRAGMENT_SPV
    "${MERLIN_SHADER_OUTPUT_DIR}/triangle.bindless.frag.spv")
set(MERLIN_VERTEX_METAL "${MERLIN_SHADER_OUTPUT_DIR}/triangle.vert.metal")
set(MERLIN_FRAGMENT_METAL "${MERLIN_SHADER_OUTPUT_DIR}/triangle.frag.metal")

set(_merlin_shader_source_dir "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(_merlin_shader_common
    "${_merlin_shader_source_dir}/forward-common.slang")
set(_merlin_shader_forward
    "${_merlin_shader_source_dir}/forward.slang")
set(_merlin_shader_bindless
    "${_merlin_shader_source_dir}/forward-bindless.slang")

# Compile policy shared by every target. These feed both the slangc command
# line and the manifest cache keys, so changing one moves the other.
set(_merlin_slang_spirv_profile "sm_6_6")
set(_merlin_slang_metal_profile "metallib_2_4")
set(_merlin_slang_matrix_layout "column-major")
set(_merlin_slang_optimization "O2")

set(_merlin_conventional_features "material_constants+base_color_texture")
set(_merlin_bindless_features
    "${_merlin_conventional_features}+bindless_resources+non_uniform_resource_indexing")

# _capabilities is the literal -capability argument, or "none" when the target
# takes no capability flag. Every field below is also recorded verbatim for the
# manifest, so the provenance and the compile share one source of truth.
function(_merlin_compile_shader _output _source _entry _stage _target _profile
                               _capabilities _permutation _features)
  set(_reflection "${_output}.reflection.json")
  set(_depfile "${_output}.d")
  set(_capability_arguments "")
  if(NOT _capabilities STREQUAL "none")
    set(_capability_arguments -capability "${_capabilities}")
  endif()
  add_custom_command(
    OUTPUT "${_output}" "${_reflection}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MERLIN_SHADER_OUTPUT_DIR}"
    COMMAND "${MERLIN_SLANGC_EXECUTABLE}" "${_source}"
            -entry "${_entry}" -stage "${_stage}"
            -target "${_target}" -profile "${_profile}"
            ${_capability_arguments}
            "-matrix-layout-${_merlin_slang_matrix_layout}"
            "-${_merlin_slang_optimization}"
            -warnings-as-errors all
            -reflection-json "${_reflection}"
            -depfile "${_depfile}"
            -o "${_output}"
    DEPENDS
      "${_merlin_shader_source_dir}/${_source}"
      "${_merlin_shader_common}"
    DEPFILE "${_depfile}"
    WORKING_DIRECTORY "${_merlin_shader_source_dir}"
    COMMENT "Slang ${_target} ${_entry}"
    VERBATIM
  )
  set_property(GLOBAL APPEND PROPERTY MERLIN_SHADER_RECORDS
    "${_output}|${_reflection}|${_source}|${_entry}|${_stage}|${_target}|${_profile}|${_capabilities}|${_permutation}|${_features}")
endfunction()

_merlin_compile_shader("${MERLIN_VERTEX_SPV}" forward.slang
  forward_vertex vertex spirv "${_merlin_slang_spirv_profile}"
  spirv_1_5 forward-conventional "${_merlin_conventional_features}")
_merlin_compile_shader("${MERLIN_FRAGMENT_SPV}" forward.slang
  forward_fragment fragment spirv "${_merlin_slang_spirv_profile}"
  spirv_1_5 forward-conventional "${_merlin_conventional_features}")
_merlin_compile_shader("${MERLIN_BINDLESS_VERTEX_SPV}" forward-bindless.slang
  forward_bindless_vertex vertex spirv "${_merlin_slang_spirv_profile}"
  spirv_1_5+spvShaderNonUniformEXT forward-bindless
  "${_merlin_bindless_features}")
_merlin_compile_shader("${MERLIN_BINDLESS_FRAGMENT_SPV}" forward-bindless.slang
  forward_bindless_fragment fragment spirv "${_merlin_slang_spirv_profile}"
  spirv_1_5+spvShaderNonUniformEXT forward-bindless
  "${_merlin_bindless_features}")
_merlin_compile_shader("${MERLIN_VERTEX_METAL}" forward.slang
  forward_vertex vertex metal "${_merlin_slang_metal_profile}"
  none forward-conventional "${_merlin_conventional_features}")
_merlin_compile_shader("${MERLIN_FRAGMENT_METAL}" forward.slang
  forward_fragment fragment metal "${_merlin_slang_metal_profile}"
  none forward-conventional "${_merlin_conventional_features}")

get_property(_merlin_shader_records GLOBAL PROPERTY MERLIN_SHADER_RECORDS)
list(JOIN _merlin_shader_records "\n" _merlin_shader_records_text)
set(_merlin_shader_records_file
    "${CMAKE_CURRENT_BINARY_DIR}/shader-records.txt")
file(GENERATE OUTPUT "${_merlin_shader_records_file}"
  CONTENT "${_merlin_shader_records_text}\n")

set(MERLIN_SHADER_MANIFEST "${MERLIN_SHADER_OUTPUT_DIR}/manifest.json")
set(_merlin_shader_reflections
  "${MERLIN_VERTEX_SPV}.reflection.json"
  "${MERLIN_FRAGMENT_SPV}.reflection.json"
  "${MERLIN_BINDLESS_VERTEX_SPV}.reflection.json"
  "${MERLIN_BINDLESS_FRAGMENT_SPV}.reflection.json"
  "${MERLIN_VERTEX_METAL}.reflection.json"
  "${MERLIN_FRAGMENT_METAL}.reflection.json"
)
add_custom_command(
  OUTPUT "${MERLIN_SHADER_MANIFEST}"
  COMMAND ${CMAKE_COMMAND}
    "-DMERLIN_SHADER_MANIFEST:FILEPATH=${MERLIN_SHADER_MANIFEST}"
    "-DMERLIN_SHADER_SCHEMA_VERSION:STRING=${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}"
    "-DMERLIN_SHADER_ABI_VERSION:STRING=${MERLIN_SHADER_ABI_VERSION}"
    "-DMERLIN_SHADER_RECORDS_FILE:FILEPATH=${_merlin_shader_records_file}"
    "-DMERLIN_SLANG_VERSION:STRING=${_merlin_slang_version}"
    "-DMERLIN_SLANG_REQUIRED_SERIES:STRING=${MERLIN_SLANG_REQUIRED_SERIES}"
    "-DMERLIN_SLANG_MATRIX_LAYOUT:STRING=${_merlin_slang_matrix_layout}"
    "-DMERLIN_SLANG_OPTIMIZATION:STRING=${_merlin_slang_optimization}"
    "-DMERLIN_VULKAN_SDK_VERSION:STRING=${_merlin_vulkan_sdk_version}"
    "-DMERLIN_CMAKE_GENERATOR:STRING=${CMAKE_GENERATOR}"
    "-DMERLIN_FORWARD_SOURCE:FILEPATH=${_merlin_shader_forward}"
    "-DMERLIN_BINDLESS_SOURCE:FILEPATH=${_merlin_shader_bindless}"
    "-DMERLIN_COMMON_SOURCE:FILEPATH=${_merlin_shader_common}"
    -P "${PROJECT_SOURCE_DIR}/cmake/write-shader-manifest.cmake"
  DEPENDS
    "${MERLIN_VERTEX_SPV}" "${MERLIN_FRAGMENT_SPV}"
    "${MERLIN_BINDLESS_VERTEX_SPV}" "${MERLIN_BINDLESS_FRAGMENT_SPV}"
    "${MERLIN_VERTEX_METAL}" "${MERLIN_FRAGMENT_METAL}"
    ${_merlin_shader_reflections}
    "${_merlin_shader_forward}" "${_merlin_shader_bindless}"
    "${_merlin_shader_common}"
    "${_merlin_shader_records_file}"
    "${PROJECT_SOURCE_DIR}/cmake/write-shader-manifest.cmake"
  COMMENT "Writing deterministic shader artifact manifest"
  VERBATIM
)

add_custom_target(merlin-vulkan-shaders
  DEPENDS
    "${MERLIN_VERTEX_SPV}"
    "${MERLIN_FRAGMENT_SPV}"
    "${MERLIN_BINDLESS_VERTEX_SPV}"
    "${MERLIN_BINDLESS_FRAGMENT_SPV}"
    "${MERLIN_VERTEX_METAL}"
    "${MERLIN_FRAGMENT_METAL}"
    ${_merlin_shader_reflections}
    "${MERLIN_SHADER_MANIFEST}"
)
add_dependencies(merlin-vulkan merlin-vulkan-shaders)

set(_merlin_shader_package_files
  "${MERLIN_VERTEX_SPV}"
  "${MERLIN_FRAGMENT_SPV}"
  "${MERLIN_BINDLESS_VERTEX_SPV}"
  "${MERLIN_BINDLESS_FRAGMENT_SPV}"
  "${MERLIN_VERTEX_METAL}"
  "${MERLIN_FRAGMENT_METAL}"
  ${_merlin_shader_reflections}
  "${MERLIN_SHADER_MANIFEST}"
)
install(FILES ${_merlin_shader_package_files}
  DESTINATION
    "${CMAKE_INSTALL_BINDIR}/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}")

set(MERLIN_VERTEX_SPV "${MERLIN_VERTEX_SPV}" PARENT_SCOPE)
set(MERLIN_FRAGMENT_SPV "${MERLIN_FRAGMENT_SPV}" PARENT_SCOPE)
set(MERLIN_BINDLESS_VERTEX_SPV "${MERLIN_BINDLESS_VERTEX_SPV}" PARENT_SCOPE)
set(MERLIN_BINDLESS_FRAGMENT_SPV
    "${MERLIN_BINDLESS_FRAGMENT_SPV}" PARENT_SCOPE)
set(MERLIN_VERTEX_METAL "${MERLIN_VERTEX_METAL}" PARENT_SCOPE)
set(MERLIN_FRAGMENT_METAL "${MERLIN_FRAGMENT_METAL}" PARENT_SCOPE)
set(MERLIN_SHADER_REFLECTIONS "${_merlin_shader_reflections}" PARENT_SCOPE)
set(MERLIN_SHADER_MANIFEST "${MERLIN_SHADER_MANIFEST}" PARENT_SCOPE)
set(MERLIN_SHADER_PACKAGE_FILES
    "${_merlin_shader_package_files}" PARENT_SCOPE)
set(MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION
    "${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}" PARENT_SCOPE)
