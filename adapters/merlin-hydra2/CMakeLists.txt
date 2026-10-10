merlin_find_hydra_dependencies()

if(NOT TARGET Merlin::Vulkan AND NOT TARGET Merlin::Metal)
  message(FATAL_ERROR
    "MERLIN_ENABLE_HYDRA2 requires MERLIN_ENABLE_VULKAN=ON or "
    "MERLIN_ENABLE_METAL=ON")
endif()

add_library(merlin-hydra2-runtime STATIC
  src/adapter.cpp
  src/distant_light.cpp
  src/hgi_vulkan_bridge.cpp
)
merlin_target_defaults(merlin-hydra2-runtime)
if(MERLIN_ENABLE_HGI_METAL_BRIDGE)
  target_sources(merlin-hydra2-runtime PRIVATE src/hgi_metal_bridge.mm)
  set_source_files_properties(src/hgi_metal_bridge.mm PROPERTIES
    COMPILE_OPTIONS "-fobjc-arc")
  set_target_properties(merlin-hydra2-runtime PROPERTIES
    OBJCXX_STANDARD 20
    OBJCXX_STANDARD_REQUIRED ON
    OBJCXX_EXTENSIONS OFF)
endif()

# Keeps OpenUSD imported-target lookup in the directory where find_package(pxr)
# created those non-GLOBAL targets, while allowing the sibling viewport product
# to consume the host-side UsdImaging APIs.
add_library(merlin-hydra2-host-dependencies INTERFACE)
target_link_libraries(merlin-hydra2-host-dependencies INTERFACE
  usd
  usdGeom
  usdImaging
)

target_include_directories(merlin-hydra2-runtime PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(merlin-hydra2-runtime
  PUBLIC
    hd
    # HgiTextureHandle reaches the public boundary through adapter.hpp.
    hgi
    Merlin::RenderBackend
  PRIVATE
    hio
    Merlin::RenderWorld
    Merlin::RenderExtraction
    ${CMAKE_DL_LIBS}
)
if(TARGET Merlin::Vulkan)
  target_link_libraries(merlin-hydra2-runtime PRIVATE Merlin::Vulkan)
  target_compile_definitions(merlin-hydra2-runtime PRIVATE
    MERLIN_HYDRA2_ENABLE_VULKAN=1
  )
elseif(TARGET Merlin::Metal)
  target_link_libraries(merlin-hydra2-runtime PRIVATE Merlin::Metal)
  target_compile_definitions(merlin-hydra2-runtime PRIVATE
    MERLIN_HYDRA2_ENABLE_METAL=1
  )
endif()
if(MERLIN_ENABLE_HGI_VULKAN_BRIDGE)
  target_compile_definitions(merlin-hydra2-runtime PRIVATE
    MERLIN_HYDRA2_ENABLE_HGI_VULKAN_BRIDGE=1
  )
  if(TARGET hgiVulkan)
    target_link_libraries(merlin-hydra2-runtime PRIVATE hgiVulkan)
    target_compile_definitions(merlin-hydra2-runtime PRIVATE
      MERLIN_HYDRA2_HAVE_HGI_VULKAN_NATIVE=1
    )
  endif()
endif()
if(MERLIN_ENABLE_HGI_METAL_BRIDGE)
  if(TARGET Merlin::Metal)
    # The Hydra renderer may select Vulkan when both native backends are
    # enabled, but the Metal bridge still needs the Metal AOV-export ABI.
    target_link_libraries(merlin-hydra2-runtime PRIVATE Merlin::Metal)
  endif()
  target_compile_definitions(merlin-hydra2-runtime PRIVATE
    MERLIN_HYDRA2_ENABLE_HGI_METAL_BRIDGE=1
  )
  if(TARGET hgiMetal)
    target_link_libraries(merlin-hydra2-runtime PRIVATE hgiMetal)
    target_compile_definitions(merlin-hydra2-runtime PRIVATE
      MERLIN_HYDRA2_HAVE_HGI_METAL_NATIVE=1
    )
  endif()
endif()
target_compile_features(merlin-hydra2-runtime PUBLIC cxx_std_20)
target_compile_definitions(merlin-hydra2-runtime PRIVATE
  NOMINMAX
  MERLIN_OPENUSD_VALIDATED_PXR_VERSION=${_merlin_pxr_version_number}
  _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING
)
if(_merlin_openusd_release_only)
  target_compile_definitions(merlin-hydra2-runtime PRIVATE
    "$<$<CONFIG:Debug>:MERLIN_OPENUSD_RELEASE_ONLY=1>")
endif()

set(_merlin_hydra_plugin_info
  "${CMAKE_CURRENT_BINARY_DIR}/generated/plugInfo.json")
configure_file(
  resources/plugInfo.json.in
  "${_merlin_hydra_plugin_info}"
  @ONLY
)

add_library(hdMerlin MODULE
  src/renderer_plugin.cpp
  "${_merlin_hydra_plugin_info}"
)
merlin_target_defaults(hdMerlin)

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
          "${_merlin_hydra_plugin_info}"
          "$<TARGET_FILE_DIR:hdMerlin>/resources/plugInfo.json"
  VERBATIM
)
if(TARGET Merlin::Vulkan)
  add_custom_target(merlin-hydra2-shader-package
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:hdMerlin>/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${MERLIN_SHADER_PACKAGE_FILES}
            "$<TARGET_FILE_DIR:hdMerlin>/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}"
    DEPENDS ${MERLIN_SHADER_PACKAGE_FILES}
    VERBATIM
  )
  add_dependencies(merlin-hydra2-shader-package merlin-vulkan-shaders)
  add_dependencies(hdMerlin merlin-hydra2-shader-package)
