cmake_minimum_required(VERSION 3.24)

project(hdMerlin VERSION 0.1.0 LANGUAGES CXX)

set(MERLIN_VULKAN_MIN_VERSION_MAJOR 1)
set(MERLIN_VULKAN_MIN_VERSION_MINOR 4)
set(MERLIN_VULKAN_MIN_VERSION
    "${MERLIN_VULKAN_MIN_VERSION_MAJOR}.${MERLIN_VULKAN_MIN_VERSION_MINOR}")

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
