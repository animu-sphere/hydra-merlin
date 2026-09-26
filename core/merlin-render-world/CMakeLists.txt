add_library(merlin-render-world STATIC
  src/gaussian.cpp
  src/identity.cpp
  src/material_abi.cpp
  src/material_diagnostic.cpp
  src/render_world.cpp
  src/shader_artifact.cpp
)
merlin_target_defaults(merlin-render-world)

add_library(Merlin::RenderWorld ALIAS merlin-render-world)

set_target_properties(merlin-render-world PROPERTIES
  EXPORT_NAME RenderWorld
)

target_include_directories(merlin-render-world
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(merlin-render-world PUBLIC cxx_std_20)
