add_library(merlin-render-backend STATIC
  src/backend.cpp
)

add_library(Merlin::RenderBackend ALIAS merlin-render-backend)

set_target_properties(merlin-render-backend PROPERTIES
  EXPORT_NAME RenderBackend
)

target_include_directories(merlin-render-backend
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(merlin-render-backend PUBLIC Merlin::RenderExtraction)
target_compile_features(merlin-render-backend PUBLIC cxx_std_20)
