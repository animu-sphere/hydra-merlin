find_package(pxr CONFIG REQUIRED)
include(GNUInstallDirs)

if(DEFINED pxr_VERSION)
  set(MERLIN_OPENUSD_DETECTED_VERSION "${pxr_VERSION}" PARENT_SCOPE)
elseif(DEFINED PXR_VERSION)
  set(MERLIN_OPENUSD_DETECTED_VERSION "${PXR_VERSION}" PARENT_SCOPE)
endif()

if(NOT TARGET Merlin::Vulkan)
  message(FATAL_ERROR "MERLIN_ENABLE_HYDRA2 requires MERLIN_ENABLE_VULKAN=ON")
endif()

add_library(merlin-hydra2-runtime STATIC
  src/adapter.cpp
)

target_include_directories(merlin-hydra2-runtime PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(merlin-hydra2-runtime
  PUBLIC hd
  PRIVATE
    hio
    Merlin::RenderWorld
    Merlin::RenderExtraction
    Merlin::Vulkan
    ${CMAKE_DL_LIBS}
)
target_compile_features(merlin-hydra2-runtime PUBLIC cxx_std_20)
target_compile_definitions(merlin-hydra2-runtime PRIVATE
  NOMINMAX
  _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING
)

add_library(hdMerlin MODULE
  src/renderer_plugin.cpp
  resources/plugInfo.json
)

target_link_libraries(hdMerlin
  PRIVATE
    merlin-hydra2-runtime
    hd
)

target_compile_features(hdMerlin PRIVATE cxx_std_20)
target_compile_definitions(hdMerlin PRIVATE
  NOMINMAX
  _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING
)
set_target_properties(hdMerlin PROPERTIES PREFIX "")

add_custom_command(TARGET hdMerlin POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "$<TARGET_FILE_DIR:hdMerlin>/resources"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/resources/plugInfo.json"
          "$<TARGET_FILE_DIR:hdMerlin>/resources/plugInfo.json"
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "$<TARGET_FILE_DIR:hdMerlin>/shaders"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${MERLIN_VERTEX_SPV}"
          "$<TARGET_FILE_DIR:hdMerlin>/shaders/triangle.vert.spv"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${MERLIN_FRAGMENT_SPV}"
          "$<TARGET_FILE_DIR:hdMerlin>/shaders/triangle.frag.spv"
  VERBATIM
)
add_dependencies(hdMerlin merlin-vulkan-shaders)

set(_merlin_hydra_install_dir
    "${CMAKE_INSTALL_LIBDIR}/usd/hdMerlin")
install(TARGETS hdMerlin
  RUNTIME DESTINATION "${_merlin_hydra_install_dir}"
  LIBRARY DESTINATION "${_merlin_hydra_install_dir}"
)
install(FILES resources/plugInfo.json
  DESTINATION "${_merlin_hydra_install_dir}/resources")
install(FILES "${MERLIN_VERTEX_SPV}" "${MERLIN_FRAGMENT_SPV}"
  DESTINATION "${_merlin_hydra_install_dir}/shaders")
install(FILES tests/usdview-smoke.usda tests/checker.ppm
  DESTINATION "${CMAKE_INSTALL_DATADIR}/merlin/tests")

if(MERLIN_BUILD_TESTS)
  add_executable(merlin-hydra2-discovery-test
    tests/plugin_discovery_test.cpp
  )
  target_link_libraries(merlin-hydra2-discovery-test PRIVATE hd plug)
  target_compile_features(merlin-hydra2-discovery-test PRIVATE cxx_std_20)
  target_compile_definitions(merlin-hydra2-discovery-test PRIVATE
    NOMINMAX
    _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING
  )
  add_dependencies(merlin-hydra2-discovery-test hdMerlin)
  add_test(
    NAME merlin-hydra2-discovery
    COMMAND merlin-hydra2-discovery-test
            "$<TARGET_FILE_DIR:hdMerlin>/resources"
  )
  get_filename_component(_merlin_pxr_root "${PXR_INCLUDE_DIRS}/.." ABSOLUTE)
  set_tests_properties(merlin-hydra2-discovery PROPERTIES
    ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:${_merlin_pxr_root}/bin;PATH=path_list_prepend:${_merlin_pxr_root}/lib"
  )

  add_executable(merlin-hydra2-render-buffer-test
    tests/render_buffer_test.cpp
  )
  target_link_libraries(merlin-hydra2-render-buffer-test
    PRIVATE merlin-hydra2-runtime)
  target_compile_features(merlin-hydra2-render-buffer-test PRIVATE cxx_std_20)
  target_compile_definitions(merlin-hydra2-render-buffer-test PRIVATE
    NOMINMAX
    _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING
  )
  add_test(NAME merlin-hydra2-render-buffer
           COMMAND merlin-hydra2-render-buffer-test)
  set_tests_properties(merlin-hydra2-render-buffer PROPERTIES
    ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:${_merlin_pxr_root}/bin;PATH=path_list_prepend:${_merlin_pxr_root}/lib"
  )

  find_package(Python3 COMPONENTS Interpreter QUIET)
  find_program(MERLIN_TESTUSDVIEW_SCRIPT testusdview
    PATHS "${_merlin_pxr_root}/bin" NO_DEFAULT_PATH)
  if(Python3_Interpreter_FOUND AND MERLIN_TESTUSDVIEW_SCRIPT)
    add_test(
      NAME merlin-hydra2-usdview-smoke
      COMMAND ${CMAKE_COMMAND}
        "-DMERLIN_CMAKE_COMMAND=${CMAKE_COMMAND}"
        "-DMERLIN_BUILD_DIR=${CMAKE_BINARY_DIR}"
        "-DMERLIN_CONFIG=$<CONFIG>"
        "-DMERLIN_STAGE_DIR=${CMAKE_CURRENT_BINARY_DIR}/usdview-install"
        "-DMERLIN_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
        "-DMERLIN_INSTALL_DATADIR=${CMAKE_INSTALL_DATADIR}"
        "-DMERLIN_PXR_ROOT=${_merlin_pxr_root}"
        "-DMERLIN_PYTHON=${Python3_EXECUTABLE}"
        "-DMERLIN_TESTUSDVIEW=${MERLIN_TESTUSDVIEW_SCRIPT}"
        "-DMERLIN_USDVIEW_TEST_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/usdview_smoke_test.py"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_usdview_smoke.cmake"
    )
    set_tests_properties(merlin-hydra2-usdview-smoke PROPERTIES
      LABELS "host-smoke;gpu"
      TIMEOUT 60
      RUN_SERIAL TRUE
    )
  endif()
endif()
