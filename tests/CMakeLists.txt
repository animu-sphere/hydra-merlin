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
            --output "${CMAKE_CURRENT_BINARY_DIR}/merlin-validation.ppm"
  )
  set_tests_properties(merlin-vulkan-validation PROPERTIES
    LABELS "gpu;validation"
    SKIP_RETURN_CODE 77
    TIMEOUT 60
  )
endif()

add_test(
  NAME merlin-install-consumer
  COMMAND ${CMAKE_COMMAND}
    "-DMERLIN_CMAKE_COMMAND=${CMAKE_COMMAND}"
    "-DMERLIN_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DMERLIN_BUILD_DIR=${PROJECT_BINARY_DIR}"
    "-DMERLIN_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
    "-DMERLIN_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
    "-DMERLIN_GENERATOR=${CMAKE_GENERATOR}"
    "-DMERLIN_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
    "-DMERLIN_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
    "-DMERLIN_CONFIG=$<CONFIG>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/run_install_consumer.cmake"
)
set_tests_properties(merlin-install-consumer PROPERTIES
  LABELS packaging
  TIMEOUT 60
)