endif()

set(_merlin_hydra_install_dir
    "${CMAKE_INSTALL_LIBDIR}/usd/hdMerlin")
install(TARGETS hdMerlin
  RUNTIME DESTINATION "${_merlin_hydra_install_dir}"
  LIBRARY DESTINATION "${_merlin_hydra_install_dir}"
)
install(FILES "${_merlin_hydra_plugin_info}"
  DESTINATION "${_merlin_hydra_install_dir}/resources")
if(TARGET Merlin::Vulkan)
  install(FILES ${MERLIN_SHADER_PACKAGE_FILES}
    DESTINATION
      "${_merlin_hydra_install_dir}/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}")
endif()
install(FILES tests/usdview-smoke.usda tests/checker.ppm
  DESTINATION "${CMAKE_INSTALL_DATADIR}/merlin/tests")

if(MERLIN_BUILD_TESTS)
  get_filename_component(_merlin_pxr_root
    "${_merlin_pxr_include_dir_selected}/.." ABSOLUTE)
  if(MERLIN_GAUSSIAN_SAMPLE)
    if(NOT EXISTS "${MERLIN_GAUSSIAN_SAMPLE}")
      message(FATAL_ERROR
        "MERLIN_GAUSSIAN_SAMPLE does not exist: ${MERLIN_GAUSSIAN_SAMPLE}")
    endif()
    set(_merlin_gaussian_reference_image
        "${MERLIN_GAUSSIAN_REFERENCE_IMAGE}")
    if(NOT _merlin_gaussian_reference_image AND
       "${MERLIN_GAUSSIAN_SAMPLE}" STREQUAL
       "${MERLIN_GAUSSIAN_BUNDLED_SAMPLE}")
      if(TARGET Merlin::Vulkan)
        set(_merlin_gaussian_reference_image
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/leica-sofort-top8192-reference-vulkan-float.png")
      else()
        set(_merlin_gaussian_reference_image
            "${MERLIN_GAUSSIAN_BUNDLED_REFERENCE_IMAGE}")
      endif()
    endif()
    if(_merlin_gaussian_reference_image AND
       NOT EXISTS "${_merlin_gaussian_reference_image}")
      message(FATAL_ERROR
        "MERLIN_GAUSSIAN_REFERENCE_IMAGE does not exist: "
        "${_merlin_gaussian_reference_image}")
    endif()
    add_executable(merlin-hydra2-gaussian-usd-test
      tests/gaussian_usd_test.cpp
    )
    merlin_target_defaults(merlin-hydra2-gaussian-usd-test)
    target_link_libraries(merlin-hydra2-gaussian-usd-test PRIVATE
      Merlin::RenderWorld usd usdGeom usdVol)
    target_compile_features(merlin-hydra2-gaussian-usd-test PRIVATE cxx_std_20)
    target_compile_definitions(merlin-hydra2-gaussian-usd-test PRIVATE
      NOMINMAX
      _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING
    )
    add_test(NAME merlin-hydra2-gaussian-usd
      COMMAND merlin-hydra2-gaussian-usd-test
              "${MERLIN_GAUSSIAN_SAMPLE}")
    set_tests_properties(merlin-hydra2-gaussian-usd PROPERTIES
      LABELS "gaussian;hydra;fixture"
      ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${_merlin_pxr_root}/bin;PATH=path_list_prepend:${_merlin_pxr_root}/lib"
    )
  endif()

  add_executable(merlin-hydra2-discovery-test
    tests/plugin_discovery_test.cpp
  )
  merlin_target_defaults(merlin-hydra2-discovery-test)
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
  set(MERLIN_OPENUSD_RUNTIME_ROOT "${_merlin_pxr_root}" PARENT_SCOPE)
  set_tests_properties(merlin-hydra2-discovery PROPERTIES
    LABELS "hydra;discovery"
    ENVIRONMENT "PXR_PLUGINPATH_NAME="
    ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:${_merlin_pxr_root}/bin;PATH=path_list_prepend:${_merlin_pxr_root}/lib"
  )

  # Exercise the staged module and its relative plugInfo library path without
  # rendering. The package fixture stages and links consumers first.
  add_test(NAME merlin-hydra2-install-discovery
    COMMAND merlin-hydra2-discovery-test
      "${PROJECT_BINARY_DIR}/tests/install-consumer-prefix/${_merlin_hydra_install_dir}/resources")
  set_tests_properties(merlin-hydra2-install-discovery PROPERTIES
    LABELS "hydra;packaging;discovery"
    FIXTURES_REQUIRED merlin-installed-package
    RUN_SERIAL TRUE
    ENVIRONMENT "PXR_PLUGINPATH_NAME="
    ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:${_merlin_pxr_root}/bin;PATH=path_list_prepend:${_merlin_pxr_root}/lib")

  add_executable(merlin-hydra2-render-buffer-test
    tests/render_buffer_test.cpp
  )
  merlin_target_defaults(merlin-hydra2-render-buffer-test)
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
    LABELS "hydra;render-buffer"
    ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:${_merlin_pxr_root}/bin;PATH=path_list_prepend:${_merlin_pxr_root}/lib"
  )

  add_executable(merlin-hydra2-render-settings-test
    tests/render_settings_test.cpp
  )

  add_executable(merlin-hydra2-distant-light-test tests/distant_light_test.cpp)
  merlin_target_defaults(merlin-hydra2-distant-light-test)
  target_link_libraries(merlin-hydra2-distant-light-test PRIVATE merlin-hydra2-runtime)
  target_compile_definitions(merlin-hydra2-distant-light-test PRIVATE
    NOMINMAX _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING)
  add_test(NAME merlin-hydra2-distant-light COMMAND merlin-hydra2-distant-light-test)
  set_tests_properties(merlin-hydra2-distant-light PROPERTIES
    LABELS "hydra;lighting;core"
    ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:${_merlin_pxr_root}/bin;PATH=path_list_prepend:${_merlin_pxr_root}/lib")
  merlin_target_defaults(merlin-hydra2-render-settings-test)
  target_link_libraries(merlin-hydra2-render-settings-test
    PRIVATE merlin-hydra2-runtime)
  target_compile_features(merlin-hydra2-render-settings-test
    PRIVATE cxx_std_20)
  target_compile_definitions(merlin-hydra2-render-settings-test PRIVATE
    NOMINMAX
    _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING
  )
  add_test(NAME merlin-hydra2-render-settings
           COMMAND merlin-hydra2-render-settings-test)
  set_tests_properties(merlin-hydra2-render-settings PROPERTIES
    LABELS "hydra;gaussian;settings"
    ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:${_merlin_pxr_root}/bin;PATH=path_list_prepend:${_merlin_pxr_root}/lib"
  )

  add_executable(merlin-hydra2-hgi-vulkan-bridge-test
    tests/hgi_vulkan_bridge_test.cpp
  )
  merlin_target_defaults(merlin-hydra2-hgi-vulkan-bridge-test)
  target_link_libraries(merlin-hydra2-hgi-vulkan-bridge-test
    PRIVATE merlin-hydra2-runtime)
  target_compile_features(merlin-hydra2-hgi-vulkan-bridge-test
    PRIVATE cxx_std_20)
  target_compile_definitions(merlin-hydra2-hgi-vulkan-bridge-test PRIVATE
    NOMINMAX
    _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING
  )
  add_test(NAME merlin-hydra2-hgi-vulkan-bridge
           COMMAND merlin-hydra2-hgi-vulkan-bridge-test)
  set_tests_properties(merlin-hydra2-hgi-vulkan-bridge PROPERTIES
    LABELS "hydra;hgi;vulkan;presentation"
    ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:${_merlin_pxr_root}/bin;PATH=path_list_prepend:${_merlin_pxr_root}/lib"
  )

  if(MERLIN_ENABLE_HGI_METAL_BRIDGE)
    add_executable(merlin-hydra2-hgi-metal-bridge-test
      tests/hgi_metal_bridge_test.cpp
    )
    merlin_target_defaults(merlin-hydra2-hgi-metal-bridge-test)
    target_link_libraries(merlin-hydra2-hgi-metal-bridge-test
      PRIVATE merlin-hydra2-runtime)
    target_compile_features(merlin-hydra2-hgi-metal-bridge-test
      PRIVATE cxx_std_20)
    target_compile_definitions(merlin-hydra2-hgi-metal-bridge-test PRIVATE
      NOMINMAX
      _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING
    )
    add_test(NAME merlin-hydra2-hgi-metal-bridge
      COMMAND merlin-hydra2-hgi-metal-bridge-test)
    set_tests_properties(merlin-hydra2-hgi-metal-bridge PROPERTIES
      LABELS "hydra;hgi;metal;presentation"
      ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${_merlin_pxr_root}/bin;PATH=path_list_prepend:${_merlin_pxr_root}/lib"
    )
  endif()

  if(TARGET Merlin::Metal AND TARGET hgiMetal AND MERLIN_ENABLE_HGI_METAL_BRIDGE)
    add_executable(merlin-hydra2-hgi-metal-aov-test tests/hgi_metal_aov_test.cpp)
    merlin_target_defaults(merlin-hydra2-hgi-metal-aov-test)
    target_link_libraries(merlin-hydra2-hgi-metal-aov-test PRIVATE
      merlin-hydra2-runtime Merlin::Metal Merlin::RenderExtraction)
    add_test(NAME merlin-hydra2-hgi-metal-aov COMMAND merlin-hydra2-hgi-metal-aov-test)
    set_tests_properties(merlin-hydra2-hgi-metal-aov PROPERTIES
      LABELS "hydra;hgi;metal;presentation;gpu;picking" TIMEOUT 30
      SKIP_RETURN_CODE 77
      ENVIRONMENT "HGI_ENABLE_VULKAN=0;MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1"
      ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${_merlin_pxr_root}/bin;PATH=path_list_prepend:${_merlin_pxr_root}/lib")
  endif()

  if(TARGET Merlin::Vulkan AND TARGET hgiVulkan AND MERLIN_ENABLE_HGI_VULKAN_BRIDGE)
    add_executable(merlin-hydra2-hgi-vulkan-aov-test tests/hgi_vulkan_aov_test.cpp)
    merlin_target_defaults(merlin-hydra2-hgi-vulkan-aov-test)
    target_link_libraries(merlin-hydra2-hgi-vulkan-aov-test PRIVATE
      merlin-hydra2-runtime Merlin::Vulkan Merlin::RenderExtraction)
    target_compile_definitions(merlin-hydra2-hgi-vulkan-aov-test PRIVATE
      NOMINMAX _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING)
    add_dependencies(merlin-hydra2-hgi-vulkan-aov-test hdMerlin)
    add_test(NAME merlin-hydra2-hgi-vulkan-aov
      COMMAND merlin-hydra2-hgi-vulkan-aov-test
        "$<TARGET_FILE_DIR:hdMerlin>/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}")
    set_tests_properties(merlin-hydra2-hgi-vulkan-aov PROPERTIES
      LABELS "hydra;hgi;vulkan;presentation;gpu;picking" TIMEOUT 30
      SKIP_RETURN_CODE 77
      ENVIRONMENT "HGI_ENABLE_VULKAN=1;HGIVULKAN_DEBUG=1"
      ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${_merlin_pxr_root}/bin;PATH=path_list_prepend:${_merlin_pxr_root}/lib")
  endif()

  find_package(Python3 COMPONENTS Interpreter QUIET)
  find_program(MERLIN_TESTUSDVIEW_SCRIPT testusdview
    PATHS "${_merlin_pxr_root}/bin" NO_DEFAULT_PATH)
  if(Python3_Interpreter_FOUND AND MERLIN_TESTUSDVIEW_SCRIPT)
    if(TARGET Merlin::Vulkan)
      set(_merlin_hydra2_test_backend vulkan)
    else()
      set(_merlin_hydra2_test_backend metal)
    endif()
    if(TARGET Merlin::Vulkan)
      add_test(
        NAME merlin-hydra2-gaussian-half-usdview-smoke
        COMMAND ${CMAKE_COMMAND}
          "-DMERLIN_CMAKE_COMMAND=${CMAKE_COMMAND}"
          "-DMERLIN_BUILD_DIR=${CMAKE_BINARY_DIR}"
          "-DMERLIN_CONFIG=$<CONFIG>"
          "-DMERLIN_STAGE_DIR=${CMAKE_CURRENT_BINARY_DIR}/gaussian-half-usdview-install"
          "-DMERLIN_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
          "-DMERLIN_PXR_ROOT=${_merlin_pxr_root}"
          "-DMERLIN_PYTHON=${Python3_EXECUTABLE}"
          "-DMERLIN_TESTUSDVIEW=${MERLIN_TESTUSDVIEW_SCRIPT}"
          "-DMERLIN_GAUSSIAN_SAMPLE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gaussian_half.usda"
          "-DMERLIN_GAUSSIAN_EXPECT_POLICY_FALLBACK=1"
          "-DMERLIN_GAUSSIAN_USDVIEW_TEST_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/gaussian_usdview_test.py"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_gaussian_usdview_smoke.cmake"
      )
      set_tests_properties(
        merlin-hydra2-gaussian-half-usdview-smoke PROPERTIES
        LABELS "gaussian;hydra;host-smoke;half"
        TIMEOUT 60
        RUN_SERIAL TRUE)
      if(TARGET hgiVulkan)
        # Exercises tiled control transfers with Hgi's synchronization
        # validation as well as the color-copy/readback boundary.
        add_test(NAME merlin-hydra2-gaussian-hgi-vulkan-benchmark
          COMMAND ${CMAKE_COMMAND}
            "-DMERLIN_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DMERLIN_CONFIG=$<CONFIG>"
            "-DMERLIN_STAGE_DIR=${CMAKE_CURRENT_BINARY_DIR}/gaussian-hgi-vulkan-benchmark-install"
            "-DMERLIN_PXR_ROOT=${_merlin_pxr_root}"
            "-DMERLIN_PYTHON=${Python3_EXECUTABLE}"
            "-DMERLIN_TESTUSDVIEW=${MERLIN_TESTUSDVIEW_SCRIPT}"
            "-DMERLIN_GAUSSIAN_SAMPLE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/leica-sofort-top8192.usdc"
            "-DMERLIN_FORCE_HGI_VULKAN=ON"
            "-DMERLIN_GAUSSIAN_VALIDATE=ON"
            "-DMERLIN_GAUSSIAN_FRAMES=4"
            -P "${PROJECT_SOURCE_DIR}/tests/run_gaussian_host_benchmark.cmake")
        set_tests_properties(merlin-hydra2-gaussian-hgi-vulkan-benchmark PROPERTIES
          LABELS "benchmark;gaussian;hydra;host-smoke;hgi;vulkan;gpu-driven;presentation"
          TIMEOUT 60 RUN_SERIAL TRUE)
        add_test(
          NAME merlin-hydra2-gaussian-half-hgi-vulkan-usdview-smoke
          COMMAND ${CMAKE_COMMAND}
            "-DMERLIN_CMAKE_COMMAND=${CMAKE_COMMAND}"
            "-DMERLIN_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DMERLIN_CONFIG=$<CONFIG>"
            "-DMERLIN_STAGE_DIR=${CMAKE_CURRENT_BINARY_DIR}/gaussian-half-hgi-vulkan-usdview-install"
            "-DMERLIN_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
            "-DMERLIN_PXR_ROOT=${_merlin_pxr_root}"
            "-DMERLIN_PYTHON=${Python3_EXECUTABLE}"
            "-DMERLIN_TESTUSDVIEW=${MERLIN_TESTUSDVIEW_SCRIPT}"
            "-DMERLIN_GAUSSIAN_SAMPLE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gaussian_half.usda"
            "-DMERLIN_FORCE_HGI_VULKAN=ON"
            "-DMERLIN_GAUSSIAN_USDVIEW_TEST_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/gaussian_usdview_test.py"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_gaussian_usdview_smoke.cmake"
        )
        set_tests_properties(
          merlin-hydra2-gaussian-half-hgi-vulkan-usdview-smoke PROPERTIES
          LABELS "gaussian;hydra;host-smoke;hgi;vulkan;presentation;half"
          TIMEOUT 60
          RUN_SERIAL TRUE)
      endif()
    endif()
    if(MERLIN_GAUSSIAN_SAMPLE AND TARGET Merlin::Vulkan)
      add_test(
        NAME merlin-hydra2-gaussian-usdview-smoke
        COMMAND ${CMAKE_COMMAND}
          "-DMERLIN_CMAKE_COMMAND=${CMAKE_COMMAND}"
          "-DMERLIN_BUILD_DIR=${CMAKE_BINARY_DIR}"
          "-DMERLIN_CONFIG=$<CONFIG>"
          "-DMERLIN_STAGE_DIR=${CMAKE_CURRENT_BINARY_DIR}/gaussian-usdview-install"
          "-DMERLIN_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
          "-DMERLIN_PXR_ROOT=${_merlin_pxr_root}"
          "-DMERLIN_PYTHON=${Python3_EXECUTABLE}"
          "-DMERLIN_TESTUSDVIEW=${MERLIN_TESTUSDVIEW_SCRIPT}"
          "-DMERLIN_GAUSSIAN_SAMPLE=${MERLIN_GAUSSIAN_SAMPLE}"
          "-DMERLIN_GAUSSIAN_REFERENCE_IMAGE=${_merlin_gaussian_reference_image}"
          "-DMERLIN_GAUSSIAN_USDVIEW_TEST_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/gaussian_usdview_test.py"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_gaussian_usdview_smoke.cmake"
      )
      set_tests_properties(merlin-hydra2-gaussian-usdview-smoke PROPERTIES
        LABELS "gaussian;hydra;host-smoke"
        TIMEOUT 60
        RUN_SERIAL TRUE)
      # usdview selects GPU Gaussian execution through the Hydra render
      # settings alone. Require makes an unavailable stage fail the smoke
      # instead of falling back to the CPU-sorted reference.
      foreach(_merlin_gaussian_raster IN ITEMS sorted-stream tiled)
        set(_merlin_gaussian_gpu_test
            "merlin-hydra2-gaussian-gpu-${_merlin_gaussian_raster}-usdview-smoke")
        add_test(
          NAME "${_merlin_gaussian_gpu_test}"
          COMMAND ${CMAKE_COMMAND}
            "-DMERLIN_CMAKE_COMMAND=${CMAKE_COMMAND}"
            "-DMERLIN_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DMERLIN_CONFIG=$<CONFIG>"
            "-DMERLIN_STAGE_DIR=${CMAKE_CURRENT_BINARY_DIR}/gaussian-gpu-${_merlin_gaussian_raster}-usdview-install"
            "-DMERLIN_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
            "-DMERLIN_PXR_ROOT=${_merlin_pxr_root}"
            "-DMERLIN_PYTHON=${Python3_EXECUTABLE}"
            "-DMERLIN_TESTUSDVIEW=${MERLIN_TESTUSDVIEW_SCRIPT}"
            "-DMERLIN_GAUSSIAN_SAMPLE=${MERLIN_GAUSSIAN_SAMPLE}"
            "-DMERLIN_GAUSSIAN_REFERENCE_IMAGE=${_merlin_gaussian_reference_image}"
            "-DMERLIN_GAUSSIAN_GPU_MODE=require"
            "-DMERLIN_GAUSSIAN_GPU_RASTER=${_merlin_gaussian_raster}"
            "-DMERLIN_HYDRA_REPORT_SCRIPT=${PROJECT_SOURCE_DIR}/scripts/hydra-performance-report.py"
            "-DMERLIN_GAUSSIAN_USDVIEW_TEST_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/gaussian_usdview_test.py"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_gaussian_usdview_smoke.cmake"
        )
        set_tests_properties("${_merlin_gaussian_gpu_test}" PROPERTIES
          LABELS "gaussian;hydra;host-smoke;gpu-driven"
          TIMEOUT 60
          RUN_SERIAL TRUE)
      endforeach()
      if(TARGET hgiVulkan)
        add_test(
          NAME merlin-hydra2-gaussian-hgi-vulkan-usdview-smoke
          COMMAND ${CMAKE_COMMAND}
            "-DMERLIN_CMAKE_COMMAND=${CMAKE_COMMAND}"
            "-DMERLIN_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DMERLIN_CONFIG=$<CONFIG>"
            "-DMERLIN_STAGE_DIR=${CMAKE_CURRENT_BINARY_DIR}/gaussian-hgi-vulkan-usdview-install"
            "-DMERLIN_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
            "-DMERLIN_PXR_ROOT=${_merlin_pxr_root}"
            "-DMERLIN_PYTHON=${Python3_EXECUTABLE}"
            "-DMERLIN_TESTUSDVIEW=${MERLIN_TESTUSDVIEW_SCRIPT}"
            "-DMERLIN_GAUSSIAN_SAMPLE=${MERLIN_GAUSSIAN_SAMPLE}"
            "-DMERLIN_GAUSSIAN_REFERENCE_IMAGE=${_merlin_gaussian_reference_image}"
            "-DMERLIN_FORCE_HGI_VULKAN=ON"
            "-DMERLIN_GAUSSIAN_USDVIEW_TEST_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/gaussian_usdview_test.py"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_gaussian_usdview_smoke.cmake"
        )
        set_tests_properties(
          merlin-hydra2-gaussian-hgi-vulkan-usdview-smoke PROPERTIES
          LABELS "gaussian;hydra;host-smoke;hgi;vulkan;presentation"
          TIMEOUT 60
          RUN_SERIAL TRUE)
      endif()
    endif()
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
        "-DMERLIN_HYDRA2_TEST_BACKEND=${_merlin_hydra2_test_backend}"
        "-DMERLIN_USDVIEW_TEST_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/usdview_smoke_test.py"
        "-DMERLIN_HYDRA_REPORT_SCRIPT=${PROJECT_SOURCE_DIR}/scripts/hydra-performance-report.py"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_usdview_smoke.cmake"
    )
    set_tests_properties(merlin-hydra2-usdview-smoke PROPERTIES
      LABELS "host-smoke;gpu"
      TIMEOUT 60
      RUN_SERIAL TRUE
    )
    if(TARGET Merlin::Vulkan AND TARGET hgiVulkan)
      add_test(
        NAME merlin-hydra2-usdview-hgi-vulkan-smoke
        COMMAND ${CMAKE_COMMAND}
          "-DMERLIN_CMAKE_COMMAND=${CMAKE_COMMAND}"
          "-DMERLIN_BUILD_DIR=${CMAKE_BINARY_DIR}"
          "-DMERLIN_CONFIG=$<CONFIG>"
          "-DMERLIN_STAGE_DIR=${CMAKE_CURRENT_BINARY_DIR}/usdview-hgi-vulkan-install"
          "-DMERLIN_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
          "-DMERLIN_INSTALL_DATADIR=${CMAKE_INSTALL_DATADIR}"
          "-DMERLIN_PXR_ROOT=${_merlin_pxr_root}"
          "-DMERLIN_PYTHON=${Python3_EXECUTABLE}"
          "-DMERLIN_TESTUSDVIEW=${MERLIN_TESTUSDVIEW_SCRIPT}"
          "-DMERLIN_HYDRA2_TEST_BACKEND=vulkan"
          "-DMERLIN_FORCE_HGI_VULKAN=ON"
          "-DMERLIN_USDVIEW_TEST_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/usdview_smoke_test.py"
          "-DMERLIN_HYDRA_REPORT_SCRIPT=${PROJECT_SOURCE_DIR}/scripts/hydra-performance-report.py"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_usdview_smoke.cmake"
      )
      set_tests_properties(merlin-hydra2-usdview-hgi-vulkan-smoke PROPERTIES
        LABELS "host-smoke;gpu;hgi;vulkan;presentation"
        TIMEOUT 60
        RUN_SERIAL TRUE
        FIXTURES_SETUP merlin-hydra2-usdview-hgi-vulkan
      )
      set_tests_properties(merlin-hydra2-usdview-smoke PROPERTIES
        FIXTURES_SETUP merlin-hydra2-usdview-tier0
      )
      add_test(
        NAME merlin-hydra2-usdview-hgi-vulkan-comparison
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/compare_usdview_smoke.py"
          "${CMAKE_CURRENT_BINARY_DIR}/usdview-install"
          "${CMAKE_CURRENT_BINARY_DIR}/usdview-hgi-vulkan-install"
          "${CMAKE_CURRENT_BINARY_DIR}/usdview-hgi-vulkan-comparison.json"
      )
      set_tests_properties(
        merlin-hydra2-usdview-hgi-vulkan-comparison PROPERTIES
        LABELS "host-smoke;gpu;hgi;vulkan;presentation;comparison"
        TIMEOUT 15
        RUN_SERIAL TRUE
        FIXTURES_REQUIRED
          "merlin-hydra2-usdview-tier0;merlin-hydra2-usdview-hgi-vulkan"
      )
    endif()
    # Keep the lighting gate independent of topology, selection and picking.
    # The installed plugin and the real host composition still use the shared
    # smoke runner, telemetry and presentation comparison tolerances.
    set(_merlin_forward_modes tier0)
    if(TARGET Merlin::Vulkan AND TARGET hgiVulkan)
      list(APPEND _merlin_forward_modes hgi-vulkan)
    endif()
    foreach(_merlin_forward_mode IN LISTS _merlin_forward_modes)
      set(_merlin_forward_force_vulkan OFF)
      if(_merlin_forward_mode STREQUAL "hgi-vulkan")
        set(_merlin_forward_force_vulkan ON)
      endif()
      foreach(_merlin_forward_fixture IN ITEMS lighting motion)
        set(_merlin_forward_motion OFF)
        set(_merlin_forward_timeout 90)
        if(_merlin_forward_fixture STREQUAL "motion")
          set(_merlin_forward_motion ON)
          set(_merlin_forward_timeout 180)
        endif()
        add_test(
          NAME merlin-hydra2-forward-${_merlin_forward_fixture}-${_merlin_forward_mode}
          COMMAND ${CMAKE_COMMAND}
            "-DMERLIN_CMAKE_COMMAND=${CMAKE_COMMAND}"
            "-DMERLIN_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DMERLIN_CONFIG=$<CONFIG>"
            "-DMERLIN_STAGE_DIR=${CMAKE_CURRENT_BINARY_DIR}/forward-${_merlin_forward_fixture}-${_merlin_forward_mode}-install"
            "-DMERLIN_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
            "-DMERLIN_INSTALL_DATADIR=${CMAKE_INSTALL_DATADIR}"
            "-DMERLIN_PXR_ROOT=${_merlin_pxr_root}"
            "-DMERLIN_PYTHON=${Python3_EXECUTABLE}"
            "-DMERLIN_TESTUSDVIEW=${MERLIN_TESTUSDVIEW_SCRIPT}"
            "-DMERLIN_HYDRA2_TEST_BACKEND=${_merlin_hydra2_test_backend}"
            "-DMERLIN_FORCE_HGI_VULKAN=${_merlin_forward_force_vulkan}"
            "-DMERLIN_FORWARD_LIGHTING_ONLY=ON"
            "-DMERLIN_FORWARD_MOTION_ONLY=${_merlin_forward_motion}"
            "-DMERLIN_USDVIEW_TEST_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/forward_lighting_usdview_test.py"
            "-DMERLIN_HYDRA_REPORT_SCRIPT=${PROJECT_SOURCE_DIR}/scripts/hydra-performance-report.py"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_usdview_smoke.cmake")
        set_tests_properties(merlin-hydra2-forward-${_merlin_forward_fixture}-${_merlin_forward_mode} PROPERTIES
          LABELS "host-smoke;gpu;lighting;presentation"
          TIMEOUT ${_merlin_forward_timeout} RUN_SERIAL TRUE
          FIXTURES_SETUP merlin-forward-${_merlin_forward_fixture}-${_merlin_forward_mode})
      endforeach()
    endforeach()
    if(TARGET Merlin::Vulkan AND TARGET merlin-headless)
      add_test(
        NAME merlin-hydra2-forward-lighting-native-comparison
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/compare_forward_lighting_native.py"
          "${CMAKE_BINARY_DIR}/tests/forward-images-handwritten"
          "${CMAKE_CURRENT_BINARY_DIR}/forward-lighting-tier0-install/usdview-first-frame-forward-lighting.json"
          "${CMAKE_CURRENT_BINARY_DIR}/forward-lighting-native-comparison.json")
      set_tests_properties(merlin-hydra2-forward-lighting-native-comparison PROPERTIES
        LABELS "host-smoke;gpu;lighting;presentation;comparison"
        TIMEOUT 15 RUN_SERIAL TRUE
        FIXTURES_REQUIRED "merlin-forward-lighting-native;merlin-forward-lighting-tier0")
    endif()
    if(TARGET Merlin::Vulkan AND TARGET hgiVulkan)
      add_test(
        NAME merlin-hydra2-forward-lighting-comparison
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/compare_usdview_smoke.py"
          "${CMAKE_CURRENT_BINARY_DIR}/forward-lighting-tier0-install"
          "${CMAKE_CURRENT_BINARY_DIR}/forward-lighting-hgi-vulkan-install"
          "${CMAKE_CURRENT_BINARY_DIR}/forward-lighting-comparison.json"
          --forward-lighting-only)
      set_tests_properties(merlin-hydra2-forward-lighting-comparison PROPERTIES
        LABELS "host-smoke;gpu;lighting;presentation;comparison"
        TIMEOUT 15 RUN_SERIAL TRUE
        FIXTURES_REQUIRED "merlin-forward-lighting-tier0;merlin-forward-lighting-hgi-vulkan")
      add_test(
        NAME merlin-hydra2-forward-motion-comparison
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/compare_usdview_smoke.py"
          "${CMAKE_CURRENT_BINARY_DIR}/forward-motion-tier0-install"
          "${CMAKE_CURRENT_BINARY_DIR}/forward-motion-hgi-vulkan-install"
          "${CMAKE_CURRENT_BINARY_DIR}/forward-motion-comparison.json"
          --forward-motion-only)
      set_tests_properties(merlin-hydra2-forward-motion-comparison PROPERTIES
        LABELS "host-smoke;gpu;lighting;presentation;comparison"
        TIMEOUT 120 RUN_SERIAL TRUE
        FIXTURES_REQUIRED "merlin-forward-motion-tier0;merlin-forward-motion-hgi-vulkan")
    endif()
  endif()
endif()
