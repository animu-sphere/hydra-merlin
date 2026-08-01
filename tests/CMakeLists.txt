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

add_executable(merlin-gaussian-resource-tests
  gaussian_resource_test.cpp
)
target_link_libraries(merlin-gaussian-resource-tests
  PRIVATE Merlin::RenderExtraction)
add_test(NAME merlin-gaussian-resource COMMAND merlin-gaussian-resource-tests)
set_tests_properties(merlin-gaussian-resource PROPERTIES
  LABELS "gaussian;core")

add_executable(merlin-shader-contract-tests
  shader_contract_test.cpp
)
target_link_libraries(merlin-shader-contract-tests
  PRIVATE Merlin::RenderWorld)
add_test(NAME merlin-shader-contract COMMAND merlin-shader-contract-tests)

add_executable(merlin-material-diagnostic-tests
  material_diagnostic_test.cpp
)
target_link_libraries(merlin-material-diagnostic-tests
  PRIVATE Merlin::RenderWorld)
add_test(NAME merlin-material-diagnostic
  COMMAND merlin-material-diagnostic-tests)
set_tests_properties(merlin-material-diagnostic PROPERTIES
  LABELS "materials;diagnostics")

add_executable(merlin-material-abi-tests
  material_abi_test.cpp
)
target_link_libraries(merlin-material-abi-tests PRIVATE Merlin::RenderWorld)
add_test(NAME merlin-material-abi COMMAND merlin-material-abi-tests)
set_tests_properties(merlin-material-abi PROPERTIES LABELS "materials;abi")

add_executable(merlin-identity-tests
  identity_test.cpp
)
target_link_libraries(merlin-identity-tests PRIVATE Merlin::RenderWorld)
add_test(NAME merlin-identity COMMAND merlin-identity-tests)
set_tests_properties(merlin-identity PROPERTIES LABELS "cache")

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

if(TARGET Merlin::Metal)
  add_executable(merlin-metal-resource-table-tests
    metal_resource_table_test.cpp
  )
  target_link_libraries(merlin-metal-resource-table-tests
    PRIVATE Merlin::Metal)
  add_test(NAME merlin-metal-resource-table
    COMMAND merlin-metal-resource-table-tests)
  set_tests_properties(merlin-metal-resource-table PROPERTIES
    LABELS "metal;residency")

  add_executable(merlin-metal-backend-tests
    metal_backend_test.mm
  )
  set_target_properties(merlin-metal-backend-tests PROPERTIES
    OBJCXX_STANDARD 20
    OBJCXX_STANDARD_REQUIRED ON
    OBJCXX_EXTENSIONS OFF
  )
  target_compile_options(merlin-metal-backend-tests PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    $<$<COMPILE_LANGUAGE:OBJCXX>:-UNDEBUG>
  )
  target_link_libraries(merlin-metal-backend-tests
    PRIVATE Merlin::Metal)
  add_test(NAME merlin-metal-backend COMMAND merlin-metal-backend-tests)
  set_tests_properties(merlin-metal-backend PROPERTIES
    LABELS "metal;runtime;aov;residency;presentation"
    SKIP_RETURN_CODE 77)
endif()

add_test(
  NAME merlin-backend-boundaries
  COMMAND ${CMAKE_COMMAND}
    "-DMERLIN_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
    -P "${PROJECT_SOURCE_DIR}/cmake/verify-backend-boundaries.cmake"
)
set_tests_properties(merlin-backend-boundaries PROPERTIES LABELS "architecture")

