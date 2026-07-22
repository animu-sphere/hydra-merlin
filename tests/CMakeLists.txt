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

add_executable(merlin-shader-contract-tests
  shader_contract_test.cpp
)
target_link_libraries(merlin-shader-contract-tests
  PRIVATE Merlin::RenderWorld)
add_test(NAME merlin-shader-contract COMMAND merlin-shader-contract-tests)

add_executable(merlin-render-extraction-tests
  render_extraction_test.cpp
)

target_link_libraries(merlin-render-extraction-tests
  PRIVATE Merlin::RenderExtraction)

add_test(NAME merlin-render-extraction COMMAND merlin-render-extraction-tests)

add_executable(merlin-render-backend-tests
  backend_contract_test.cpp
)
target_link_libraries(merlin-render-backend-tests PRIVATE Merlin::RenderBackend)
add_test(NAME merlin-render-backend COMMAND merlin-render-backend-tests)

add_test(
  NAME merlin-backend-boundaries
  COMMAND ${CMAKE_COMMAND}
    "-DMERLIN_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
    -P "${PROJECT_SOURCE_DIR}/cmake/verify-backend-boundaries.cmake"
)
set_tests_properties(merlin-backend-boundaries PROPERTIES LABELS "architecture")

if(TARGET Merlin::MaterialX)
  add_executable(merlin-materialx-sha256-tests
    sha256_test.cpp
  )
  target_include_directories(merlin-materialx-sha256-tests PRIVATE
    "${PROJECT_SOURCE_DIR}/material/merlin-materialx/src"
  )
  target_link_libraries(merlin-materialx-sha256-tests
    PRIVATE Merlin::MaterialX)
  add_test(NAME merlin-materialx-sha256
           COMMAND merlin-materialx-sha256-tests)
  set_tests_properties(merlin-materialx-sha256 PROPERTIES
    LABELS "materials;materialx;cache"
  )

  add_executable(merlin-materialx-compiler-tests
    materialx_compiler_test.cpp
  )
  target_link_libraries(merlin-materialx-compiler-tests
    PRIVATE Merlin::MaterialX)
  add_test(
    NAME merlin-materialx-compiler
    COMMAND merlin-materialx-compiler-tests
      "${CMAKE_CURRENT_SOURCE_DIR}/data/materialx/prototype.mtlx"
      "${_merlin_materialx_data_root}"
      "${CMAKE_CURRENT_BINARY_DIR}/materialx-prototype.slang"
      "${CMAKE_CURRENT_SOURCE_DIR}/data/materialx/standard-surface.mtlx"
      "${CMAKE_CURRENT_BINARY_DIR}/materialx-standard-surface.slang"
  )
  set_tests_properties(merlin-materialx-compiler PROPERTIES
    LABELS "materials;materialx;shader"
    FIXTURES_SETUP merlin-materialx-generated-module
  )

  find_program(MERLIN_MATERIALX_TEST_SLANGC
    NAMES slangc
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
  )
  if(MERLIN_MATERIALX_TEST_SLANGC)
    add_test(
      NAME merlin-materialx-spirv-compile
      COMMAND "${MERLIN_MATERIALX_TEST_SLANGC}"
        "${CMAKE_CURRENT_SOURCE_DIR}/data/materialx/compile-wrapper.slang"
        -I "${CMAKE_CURRENT_BINARY_DIR}"
        -entry merlin_materialx_test_fragment
        -stage fragment
        -target spirv
        -profile sm_6_6
        -capability spirv_1_5
        -matrix-layout-column-major
        -O2
        -warnings-as-errors all
        -reflection-json
          "${CMAKE_CURRENT_BINARY_DIR}/materialx-prototype.spv.reflection.json"
        -o "${CMAKE_CURRENT_BINARY_DIR}/materialx-prototype.spv"
    )
    set_tests_properties(merlin-materialx-spirv-compile PROPERTIES
      LABELS "materials;materialx;shader;spirv"
      FIXTURES_REQUIRED merlin-materialx-generated-module
    )

    add_test(
      NAME merlin-materialx-metal-compile
      COMMAND "${MERLIN_MATERIALX_TEST_SLANGC}"
        "${CMAKE_CURRENT_SOURCE_DIR}/data/materialx/compile-wrapper.slang"
        -I "${CMAKE_CURRENT_BINARY_DIR}"
        -entry merlin_materialx_test_fragment
        -stage fragment
        -target metal
        -profile metallib_2_4
        -matrix-layout-column-major
        -O2
        -warnings-as-errors all
        -reflection-json
          "${CMAKE_CURRENT_BINARY_DIR}/materialx-prototype.metal.reflection.json"
        -o "${CMAKE_CURRENT_BINARY_DIR}/materialx-prototype.metal"
    )
    set_tests_properties(merlin-materialx-metal-compile PROPERTIES
      LABELS "materials;materialx;metal;shader"
      FIXTURES_REQUIRED merlin-materialx-generated-module
    )

    add_test(
      NAME merlin-materialx-standard-surface-spirv-compile
      COMMAND "${MERLIN_MATERIALX_TEST_SLANGC}"
        "${CMAKE_CURRENT_SOURCE_DIR}/data/materialx/standard-surface-wrapper.slang"
        -I "${CMAKE_CURRENT_BINARY_DIR}"
        -entry merlin_materialx_standard_surface_test_fragment
        -stage fragment
        -target spirv
        -profile sm_6_6
        -capability spirv_1_5
        -matrix-layout-column-major
        -O2
        -warnings-as-errors all
        -reflection-json
          "${CMAKE_CURRENT_BINARY_DIR}/materialx-standard-surface.spv.reflection.json"
        -o "${CMAKE_CURRENT_BINARY_DIR}/materialx-standard-surface.spv"
    )
    set_tests_properties(
      merlin-materialx-standard-surface-spirv-compile PROPERTIES
      LABELS "materials;materialx;shader;spirv;standard-surface"
      FIXTURES_REQUIRED merlin-materialx-generated-module
    )

    add_test(
      NAME merlin-materialx-standard-surface-metal-compile
      COMMAND "${MERLIN_MATERIALX_TEST_SLANGC}"
        "${CMAKE_CURRENT_SOURCE_DIR}/data/materialx/standard-surface-wrapper.slang"
        -I "${CMAKE_CURRENT_BINARY_DIR}"
        -entry merlin_materialx_standard_surface_test_fragment
        -stage fragment
        -target metal
        -profile metallib_2_4
        -matrix-layout-column-major
        -O2
        -warnings-as-errors all
        -reflection-json
          "${CMAKE_CURRENT_BINARY_DIR}/materialx-standard-surface.metal.reflection.json"
        -o "${CMAKE_CURRENT_BINARY_DIR}/materialx-standard-surface.metal"
    )
    set_tests_properties(
      merlin-materialx-standard-surface-metal-compile PROPERTIES
      LABELS "materials;materialx;metal;shader;standard-surface"
      FIXTURES_REQUIRED merlin-materialx-generated-module
    )
  endif()
