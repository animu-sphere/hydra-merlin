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

FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG ${MERLIN_IMGUI_PINNED_COMMIT}
  GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(imgui)

if(MERLIN_ENABLE_HYDRA2)
  if(APPLE)
    enable_language(OBJC)
  endif()
  set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(NFD_BUILD_SDL2_TESTS OFF CACHE BOOL "" FORCE)
  set(NFD_BUILD_GLFW3_TESTS OFF CACHE BOOL "" FORCE)
  set(NFD_INSTALL OFF CACHE BOOL "" FORCE)
  if(UNIX AND NOT APPLE)
    # The portal delegates the chooser to the active desktop environment,
    # which keeps the viewport usable under both X11 and Wayland without
    # linking either display-server client into the dialog implementation.
    set(NFD_PORTAL ON CACHE BOOL "" FORCE)
    set(NFD_X11 OFF CACHE BOOL "" FORCE)
    set(NFD_WAYLAND OFF CACHE BOOL "" FORCE)
  endif()
  FetchContent_Declare(nfd
    GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
    GIT_TAG ${MERLIN_NFD_PINNED_COMMIT}
    GIT_SHALLOW FALSE
  )
  FetchContent_MakeAvailable(nfd)
endif()

add_library(merlin-viewport-imgui STATIC
  "${imgui_SOURCE_DIR}/imgui.cpp"
  "${imgui_SOURCE_DIR}/imgui_draw.cpp"
  "${imgui_SOURCE_DIR}/imgui_tables.cpp"
  "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
)
if(TARGET Merlin::Vulkan)
  target_sources(merlin-viewport-imgui PRIVATE
    "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp"
  )
  target_link_libraries(merlin-viewport-imgui PUBLIC Merlin::Vulkan)
endif()
if(TARGET Merlin::Metal)
  target_sources(merlin-viewport-imgui PRIVATE
    "${imgui_SOURCE_DIR}/backends/imgui_impl_metal.mm"
  )
  set_target_properties(merlin-viewport-imgui PROPERTIES
    OBJCXX_STANDARD 20
    OBJCXX_STANDARD_REQUIRED ON
    OBJCXX_EXTENSIONS OFF
  )
  target_compile_options(merlin-viewport-imgui PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
  )
endif()
target_include_directories(merlin-viewport-imgui
  SYSTEM PUBLIC
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
)
target_link_libraries(merlin-viewport-imgui
  PUBLIC glfw
)
target_compile_features(merlin-viewport-imgui PUBLIC cxx_std_20)
if(MSVC)
  target_compile_options(merlin-viewport-imgui PRIVATE /W0)
else()
  target_compile_options(merlin-viewport-imgui PRIVATE -w)
endif()

add_executable(merlin-viewport
  src/developer_ui_aov.cpp
  src/developer_ui.cpp
  src/developer_ui_settings.cpp
  src/main.cpp
  src/window_glfw.cpp
)
if(TARGET Merlin::Vulkan)
  target_sources(merlin-viewport PRIVATE src/presentation_glfw.cpp)
  target_compile_definitions(merlin-viewport PRIVATE
    MERLIN_VIEWPORT_ENABLE_VULKAN=1
  )
  target_link_libraries(merlin-viewport PRIVATE Merlin::Vulkan)
