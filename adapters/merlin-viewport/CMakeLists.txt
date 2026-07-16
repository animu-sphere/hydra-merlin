enable_language(C)

find_package(glfw3 ${MERLIN_GLFW_MIN_VERSION} CONFIG QUIET)
if(NOT TARGET glfw)
  include(FetchContent)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG ${MERLIN_GLFW_PINNED_COMMIT}
    GIT_SHALLOW FALSE
  )
  FetchContent_MakeAvailable(glfw)
endif()

add_executable(merlin-viewport
  src/main.cpp
  src/presentation_glfw.cpp
  src/window_glfw.cpp
)

if(MERLIN_ENABLE_HYDRA2)
  target_sources(merlin-viewport PRIVATE src/hydra_scene.cpp)
  target_link_libraries(merlin-viewport PRIVATE
    merlin-hydra2-runtime
    merlin-hydra2-host-dependencies
  )
  target_compile_definitions(merlin-viewport PRIVATE
    MERLIN_VIEWPORT_ENABLE_HYDRA2=1
    NOMINMAX
    _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING
  )
endif()

target_include_directories(merlin-viewport PRIVATE src)
target_link_libraries(merlin-viewport
  PRIVATE
    Merlin::RenderWorld
    Merlin::RenderBackend
    Merlin::Vulkan
    glfw
)
target_compile_features(merlin-viewport PRIVATE cxx_std_20)

add_dependencies(merlin-viewport merlin-vulkan-shaders)
add_custom_target(merlin-viewport-shader-package
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "$<TARGET_FILE_DIR:merlin-viewport>/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${MERLIN_SHADER_PACKAGE_FILES}
          "$<TARGET_FILE_DIR:merlin-viewport>/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}"
  DEPENDS ${MERLIN_SHADER_PACKAGE_FILES}
  VERBATIM
)
add_dependencies(merlin-viewport-shader-package merlin-vulkan-shaders)
add_dependencies(merlin-viewport merlin-viewport-shader-package)

install(TARGETS merlin-viewport
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(FILES ${MERLIN_SHADER_PACKAGE_FILES}
  DESTINATION
    "${CMAKE_INSTALL_BINDIR}/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}")

if(MERLIN_BUILD_TESTS)
  add_test(
    NAME merlin-viewport-vulkan
    COMMAND merlin-viewport
            --hidden
            --frames 8
            --vsync off
            --validate
            --reference-check
            --resize-test
            --benchmark "${CMAKE_CURRENT_BINARY_DIR}/viewport-benchmark.json"
  )
  set_tests_properties(merlin-viewport-vulkan PROPERTIES
    LABELS "gpu;viewport;presentation"
    TIMEOUT 60
  )
  if(MERLIN_ENABLE_HYDRA2)
    set_property(TEST merlin-viewport-vulkan APPEND PROPERTY
      ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${MERLIN_OPENUSD_RUNTIME_ROOT}/bin"
        "PATH=path_list_prepend:${MERLIN_OPENUSD_RUNTIME_ROOT}/lib"
    )
    add_test(
      NAME merlin-viewport-hydra
      COMMAND merlin-viewport
              --hidden
              --frames 6
              --vsync off
              --validate
              --resize-test
              --usd "${PROJECT_SOURCE_DIR}/adapters/merlin-hydra2/tests/usdview-smoke.usda"
              --benchmark "${CMAKE_CURRENT_BINARY_DIR}/viewport-hydra-benchmark.json"
    )
    set_tests_properties(merlin-viewport-hydra PROPERTIES
      LABELS "gpu;viewport;presentation;hydra;host-smoke"
      TIMEOUT 60
      ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${MERLIN_OPENUSD_RUNTIME_ROOT}/bin;PATH=path_list_prepend:${MERLIN_OPENUSD_RUNTIME_ROOT}/lib"
    )
  endif()
endif()
