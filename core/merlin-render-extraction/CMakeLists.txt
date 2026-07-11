add_library(merlin-render-extraction STATIC
  src/scene_extractor.cpp
)

add_library(Merlin::RenderExtraction ALIAS merlin-render-extraction)

set_target_properties(merlin-render-extraction PROPERTIES
  EXPORT_NAME RenderExtraction
)

target_include_directories(merlin-render-extraction
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(merlin-render-extraction PUBLIC Merlin::RenderWorld)
target_compile_features(merlin-render-extraction PUBLIC cxx_std_20)