endif()

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
  add_executable(merlin-shader-abi-tests
    shader_abi_test.cpp
  )
  target_link_libraries(merlin-shader-abi-tests PRIVATE Merlin::Vulkan)
  add_dependencies(merlin-shader-abi-tests merlin-vulkan-shaders)
  add_test(
    NAME merlin-shader-abi
    COMMAND merlin-shader-abi-tests
      "${MERLIN_VERTEX_SPV}.reflection.json"
      "${MERLIN_FRAGMENT_SPV}.reflection.json"
      "${MERLIN_BINDLESS_VERTEX_SPV}.reflection.json"
      "${MERLIN_BINDLESS_FRAGMENT_SPV}.reflection.json"
      "${MERLIN_VERTEX_METAL}.reflection.json"
      "${MERLIN_FRAGMENT_METAL}.reflection.json"
      "${MERLIN_SHADER_MANIFEST}"
  )
  set_tests_properties(merlin-shader-abi PROPERTIES LABELS "shader;abi")

  add_executable(merlin-environment-lighting-tests
    environment_lighting_test.cpp
  )
  target_include_directories(merlin-environment-lighting-tests PRIVATE
    "${PROJECT_SOURCE_DIR}/backend/merlin-vulkan/src"
  )
  target_link_libraries(merlin-environment-lighting-tests
    PRIVATE Merlin::Vulkan)
  add_test(
    NAME merlin-environment-lighting
    COMMAND merlin-environment-lighting-tests
            "${PROJECT_SOURCE_DIR}/backend/merlin-vulkan/assets/environment.hdr"
  )
  set_tests_properties(merlin-environment-lighting PROPERTIES
    LABELS "materials;ibl")

  add_executable(merlin-bindless-resource-table-tests
    bindless_resource_table_test.cpp
  )
  target_link_libraries(merlin-bindless-resource-table-tests
    PRIVATE Merlin::Vulkan)
  add_test(NAME merlin-bindless-resource-table
           COMMAND merlin-bindless-resource-table-tests)

  add_executable(merlin-descriptor-indexing-tests
    descriptor_indexing_test.cpp
  )
  target_link_libraries(merlin-descriptor-indexing-tests
    PRIVATE Merlin::Vulkan)
  add_test(NAME merlin-descriptor-indexing
           COMMAND merlin-descriptor-indexing-tests)

  add_executable(merlin-render-product-contract-tests
    render_product_contract_test.cpp
  )
  target_link_libraries(merlin-render-product-contract-tests
    PRIVATE Merlin::Vulkan)
  add_test(NAME merlin-render-product-contract
           COMMAND merlin-render-product-contract-tests)

  add_executable(merlin-render-artifact-tests
    render_artifact_test.cpp
  )
  target_link_libraries(merlin-render-artifact-tests
    PRIVATE Merlin::Vulkan)
  add_test(NAME merlin-render-artifacts
           COMMAND merlin-render-artifact-tests
                   "${CMAKE_CURRENT_BINARY_DIR}/render-artifacts")
