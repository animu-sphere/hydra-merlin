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

function(_merlin_compile_spirv _output _source _entry _stage _capabilities)
  set(_reflection "${_output}.reflection.json")
  set(_depfile "${_output}.d")
  add_custom_command(
    OUTPUT "${_output}" "${_reflection}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MERLIN_SHADER_OUTPUT_DIR}"
    COMMAND "${MERLIN_SLANGC_EXECUTABLE}" "${_source}"
            -entry "${_entry}" -stage "${_stage}"
            -target spirv -profile sm_6_6
            -capability "${_capabilities}"
            -matrix-layout-column-major -O2 -warnings-as-errors all
            -reflection-json "${_reflection}"
            -depfile "${_depfile}"
            -o "${_output}"
    DEPENDS
      "${_merlin_shader_source_dir}/${_source}"
      "${_merlin_shader_common}"
    DEPFILE "${_depfile}"
    WORKING_DIRECTORY "${_merlin_shader_source_dir}"
    COMMENT "Slang SPIR-V ${_entry}"
    VERBATIM
  )
endfunction()

function(_merlin_compile_metal _output _source _entry _stage)
  set(_reflection "${_output}.reflection.json")
  set(_depfile "${_output}.d")
  add_custom_command(
    OUTPUT "${_output}" "${_reflection}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MERLIN_SHADER_OUTPUT_DIR}"
    COMMAND "${MERLIN_SLANGC_EXECUTABLE}" "${_source}"
            -entry "${_entry}" -stage "${_stage}"
            -target metal -profile metallib_2_4
            -matrix-layout-column-major -O2 -warnings-as-errors all
            -reflection-json "${_reflection}"
            -depfile "${_depfile}"
            -o "${_output}"
    DEPENDS
      "${_merlin_shader_source_dir}/${_source}"
      "${_merlin_shader_common}"
    DEPFILE "${_depfile}"
    WORKING_DIRECTORY "${_merlin_shader_source_dir}"
    COMMENT "Slang Metal ${_entry}"
    VERBATIM
  )
endfunction()

_merlin_compile_spirv("${MERLIN_VERTEX_SPV}" forward.slang
  forward_vertex vertex spirv_1_5)
_merlin_compile_spirv("${MERLIN_FRAGMENT_SPV}" forward.slang
  forward_fragment fragment spirv_1_5)
_merlin_compile_spirv("${MERLIN_BINDLESS_VERTEX_SPV}"
  forward-bindless.slang forward_bindless_vertex vertex
  spirv_1_5+spvShaderNonUniformEXT)
_merlin_compile_spirv("${MERLIN_BINDLESS_FRAGMENT_SPV}"
  forward-bindless.slang forward_bindless_fragment fragment
  spirv_1_5+spvShaderNonUniformEXT)
_merlin_compile_metal("${MERLIN_VERTEX_METAL}" forward.slang
  forward_vertex vertex)
_merlin_compile_metal("${MERLIN_FRAGMENT_METAL}" forward.slang
  forward_fragment fragment)

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
    "-DMERLIN_SLANG_VERSION:STRING=${_merlin_slang_version}"
    "-DMERLIN_SLANG_REQUIRED_SERIES:STRING=${MERLIN_SLANG_REQUIRED_SERIES}"
    "-DMERLIN_VULKAN_SDK_VERSION:STRING=${_merlin_vulkan_sdk_version}"
    "-DMERLIN_CMAKE_GENERATOR:STRING=${CMAKE_GENERATOR}"
    "-DMERLIN_FORWARD_SOURCE:FILEPATH=${_merlin_shader_forward}"
    "-DMERLIN_BINDLESS_SOURCE:FILEPATH=${_merlin_shader_bindless}"
    "-DMERLIN_COMMON_SOURCE:FILEPATH=${_merlin_shader_common}"
    "-DMERLIN_FORWARD_VERTEX_SPV:FILEPATH=${MERLIN_VERTEX_SPV}"
    "-DMERLIN_FORWARD_FRAGMENT_SPV:FILEPATH=${MERLIN_FRAGMENT_SPV}"
    "-DMERLIN_BINDLESS_VERTEX_SPV:FILEPATH=${MERLIN_BINDLESS_VERTEX_SPV}"
    "-DMERLIN_BINDLESS_FRAGMENT_SPV:FILEPATH=${MERLIN_BINDLESS_FRAGMENT_SPV}"
    "-DMERLIN_FORWARD_VERTEX_METAL:FILEPATH=${MERLIN_VERTEX_METAL}"
    "-DMERLIN_FORWARD_FRAGMENT_METAL:FILEPATH=${MERLIN_FRAGMENT_METAL}"
    "-DMERLIN_FORWARD_VERTEX_REFLECTION:FILEPATH=${MERLIN_VERTEX_SPV}.reflection.json"
    "-DMERLIN_FORWARD_FRAGMENT_REFLECTION:FILEPATH=${MERLIN_FRAGMENT_SPV}.reflection.json"
    "-DMERLIN_BINDLESS_VERTEX_REFLECTION:FILEPATH=${MERLIN_BINDLESS_VERTEX_SPV}.reflection.json"
    "-DMERLIN_BINDLESS_FRAGMENT_REFLECTION:FILEPATH=${MERLIN_BINDLESS_FRAGMENT_SPV}.reflection.json"
    "-DMERLIN_FORWARD_VERTEX_METAL_REFLECTION:FILEPATH=${MERLIN_VERTEX_METAL}.reflection.json"
    "-DMERLIN_FORWARD_FRAGMENT_METAL_REFLECTION:FILEPATH=${MERLIN_FRAGMENT_METAL}.reflection.json"
    -P "${PROJECT_SOURCE_DIR}/cmake/write-shader-manifest.cmake"
  DEPENDS
    "${MERLIN_VERTEX_SPV}" "${MERLIN_FRAGMENT_SPV}"
    "${MERLIN_BINDLESS_VERTEX_SPV}" "${MERLIN_BINDLESS_FRAGMENT_SPV}"
    "${MERLIN_VERTEX_METAL}" "${MERLIN_FRAGMENT_METAL}"
    ${_merlin_shader_reflections}
    "${_merlin_shader_forward}" "${_merlin_shader_bindless}"
    "${_merlin_shader_common}"
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
