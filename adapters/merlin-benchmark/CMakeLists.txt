find_package(Git QUIET)
set(_merlin_benchmark_commit "unknown")
if(Git_FOUND)
  foreach(_merlin_git_path HEAD logs/HEAD)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" rev-parse --git-path "${_merlin_git_path}"
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
      RESULT_VARIABLE _merlin_git_path_result
      OUTPUT_VARIABLE _merlin_git_path_output
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
    if(_merlin_git_path_result EQUAL 0)
      cmake_path(ABSOLUTE_PATH _merlin_git_path_output
                 BASE_DIRECTORY "${PROJECT_SOURCE_DIR}"
                 NORMALIZE OUTPUT_VARIABLE _merlin_git_dependency)
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                   "${_merlin_git_dependency}")
    endif()
  endforeach()

  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    RESULT_VARIABLE _merlin_git_result
    OUTPUT_VARIABLE _merlin_git_output
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(_merlin_git_result EQUAL 0)
    set(_merlin_benchmark_commit "${_merlin_git_output}")
  endif()
endif()

add_executable(merlin-benchmark
  src/main.cpp
)
merlin_target_defaults(merlin-benchmark)

target_link_libraries(merlin-benchmark
  PRIVATE
    Merlin::RenderWorld
    Merlin::Vulkan
)

target_compile_definitions(merlin-benchmark PRIVATE
  MERLIN_BENCHMARK_COMMIT="${_merlin_benchmark_commit}"
  MERLIN_BENCHMARK_BUILD_TYPE="$<CONFIG>"
  MERLIN_BENCHMARK_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
  MERLIN_BENCHMARK_OS="${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_VERSION}"
  MERLIN_BENCHMARK_ARCHITECTURE="${CMAKE_SYSTEM_PROCESSOR}"
)

add_dependencies(merlin-benchmark merlin-vulkan-shaders)

add_custom_target(merlin-benchmark-shader-package
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "$<TARGET_FILE_DIR:merlin-benchmark>/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${MERLIN_SHADER_PACKAGE_FILES}
          "$<TARGET_FILE_DIR:merlin-benchmark>/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}"
  DEPENDS ${MERLIN_SHADER_PACKAGE_FILES}
  VERBATIM
)
add_dependencies(merlin-benchmark-shader-package merlin-vulkan-shaders)
add_dependencies(merlin-benchmark merlin-benchmark-shader-package)

# Benchmark-only generated-material ABI artifact; not part of renderer packages.
set(_merlin_benchmark_generated_spv "${CMAKE_CURRENT_BINARY_DIR}/benchmark-generated.frag.spv")
add_custom_command(OUTPUT "${_merlin_benchmark_generated_spv}"
  COMMAND "${MERLIN_SLANGC_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/generated-scale-material.slang"
    -I "${PROJECT_SOURCE_DIR}/backend/merlin-vulkan/shaders"
    -entry benchmark_generated_fragment -stage fragment -target spirv
    -profile sm_6_6 -matrix-layout-column-major -fvk-use-entrypoint-name -O2
    -o "${_merlin_benchmark_generated_spv}"
  DEPENDS src/generated-scale-material.slang
          "${PROJECT_SOURCE_DIR}/backend/merlin-vulkan/shaders/forward-common.slang"
  VERBATIM)
add_custom_target(merlin-benchmark-generated-shader
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_merlin_benchmark_generated_spv}"
    "$<TARGET_FILE_DIR:merlin-benchmark>/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}/benchmark-generated.frag.spv"
  DEPENDS "${_merlin_benchmark_generated_spv}" merlin-benchmark-shader-package
  VERBATIM)
add_dependencies(merlin-benchmark merlin-benchmark-generated-shader)
install(FILES "${_merlin_benchmark_generated_spv}"
  DESTINATION "${CMAKE_INSTALL_BINDIR}/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}")

install(TARGETS merlin-benchmark
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
