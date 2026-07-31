if(MERLIN_ENABLE_HGI_VULKAN_BRIDGE)
  # Vulkan-enabled OpenUSD packages expose hgiVulkan through pxrTargets.cmake,
  # whose imported link interface names Vulkan::Vulkan.
  find_package(Vulkan REQUIRED COMPONENTS shaderc_combined)
endif()
find_package(pxr CONFIG REQUIRED)
include(GNUInstallDirs)

get_target_property(_merlin_pxr_include_dirs hd INTERFACE_INCLUDE_DIRECTORIES)
set(_merlin_pxr_header "")
foreach(_merlin_pxr_include_dir IN LISTS _merlin_pxr_include_dirs)
  if(EXISTS "${_merlin_pxr_include_dir}/pxr/pxr.h")
    set(_merlin_pxr_header "${_merlin_pxr_include_dir}/pxr/pxr.h")
    set(_merlin_pxr_include_dir_selected "${_merlin_pxr_include_dir}")
    break()
  endif()
endforeach()
if(NOT _merlin_pxr_header)
  message(FATAL_ERROR
    "hdMerlin could not locate pxr/pxr.h through the imported hd target; "
    "use a complete OpenUSD SDK package")
endif()

file(STRINGS "${_merlin_pxr_header}" _merlin_pxr_version_line
  REGEX "^#define PXR_VERSION [0-9]+$")
if(NOT _merlin_pxr_version_line)
  message(FATAL_ERROR
    "hdMerlin could not read PXR_VERSION from ${_merlin_pxr_header}")
endif()
string(REGEX REPLACE ".*PXR_VERSION ([0-9]+).*" "\\1"
  _merlin_pxr_version_number "${_merlin_pxr_version_line}")
math(EXPR _merlin_pxr_version_major
  "${_merlin_pxr_version_number} / 100")
math(EXPR _merlin_pxr_version_minor
  "${_merlin_pxr_version_number} % 100")
if(_merlin_pxr_version_minor LESS 10)
  set(_merlin_pxr_version_minor "0${_merlin_pxr_version_minor}")
endif()
set(_merlin_openusd_detected_version
  "${_merlin_pxr_version_major}.${_merlin_pxr_version_minor}")
set(MERLIN_OPENUSD_DETECTED_VERSION
  "${_merlin_openusd_detected_version}" PARENT_SCOPE)

if(NOT _merlin_openusd_detected_version IN_LIST
   MERLIN_OPENUSD_VALIDATED_VERSIONS)
  string(JOIN ", " _merlin_openusd_supported_versions
    ${MERLIN_OPENUSD_VALIDATED_VERSIONS})
  message(FATAL_ERROR
    "hdMerlin validates OpenUSD ${_merlin_openusd_supported_versions}, but "
    "${_merlin_openusd_detected_version} was found at "
    "${_merlin_pxr_include_dir_selected}. Use the validated SDK or disable "
    "MERLIN_ENABLE_HYDRA2; Core and native GPU backends remain "
    "OpenUSD-independent.")
endif()
set(MERLIN_OPENUSD_VALIDATED_VERSION
  "${_merlin_openusd_detected_version}" PARENT_SCOPE)

get_target_property(_merlin_hd_target_type hd TYPE)
if(NOT _merlin_hd_target_type STREQUAL "SHARED_LIBRARY")
  message(FATAL_ERROR
    "hdMerlin requires the validated shared-library OpenUSD layout; imported "
    "target hd is ${_merlin_hd_target_type}. Static/monolithic plugin ABI "
    "support is not yet validated.")
endif()

set(_merlin_openusd_release_only FALSE)
if(MSVC)
  get_target_property(_merlin_hd_imported_configurations hd
    IMPORTED_CONFIGURATIONS)
  if(_merlin_hd_imported_configurations)
    list(TRANSFORM _merlin_hd_imported_configurations TOUPPER)
    if(NOT "DEBUG" IN_LIST _merlin_hd_imported_configurations)
      set(_merlin_openusd_release_only TRUE)
      message(STATUS
        "OpenUSD ${_merlin_openusd_detected_version} provides no Debug "
        "libraries; hdMerlin Debug builds will be rejected to prevent an "
        "MSVC runtime/iterator ABI mismatch")
    endif()
  endif()
endif()

if(NOT TARGET Merlin::Vulkan AND NOT TARGET Merlin::Metal)
  message(FATAL_ERROR
    "MERLIN_ENABLE_HYDRA2 requires MERLIN_ENABLE_VULKAN=ON or "
    "MERLIN_ENABLE_METAL=ON")
endif()

add_library(merlin-hydra2-runtime STATIC
  src/adapter.cpp
  src/hgi_vulkan_bridge.cpp
)
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
  get_filename_component(_merlin_pxr_root
    "${_merlin_pxr_include_dir_selected}/.." ABSOLUTE)
  set(MERLIN_OPENUSD_RUNTIME_ROOT "${_merlin_pxr_root}" PARENT_SCOPE)
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

  add_executable(merlin-hydra2-hgi-vulkan-bridge-test
    tests/hgi_vulkan_bridge_test.cpp
  )
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

  find_package(Python3 COMPONENTS Interpreter QUIET)
  find_program(MERLIN_TESTUSDVIEW_SCRIPT testusdview
    PATHS "${_merlin_pxr_root}/bin" NO_DEFAULT_PATH)
  if(Python3_Interpreter_FOUND AND MERLIN_TESTUSDVIEW_SCRIPT)
    if(TARGET Merlin::Vulkan)
      set(_merlin_hydra2_test_backend vulkan)
    else()
      set(_merlin_hydra2_test_backend metal)
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
        "-DMERLIN_HGI_METAL_BRIDGE_ENABLED:BOOL=${MERLIN_ENABLE_HGI_METAL_BRIDGE}"
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
  endif()
endif()
