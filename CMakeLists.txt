cmake_minimum_required(VERSION 3.24)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" MERLIN_PROJECT_VERSION)
string(STRIP "${MERLIN_PROJECT_VERSION}" MERLIN_PROJECT_VERSION)
if(NOT MERLIN_PROJECT_VERSION MATCHES
   "^[0-9]+[.][0-9]+[.][0-9]+$")
  message(FATAL_ERROR
    "VERSION must contain exactly one stable SemVer value (MAJOR.MINOR.PATCH)")
endif()

project(hdMerlin VERSION "${MERLIN_PROJECT_VERSION}" LANGUAGES CXX)

set(MERLIN_VULKAN_MIN_VERSION_MAJOR 1)
set(MERLIN_VULKAN_MIN_VERSION_MINOR 4)
set(MERLIN_VULKAN_MIN_VERSION
    "${MERLIN_VULKAN_MIN_VERSION_MAJOR}.${MERLIN_VULKAN_MIN_VERSION_MINOR}")
set(MERLIN_OPENUSD_VALIDATED_VERSION "26.05")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(MERLIN_BUILD_TESTS "Build Merlin tests" ON)
option(MERLIN_ENABLE_VULKAN "Build the Vulkan backend" ON)
option(MERLIN_ENABLE_HYDRA2 "Build the OpenUSD Hydra 2 adapter" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MERLIN_BUILD_TESTS)
  include(CTest)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /EHsc)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_subdirectory(core/merlin-render-world)
add_subdirectory(core/merlin-render-extraction)

if(MERLIN_ENABLE_VULKAN)
  add_subdirectory(backend/merlin-vulkan)
  add_subdirectory(adapters/merlin-headless)
  add_subdirectory(adapters/merlin-benchmark)
endif()

if(MERLIN_ENABLE_HYDRA2)
  add_subdirectory(adapters/merlin-hydra2)
endif()

if(MERLIN_BUILD_TESTS)
  add_subdirectory(tests)
endif()

set(_merlin_install_cmakedir "${CMAKE_INSTALL_LIBDIR}/cmake/Merlin")

set(_merlin_core_targets
  merlin-render-world
  merlin-render-extraction
)

install(TARGETS ${_merlin_core_targets}
  EXPORT MerlinTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
if(TARGET merlin-vulkan)
  install(TARGETS merlin-vulkan
    EXPORT MerlinVulkanTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  )
endif()
install(DIRECTORY core/merlin-render-world/include/
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY core/merlin-render-extraction/include/
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
if(TARGET merlin-vulkan)
  install(DIRECTORY backend/merlin-vulkan/include/
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
endif()

install(EXPORT MerlinTargets
  FILE MerlinTargets.cmake
  NAMESPACE Merlin::
  DESTINATION "${_merlin_install_cmakedir}"
)
if(TARGET merlin-vulkan)
  install(EXPORT MerlinVulkanTargets
    FILE MerlinVulkanTargets.cmake
    NAMESPACE Merlin::
    DESTINATION "${_merlin_install_cmakedir}"
  )
endif()

configure_package_config_file(
  cmake/MerlinConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/MerlinConfig.cmake"
  INSTALL_DESTINATION "${_merlin_install_cmakedir}"
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/MerlinConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/MerlinConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/MerlinConfigVersion.cmake"
  DESTINATION "${_merlin_install_cmakedir}"
)

if(MERLIN_ENABLE_VULKAN)
  set(_merlin_metadata_vulkan_enabled true)
  set(_merlin_metadata_vulkan_version "null")
  if(DEFINED MERLIN_VULKAN_DETECTED_VERSION AND
     MERLIN_VULKAN_DETECTED_VERSION MATCHES "^[0-9A-Za-z.+_-]+$")
    set(_merlin_metadata_vulkan_version
        "\"${MERLIN_VULKAN_DETECTED_VERSION}\"")
  endif()
  set(_merlin_metadata_vulkan_sdk_version "null")
  if(DEFINED MERLIN_VULKAN_SDK_DETECTED_VERSION AND
     MERLIN_VULKAN_SDK_DETECTED_VERSION MATCHES "^[0-9A-Za-z.+_-]+$")
    set(_merlin_metadata_vulkan_sdk_version
        "\"${MERLIN_VULKAN_SDK_DETECTED_VERSION}\"")
  endif()
  set(_merlin_metadata_exported_vulkan
      ",\n      \"Merlin::Vulkan\"")
  set(_merlin_metadata_runtime_products
      "      \"merlin-headless\",\n      \"merlin-benchmark\"")
else()
  set(_merlin_metadata_vulkan_enabled false)
  set(_merlin_metadata_vulkan_version "null")
  set(_merlin_metadata_vulkan_sdk_version "null")
  set(_merlin_metadata_exported_vulkan "")
  set(_merlin_metadata_runtime_products "")
endif()

if(MERLIN_ENABLE_HYDRA2)
  set(_merlin_metadata_hydra2_enabled true)
  set(_merlin_metadata_openusd_version "null")
  if(DEFINED MERLIN_OPENUSD_DETECTED_VERSION AND
     MERLIN_OPENUSD_DETECTED_VERSION MATCHES "^[0-9A-Za-z.+_-]+$")
    set(_merlin_metadata_openusd_version
        "\"${MERLIN_OPENUSD_DETECTED_VERSION}\"")
  endif()
  if(_merlin_metadata_runtime_products)
    string(APPEND _merlin_metadata_runtime_products ",\n")
  endif()
  string(APPEND _merlin_metadata_runtime_products "      \"hdMerlin\"")
else()
  set(_merlin_metadata_hydra2_enabled false)
  set(_merlin_metadata_openusd_version "null")
endif()

if(_merlin_metadata_runtime_products)
  set(_merlin_metadata_runtime_products
      "[\n${_merlin_metadata_runtime_products}\n    ]")
else()
  set(_merlin_metadata_runtime_products "[]")
endif()

configure_file(
  cmake/merlin-release-metadata.json.in
  "${CMAKE_CURRENT_BINARY_DIR}/merlin-release-metadata.json"
  @ONLY
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/merlin-release-metadata.json"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/merlin"
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/merlin"
)

if(MERLIN_BUILD_TESTS)
  add_test(
    NAME merlin-release-metadata
    COMMAND ${CMAKE_COMMAND}
      "-DMERLIN_METADATA:FILEPATH=${CMAKE_CURRENT_BINARY_DIR}/merlin-release-metadata.json"
      "-DMERLIN_EXPECTED_VERSION:STRING=${PROJECT_VERSION}"
      "-DMERLIN_EXPECTED_VULKAN:BOOL=${MERLIN_ENABLE_VULKAN}"
      "-DMERLIN_EXPECTED_HYDRA2:BOOL=${MERLIN_ENABLE_HYDRA2}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify-release-metadata.cmake"
  )
  set_tests_properties(merlin-release-metadata PROPERTIES LABELS packaging)
endif()
