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

add_custom_command(TARGET merlin-benchmark POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "$<TARGET_FILE_DIR:merlin-benchmark>/shaders"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${MERLIN_VERTEX_SPV}"
          "$<TARGET_FILE_DIR:merlin-benchmark>/shaders/triangle.vert.spv"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${MERLIN_FRAGMENT_SPV}"
          "$<TARGET_FILE_DIR:merlin-benchmark>/shaders/triangle.frag.spv"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${MERLIN_BINDLESS_VERTEX_SPV}"
          "$<TARGET_FILE_DIR:merlin-benchmark>/shaders/triangle.bindless.vert.spv"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${MERLIN_BINDLESS_FRAGMENT_SPV}"
          "$<TARGET_FILE_DIR:merlin-benchmark>/shaders/triangle.bindless.frag.spv"
  VERBATIM
)

install(TARGETS merlin-benchmark
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
