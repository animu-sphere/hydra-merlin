add_library(merlin-materialx STATIC
  src/compiler.cpp
  src/diagnostic_bridge.cpp
)

add_library(Merlin::MaterialX ALIAS merlin-materialx)

set_target_properties(merlin-materialx PROPERTIES
  EXPORT_NAME MaterialX
)

target_include_directories(merlin-materialx
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    src
)

target_link_libraries(merlin-materialx
  PUBLIC
    Merlin::RenderWorld
  PRIVATE
    MaterialXFormat
    MaterialXGenSlang
)
target_compile_features(merlin-materialx PUBLIC cxx_std_20)
target_compile_definitions(merlin-materialx PRIVATE
  MERLIN_MATERIALX_GENSLANG_REVISION="${MERLIN_MATERIALX_GENSLANG_REVISION}"
)

# Build-only helper used to materialize the accepted cross-target artifact
# slice. It is intentionally not installed or exported as part of the public
# SDK.
add_executable(merlin-materialx-artifact-generator EXCLUDE_FROM_ALL
  tools/artifact_generator.cpp
)
target_link_libraries(merlin-materialx-artifact-generator
  PRIVATE Merlin::MaterialX
)
target_compile_features(merlin-materialx-artifact-generator PRIVATE cxx_std_20)
