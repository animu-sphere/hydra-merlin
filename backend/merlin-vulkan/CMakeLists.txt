find_package(Vulkan ${MERLIN_VULKAN_MIN_VERSION} REQUIRED COMPONENTS glslc)
set(MERLIN_VULKAN_DETECTED_VERSION "${Vulkan_VERSION}" PARENT_SCOPE)

set(_merlin_vulkan_sdk_version "unknown")
if(DEFINED ENV{VULKAN_SDK})
  set(_merlin_vulkan_sdk_path "$ENV{VULKAN_SDK}")
  cmake_path(GET _merlin_vulkan_sdk_path FILENAME
             _merlin_vulkan_sdk_version)
endif()
set(MERLIN_VULKAN_SDK_DETECTED_VERSION
    "${_merlin_vulkan_sdk_version}" PARENT_SCOPE)

add_library(merlin-vulkan STATIC
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
)

set(MERLIN_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(MERLIN_VERTEX_SPV "${MERLIN_SHADER_OUTPUT_DIR}/triangle.vert.spv")
set(MERLIN_FRAGMENT_SPV "${MERLIN_SHADER_OUTPUT_DIR}/triangle.frag.spv")

add_custom_command(
  OUTPUT "${MERLIN_VERTEX_SPV}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${MERLIN_SHADER_OUTPUT_DIR}"
  COMMAND Vulkan::glslc -fshader-stage=vert
          "${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle.vert"
          -o "${MERLIN_VERTEX_SPV}"
  DEPENDS shaders/triangle.vert
  VERBATIM
)

add_custom_command(
  OUTPUT "${MERLIN_FRAGMENT_SPV}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${MERLIN_SHADER_OUTPUT_DIR}"
  COMMAND Vulkan::glslc -fshader-stage=frag
          "${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle.frag"
          -o "${MERLIN_FRAGMENT_SPV}"
  DEPENDS shaders/triangle.frag
  VERBATIM
)

add_custom_target(merlin-vulkan-shaders
  DEPENDS "${MERLIN_VERTEX_SPV}" "${MERLIN_FRAGMENT_SPV}"
)

set(MERLIN_VERTEX_SPV "${MERLIN_VERTEX_SPV}" PARENT_SCOPE)
set(MERLIN_FRAGMENT_SPV "${MERLIN_FRAGMENT_SPV}" PARENT_SCOPE)
