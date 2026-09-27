merlin_find_vulkan_dependencies()

add_library(merlin-vulkan STATIC
  src/backend.cpp
  src/bindless_resource_table.cpp
  src/descriptor_indexing.cpp
  src/environment_lighting.cpp
  src/render_artifacts.cpp
  src/renderer.cpp
)
merlin_target_defaults(merlin-vulkan)

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
  MERLIN_SHADER_ABI_VERSION=${MERLIN_SHADER_ABI_VERSION}
)

merlin_add_vulkan_shaders()