endif()

if(TARGET merlin-headless)
  add_executable(merlin-execution-lifetime-tests
    execution_lifetime_test.cpp
  )
  target_link_libraries(merlin-execution-lifetime-tests
    PRIVATE Merlin::RenderWorld Merlin::RenderExtraction Merlin::Vulkan)
  add_test(
    NAME merlin-vulkan-execution-lifetime
    COMMAND merlin-execution-lifetime-tests
            "$<TARGET_FILE_DIR:merlin-headless>/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}"
  )
  set_tests_properties(merlin-vulkan-execution-lifetime PROPERTIES
    LABELS "gpu;lifetime"
    SKIP_RETURN_CODE 77
    TIMEOUT 60
  )

  add_executable(merlin-resource-update-tests
    resource_update_test.cpp
  )
  target_link_libraries(merlin-resource-update-tests
    PRIVATE Merlin::RenderExtraction Merlin::Vulkan)
  add_test(
    NAME merlin-vulkan-resource-update
    COMMAND merlin-resource-update-tests
            "$<TARGET_FILE_DIR:merlin-headless>/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}"
  )
  set_tests_properties(merlin-vulkan-resource-update PROPERTIES
    LABELS "gpu"
    SKIP_RETURN_CODE 77
    TIMEOUT 60
  )

  add_executable(merlin-material-resource-tests
    material_resource_test.cpp
  )
  target_link_libraries(merlin-material-resource-tests
    PRIVATE Merlin::RenderExtraction Merlin::Vulkan)
  add_test(
    NAME merlin-vulkan-material-resources
    COMMAND merlin-material-resource-tests
            "$<TARGET_FILE_DIR:merlin-headless>/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}"
  )
  set_tests_properties(merlin-vulkan-material-resources PROPERTIES
    LABELS "gpu;materials"
    SKIP_RETURN_CODE 77
    TIMEOUT 60
  )

  add_executable(merlin-bindless-residency-tests
    bindless_residency_test.cpp
  )
  target_link_libraries(merlin-bindless-residency-tests
    PRIVATE Merlin::RenderWorld Merlin::RenderExtraction Merlin::Vulkan)
  add_test(
    NAME merlin-vulkan-bindless-residency
    COMMAND merlin-bindless-residency-tests
            "$<TARGET_FILE_DIR:merlin-headless>/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}"
  )
  set_tests_properties(merlin-vulkan-bindless-residency PROPERTIES
    LABELS "gpu;bindless;residency"
    SKIP_RETURN_CODE 77
    TIMEOUT 60
  )
  add_test(
    NAME merlin-vulkan-offscreen
    COMMAND merlin-headless
            --width 64
            --height 64
            --frames 6
            --artifact-dir "${CMAKE_CURRENT_BINARY_DIR}/offscreen-artifacts"
            --output "${CMAKE_CURRENT_BINARY_DIR}/merlin-smoke.ppm"
  )
  add_test(
    NAME merlin-vulkan-validation
    COMMAND merlin-headless
            --width 64
            --height 64
            --frames 64
            --validate
            --artifact-dir "${CMAKE_CURRENT_BINARY_DIR}/validation-artifacts"
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
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME merlin-benchmark-compare
      COMMAND ${CMAKE_COMMAND}
        "-DMERLIN_PYTHON=${Python3_EXECUTABLE}"
        "-DMERLIN_COMPARE_SCRIPT=${PROJECT_SOURCE_DIR}/scripts/compare-benchmarks.py"
        "-DMERLIN_BENCHMARK_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/merlin-benchmark.json"
        "-DMERLIN_COMPARISON_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/merlin-benchmark-comparison.json"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_benchmark_compare_test.cmake"
    )
    set_tests_properties(merlin-benchmark-compare PROPERTIES
      DEPENDS merlin-benchmark-json
      LABELS "benchmark;gpu"
      TIMEOUT 30
    )
  endif()
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
