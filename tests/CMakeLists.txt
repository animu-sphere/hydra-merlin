# These tests assert on their invariants, so assert() must stay live in every
# configuration. CMake defines NDEBUG in the optimized configs; strip it for the
# test targets in this directory. Removing the flag (rather than appending
# -UNDEBUG) keeps the build free of MSVC warning D9025.
foreach(_config "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
  string(REGEX REPLACE "[-/]DNDEBUG( |$)" "\\1"
         "CMAKE_CXX_FLAGS${_config}" "${CMAKE_CXX_FLAGS${_config}}")
endforeach()

add_executable(merlin-render-world-tests
  render_world_test.cpp
)

target_link_libraries(merlin-render-world-tests PRIVATE Merlin::RenderWorld)

add_test(NAME merlin-render-world COMMAND merlin-render-world-tests)

add_executable(merlin-render-extraction-tests
  render_extraction_test.cpp
)

target_link_libraries(merlin-render-extraction-tests
  PRIVATE Merlin::RenderExtraction)

add_test(NAME merlin-render-extraction COMMAND merlin-render-extraction-tests)

add_executable(merlin-mesh-scale-regression-tests
  mesh_scale_regression_test.cpp
)
target_link_libraries(merlin-mesh-scale-regression-tests
  PRIVATE Merlin::RenderExtraction)
add_test(NAME merlin-mesh-scale-regression
         COMMAND merlin-mesh-scale-regression-tests)
set_tests_properties(merlin-mesh-scale-regression PROPERTIES
  LABELS "regression;scale"
  TIMEOUT 60)

if(TARGET Merlin::Vulkan)
  add_executable(merlin-render-product-contract-tests
    render_product_contract_test.cpp
  )
  target_link_libraries(merlin-render-product-contract-tests
    PRIVATE Merlin::Vulkan)
  add_test(NAME merlin-render-product-contract
           COMMAND merlin-render-product-contract-tests)
endif()

if(TARGET merlin-headless)
  add_executable(merlin-resource-update-tests
    resource_update_test.cpp
  )
  target_link_libraries(merlin-resource-update-tests
    PRIVATE Merlin::RenderExtraction Merlin::Vulkan)
  add_test(
    NAME merlin-vulkan-resource-update
    COMMAND merlin-resource-update-tests
            "$<TARGET_FILE_DIR:merlin-headless>/shaders"
  )
  set_tests_properties(merlin-vulkan-resource-update PROPERTIES
    LABELS "gpu"
    SKIP_RETURN_CODE 77
    TIMEOUT 60
  )
  add_test(
    NAME merlin-vulkan-offscreen
    COMMAND merlin-headless
            --width 64
            --height 64
            --frames 6
            --output "${CMAKE_CURRENT_BINARY_DIR}/merlin-smoke.ppm"
  )
  add_test(
    NAME merlin-vulkan-validation
    COMMAND merlin-headless
            --width 64
            --height 64
            --frames 64
            --validate
            --metadata "${CMAKE_CURRENT_BINARY_DIR}/merlin-vulkan-capabilities.json"
            --output "${CMAKE_CURRENT_BINARY_DIR}/merlin-validation.ppm"
  )
  set_tests_properties(merlin-vulkan-validation PROPERTIES
    LABELS "gpu;validation"
    SKIP_RETURN_CODE 77
    TIMEOUT 60
  )
endif()

if(TARGET merlin-benchmark)
  add_test(
    NAME merlin-benchmark-json
    COMMAND ${CMAKE_COMMAND}
      "-DMERLIN_BENCHMARK=$<TARGET_FILE:merlin-benchmark>"
      "-DMERLIN_BENCHMARK_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/merlin-benchmark.json"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/run_benchmark_test.cmake"
  )
  set_tests_properties(merlin-benchmark-json PROPERTIES
    LABELS "benchmark;gpu"
    TIMEOUT 60
  )
endif()

get_property(_merlin_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

add_test(
  NAME merlin-install-consumer
  COMMAND ${CMAKE_COMMAND}
    "-DMERLIN_CMAKE_COMMAND=${CMAKE_COMMAND}"
    "-DMERLIN_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DMERLIN_BUILD_DIR=${PROJECT_BINARY_DIR}"
    "-DMERLIN_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
    "-DMERLIN_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
    "-DMERLIN_INSTALL_BINDIR=${CMAKE_INSTALL_BINDIR}"
    "-DMERLIN_INSTALL_DATADIR=${CMAKE_INSTALL_DATADIR}"
    "-DMERLIN_EXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
    "-DMERLIN_EXPECTED_VERSION=${PROJECT_VERSION}"
    "-DMERLIN_GENERATOR=${CMAKE_GENERATOR}"
    "-DMERLIN_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
    "-DMERLIN_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
    "-DMERLIN_MULTI_CONFIG=${_merlin_is_multi_config}"
    "-DMERLIN_CONFIG=$<CONFIG>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/run_install_consumer.cmake"
)
set_tests_properties(merlin-install-consumer PROPERTIES
  LABELS packaging
  RUN_SERIAL TRUE
  TIMEOUT 60
)

add_test(
  NAME merlin-release-tooling
  COMMAND ${CMAKE_COMMAND}
    "-DMERLIN_CMAKE_COMMAND=${CMAKE_COMMAND}"
    "-DMERLIN_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DMERLIN_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/verify_release_tooling.cmake"
)
set_tests_properties(merlin-release-tooling PROPERTIES LABELS packaging)
