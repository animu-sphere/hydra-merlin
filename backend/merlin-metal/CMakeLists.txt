if(NOT APPLE)
  message(FATAL_ERROR "MERLIN_ENABLE_METAL requires an Apple platform")
endif()

find_library(MERLIN_METAL_FRAMEWORK Metal REQUIRED)
find_library(MERLIN_FOUNDATION_FRAMEWORK Foundation REQUIRED)
find_library(MERLIN_QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)

add_library(merlin-metal STATIC
  src/backend.mm
  src/resource_table.cpp
)
add_library(Merlin::Metal ALIAS merlin-metal)

set_target_properties(merlin-metal PROPERTIES
  EXPORT_NAME Metal
  OBJCXX_STANDARD 20
  OBJCXX_STANDARD_REQUIRED ON
  OBJCXX_EXTENSIONS OFF
)

target_include_directories(merlin-metal
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(merlin-metal
  PUBLIC
    Merlin::RenderBackend
    Merlin::RenderExtraction
  PRIVATE
    "${MERLIN_METAL_FRAMEWORK}"
    "${MERLIN_FOUNDATION_FRAMEWORK}"
    "${MERLIN_QUARTZCORE_FRAMEWORK}"
)
target_compile_features(merlin-metal PUBLIC cxx_std_20)
target_compile_options(merlin-metal PRIVATE
  $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
)