endif()
if(TARGET Merlin::Metal)
  find_library(MERLIN_VIEWPORT_APPKIT_FRAMEWORK AppKit REQUIRED)
  find_library(MERLIN_VIEWPORT_QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
  target_sources(merlin-viewport PRIVATE
    src/presentation_metal.mm
  )
  set_source_files_properties(src/developer_ui.cpp PROPERTIES
    LANGUAGE OBJCXX
  )
  set_target_properties(merlin-viewport PROPERTIES
    OBJCXX_STANDARD 20
    OBJCXX_STANDARD_REQUIRED ON
    OBJCXX_EXTENSIONS OFF
  )
  target_compile_definitions(merlin-viewport PRIVATE
    MERLIN_VIEWPORT_ENABLE_METAL=1
  )
  target_compile_options(merlin-viewport PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
  )
  target_link_libraries(merlin-viewport PRIVATE
    Merlin::Metal
    "${MERLIN_VIEWPORT_APPKIT_FRAMEWORK}"
    "${MERLIN_VIEWPORT_QUARTZCORE_FRAMEWORK}"
  )
endif()

if(MERLIN_ENABLE_HYDRA2)
  target_sources(merlin-viewport PRIVATE src/hydra_scene.cpp)
  target_link_libraries(merlin-viewport PRIVATE
    merlin-hydra2-runtime
    merlin-hydra2-host-dependencies
    nfd
  )
  target_compile_definitions(merlin-viewport PRIVATE
    MERLIN_VIEWPORT_ENABLE_HYDRA2=1
    MERLIN_VIEWPORT_ENABLE_NATIVE_FILE_DIALOG=1
    NOMINMAX
    _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING
  )
endif()

target_include_directories(merlin-viewport PRIVATE src)
target_link_libraries(merlin-viewport
  PRIVATE
    Merlin::RenderWorld
    Merlin::RenderBackend
    glfw
    merlin-viewport-imgui
)
target_compile_features(merlin-viewport PRIVATE cxx_std_20)

if(WIN32 AND MERLIN_ENABLE_HYDRA2)
  # Developer builds use an external OpenUSD SDK. Generate a launcher beside
  # each configuration's executable so it can resolve the SDK DLLs outside
  # CTest without requiring a persistent machine-wide PATH change.
  file(GENERATE
    OUTPUT "$<TARGET_FILE_DIR:merlin-viewport>/run-merlin-viewport.cmd"
    CONTENT
      "@echo off\r\nsetlocal\r\nset \"PATH=${MERLIN_OPENUSD_RUNTIME_ROOT}/bin;${MERLIN_OPENUSD_RUNTIME_ROOT}/lib;%PATH%\"\r\n\"$<TARGET_FILE:merlin-viewport>\" %*\r\nexit /b %ERRORLEVEL%\r\n"
    TARGET merlin-viewport
  )
endif()

if(TARGET Merlin::Vulkan)
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
endif()

install(TARGETS merlin-viewport
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(FILES "${imgui_SOURCE_DIR}/LICENSE.txt"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/merlin/licenses/imgui"
)
if(MERLIN_ENABLE_HYDRA2)
  install(FILES "${nfd_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/merlin/licenses/nativefiledialog-extended"
  )
endif()
if(TARGET Merlin::Vulkan)
  install(FILES ${MERLIN_SHADER_PACKAGE_FILES}
    DESTINATION
      "${CMAKE_INSTALL_BINDIR}/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}")
endif()

if(MERLIN_BUILD_TESTS)
  add_executable(merlin-viewport-settings-tests
    src/developer_ui_aov.cpp
    src/developer_ui_settings.cpp
    tests/developer_ui_settings_test.cpp
  )
  target_include_directories(merlin-viewport-settings-tests PRIVATE src)
  target_link_libraries(merlin-viewport-settings-tests PRIVATE
    Merlin::RenderBackend
  )
  target_compile_features(merlin-viewport-settings-tests PRIVATE cxx_std_20)
  add_test(
    NAME merlin-viewport-settings
    COMMAND merlin-viewport-settings-tests
  )
  set_tests_properties(merlin-viewport-settings PROPERTIES
    LABELS "viewport;unit"
  )

  if(TARGET Merlin::Vulkan)
    add_test(
      NAME merlin-viewport-vulkan
      COMMAND merlin-viewport
              --backend vulkan
              --hidden
              --frames 8
              --vsync off
              --validate
              --reference-check
              --resize-test
              --allow-unavailable
              --benchmark "${CMAKE_CURRENT_BINARY_DIR}/viewport-benchmark.json"
    )
    set_tests_properties(merlin-viewport-vulkan PROPERTIES
      LABELS "gpu;viewport;presentation"
      SKIP_RETURN_CODE 77
      TIMEOUT 60
    )
  endif()
  if(TARGET Merlin::Metal)
    add_test(
      NAME merlin-viewport-metal
      COMMAND merlin-viewport
              --backend metal
              --hidden
              --frames 8
              --vsync off
              --validate
              --reference-check
              --resize-test
              --allow-unavailable
              --benchmark
              "${CMAKE_CURRENT_BINARY_DIR}/viewport-metal-benchmark.json"
    )
    set_tests_properties(merlin-viewport-metal PROPERTIES
      LABELS "gpu;viewport;presentation;metal"
      SKIP_RETURN_CODE 77
      TIMEOUT 60
    )
  endif()
  if(MERLIN_ENABLE_HYDRA2)
    if(TARGET Merlin::Vulkan)
      set_property(TEST merlin-viewport-vulkan APPEND PROPERTY
        ENVIRONMENT_MODIFICATION
          "PATH=path_list_prepend:${MERLIN_OPENUSD_RUNTIME_ROOT}/bin"
          "PATH=path_list_prepend:${MERLIN_OPENUSD_RUNTIME_ROOT}/lib"
      )
    endif()
    add_test(
      NAME merlin-viewport-hydra
      COMMAND merlin-viewport
              --hidden
              --frames 6
              --vsync off
              --validate
              --reference-check
              --resize-test
              --allow-unavailable
              --usd "${PROJECT_SOURCE_DIR}/adapters/merlin-hydra2/tests/usdview-smoke.usda"
              --benchmark "${CMAKE_CURRENT_BINARY_DIR}/viewport-hydra-benchmark.json"
    )
    set_tests_properties(merlin-viewport-hydra PROPERTIES
      LABELS "gpu;viewport;presentation;hydra;host-smoke"
      SKIP_RETURN_CODE 77
      TIMEOUT 60
      ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${MERLIN_OPENUSD_RUNTIME_ROOT}/bin;PATH=path_list_prepend:${MERLIN_OPENUSD_RUNTIME_ROOT}/lib"
    )
    add_test(
      NAME merlin-viewport-hydra-no-readback
      COMMAND merlin-viewport
              --hidden
              --frames 4
              --vsync off
              --validate
              --allow-unavailable
              --usd "${PROJECT_SOURCE_DIR}/adapters/merlin-hydra2/tests/usdview-smoke.usda"
    )
    set_tests_properties(merlin-viewport-hydra-no-readback PROPERTIES
      LABELS "gpu;viewport;presentation;hydra;host-smoke"
      SKIP_RETURN_CODE 77
      TIMEOUT 60
      ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${MERLIN_OPENUSD_RUNTIME_ROOT}/bin;PATH=path_list_prepend:${MERLIN_OPENUSD_RUNTIME_ROOT}/lib"
    )
  endif()
endif()
