add_library(merlin-render-world STATIC
  src/render_world.cpp
)

add_library(Merlin::RenderWorld ALIAS merlin-render-world)

target_include_directories(merlin-render-world
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(merlin-render-world PUBLIC cxx_std_20)