if(TARGET Merlin::MaterialX)
  set(_merlin_materialx_artifact_source_dir
      "${PROJECT_SOURCE_DIR}/material/merlin-materialx/artifacts")
  add_executable(merlin-materialx-compiler-tests
    materialx_compiler_test.cpp
  )
  target_link_libraries(merlin-materialx-compiler-tests
    PRIVATE Merlin::MaterialX)
  # The pass-neutrality wiring is internal to the integration and unreachable
  # from any document, so the test reaches it through the private header rather
  # than through a widened public boundary.
  target_include_directories(merlin-materialx-compiler-tests
    PRIVATE "${PROJECT_SOURCE_DIR}/material/merlin-materialx/src")
  add_test(
    NAME merlin-materialx-compiler
    COMMAND merlin-materialx-compiler-tests
      "${_merlin_materialx_artifact_source_dir}/prototype.mtlx"
      "${_merlin_materialx_data_root}"
      "${CMAKE_CURRENT_BINARY_DIR}/materialx-prototype.slang"
      "${_merlin_materialx_artifact_source_dir}/standard-surface.mtlx"
      "${CMAKE_CURRENT_BINARY_DIR}/materialx-standard-surface.slang"
  )
  set_tests_properties(merlin-materialx-compiler PROPERTIES
    LABELS "materials;materialx;shader"
    FIXTURES_SETUP merlin-materialx-generated-module
  )

  set(MERLIN_MATERIALX_TEST_SLANGC
      "${MERLIN_MATERIALX_SLANGC_EXECUTABLE}")
  if(MERLIN_MATERIALX_TEST_SLANGC)
    add_test(
      NAME merlin-materialx-spirv-compile
      COMMAND "${MERLIN_MATERIALX_TEST_SLANGC}"
        "${_merlin_materialx_artifact_source_dir}/compile-wrapper.slang"
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
      FIXTURES_SETUP merlin-materialx-target-artifacts
    )

    if(TARGET Merlin::Vulkan)
      set(MERLIN_MATERIALX_FORWARD_SPV
          "${CMAKE_CURRENT_BINARY_DIR}/materialx-forward-prototype.spv")
      set(MERLIN_MATERIALX_STANDARD_SURFACE_FORWARD_SPV
          "${CMAKE_CURRENT_BINARY_DIR}/materialx-forward-standard-surface.spv")
      add_test(
        NAME merlin-materialx-vulkan-forward-compile
        COMMAND "${MERLIN_MATERIALX_TEST_SLANGC}"
          "${CMAKE_CURRENT_SOURCE_DIR}/data/materialx/vulkan-forward-prototype-wrapper.slang"
          -I "${CMAKE_CURRENT_BINARY_DIR}"
          -I "${PROJECT_SOURCE_DIR}/backend/merlin-vulkan/shaders"
          -entry merlin_materialx_forward_fragment
          -stage fragment
          -target spirv
          -profile sm_6_6
          -capability spirv_1_5
          -fvk-use-entrypoint-name
          -matrix-layout-column-major
          -O2
          -warnings-as-errors all
          -reflection-json
            "${MERLIN_MATERIALX_FORWARD_SPV}.reflection.json"
          -o "${MERLIN_MATERIALX_FORWARD_SPV}"
      )
      set_tests_properties(merlin-materialx-vulkan-forward-compile PROPERTIES
        LABELS "materials;materialx;shader;spirv;vulkan"
        FIXTURES_REQUIRED merlin-materialx-generated-module
        FIXTURES_SETUP merlin-materialx-vulkan-forward-artifact
      )
      add_test(
        NAME merlin-materialx-standard-surface-vulkan-forward-compile
        COMMAND "${MERLIN_MATERIALX_TEST_SLANGC}"
          "${CMAKE_CURRENT_SOURCE_DIR}/data/materialx/vulkan-forward-standard-surface-wrapper.slang"
          -I "${CMAKE_CURRENT_BINARY_DIR}"
          -I "${PROJECT_SOURCE_DIR}/backend/merlin-vulkan/shaders"
          -entry merlin_materialx_standard_surface_forward_fragment
          -stage fragment
          -target spirv
          -profile sm_6_6
          -capability spirv_1_5
          -fvk-use-entrypoint-name
          -matrix-layout-column-major
          -O2
          -warnings-as-errors all
          -reflection-json
            "${MERLIN_MATERIALX_STANDARD_SURFACE_FORWARD_SPV}.reflection.json"
          -o "${MERLIN_MATERIALX_STANDARD_SURFACE_FORWARD_SPV}"
      )
      set_tests_properties(
        merlin-materialx-standard-surface-vulkan-forward-compile PROPERTIES
        LABELS "materials;materialx;shader;spirv;vulkan;standard-surface"
        FIXTURES_REQUIRED merlin-materialx-generated-module
        FIXTURES_SETUP merlin-materialx-vulkan-forward-artifact
      )
    endif()

    add_test(
      NAME merlin-materialx-metal-compile
      COMMAND "${MERLIN_MATERIALX_TEST_SLANGC}"
        "${_merlin_materialx_artifact_source_dir}/compile-wrapper.slang"
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
      FIXTURES_SETUP merlin-materialx-target-artifacts
    )

    add_test(
      NAME merlin-materialx-standard-surface-spirv-compile
      COMMAND "${MERLIN_MATERIALX_TEST_SLANGC}"
        "${_merlin_materialx_artifact_source_dir}/standard-surface-wrapper.slang"
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
      FIXTURES_SETUP merlin-materialx-target-artifacts
    )

    add_test(
      NAME merlin-materialx-standard-surface-metal-compile
      COMMAND "${MERLIN_MATERIALX_TEST_SLANGC}"
        "${_merlin_materialx_artifact_source_dir}/standard-surface-wrapper.slang"
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
      FIXTURES_SETUP merlin-materialx-target-artifacts
    )

    # The reflection each of those four compiles emitted, checked back against
    # the module they were all built from.
    add_executable(merlin-material-target-reflection-tests
      material_target_reflection_test.cpp
    )
    target_link_libraries(merlin-material-target-reflection-tests
      PRIVATE Merlin::MaterialX Merlin::RenderWorld)
    add_test(
      NAME merlin-material-target-reflection
      COMMAND merlin-material-target-reflection-tests
        "${_merlin_materialx_artifact_source_dir}/prototype.mtlx"
        "${_merlin_materialx_artifact_source_dir}/standard-surface.mtlx"
        "${_merlin_materialx_data_root}"
        "${CMAKE_CURRENT_BINARY_DIR}/materialx-prototype.spv.reflection.json"
        "${CMAKE_CURRENT_BINARY_DIR}/materialx-prototype.metal.reflection.json"
        "${CMAKE_CURRENT_BINARY_DIR}/materialx-standard-surface.spv.reflection.json"
        "${CMAKE_CURRENT_BINARY_DIR}/materialx-standard-surface.metal.reflection.json"
    )
    set_tests_properties(merlin-material-target-reflection PROPERTIES
      LABELS "materials;materialx;abi;shader;spirv;metal"
      FIXTURES_REQUIRED merlin-materialx-target-artifacts
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
  add_executable(merlin-borrowed-vulkan-context-tests
    borrowed_vulkan_context_test.cpp
  )
  target_link_libraries(merlin-borrowed-vulkan-context-tests
    PRIVATE Merlin::Vulkan)
  add_test(NAME merlin-vulkan-borrowed-context
           COMMAND merlin-borrowed-vulkan-context-tests)
  set_tests_properties(merlin-vulkan-borrowed-context PROPERTIES
    LABELS "gpu;vulkan;hgi;presentation"
    SKIP_RETURN_CODE 77
    TIMEOUT 60)

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

  add_executable(merlin-shader-artifact-key-tests
    shader_artifact_key_test.cpp
  )
  target_link_libraries(merlin-shader-artifact-key-tests
    PRIVATE Merlin::RenderWorld)
  add_dependencies(merlin-shader-artifact-key-tests merlin-vulkan-shaders)
  add_test(
    NAME merlin-shader-artifact-key
    COMMAND merlin-shader-artifact-key-tests "${MERLIN_SHADER_MANIFEST}"
  )
  set_tests_properties(merlin-shader-artifact-key PROPERTIES
    LABELS "shader;cache")

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
  if(TARGET Merlin::MaterialX AND DEFINED MERLIN_MATERIALX_FORWARD_SPV)
    add_executable(merlin-generated-material-vulkan-tests
      generated_material_vulkan_test.cpp
    )
    target_link_libraries(merlin-generated-material-vulkan-tests
      PRIVATE Merlin::RenderWorld Merlin::RenderExtraction
              Merlin::MaterialX Merlin::Vulkan)
    add_test(
      NAME merlin-vulkan-generated-material
      COMMAND merlin-generated-material-vulkan-tests
        "$<TARGET_FILE_DIR:merlin-headless>/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}"
        "${_merlin_materialx_artifact_source_dir}/prototype.mtlx"
        "${_merlin_materialx_data_root}"
        "${MERLIN_MATERIALX_FORWARD_SPV}"
        "${_merlin_materialx_artifact_source_dir}/standard-surface.mtlx"
        "${MERLIN_MATERIALX_STANDARD_SURFACE_FORWARD_SPV}"
        "${PROJECT_SOURCE_DIR}/backend/merlin-vulkan/assets/environment.hdr"
    )
    set_tests_properties(merlin-vulkan-generated-material PROPERTIES
      LABELS "gpu;materials;materialx;vulkan"
      FIXTURES_REQUIRED merlin-materialx-vulkan-forward-artifact
      SKIP_RETURN_CODE 77
      TIMEOUT 60
    )
  endif()

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
    "-DMERLIN_SHADER_ARTIFACT_SCHEMA_VERSION=${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}"
    "-DMERLIN_EXPECTED_MATERIALX_ARTIFACTS=${MERLIN_MATERIALX_ARTIFACTS_ENABLED}"
    "-DMERLIN_EXPECTED_METAL=${MERLIN_ENABLE_METAL}"
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
