add_library(merlin-materialx STATIC
  src/compiler.cpp
  src/sha256.cpp
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
