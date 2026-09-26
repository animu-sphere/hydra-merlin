cmake_minimum_required(VERSION 3.24)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(MerlinVersions)

if(APPLE)
  project(hdMerlin VERSION "${MERLIN_PROJECT_VERSION}" LANGUAGES CXX OBJCXX)
else()
  project(hdMerlin VERSION "${MERLIN_PROJECT_VERSION}" LANGUAGES CXX)
endif()

include(GNUInstallDirs)
include(MerlinOptions)
include(MerlinCompiler)
include(MerlinOpenStrata)
include(MerlinShaders)
merlin_validate_openstrata_metadata()

if(MERLIN_BUILD_TESTS)
  include(CTest)
endif()

include(MerlinDependencies)

add_subdirectory(core/merlin-render-world)
add_subdirectory(core/merlin-render-extraction)
add_subdirectory(core/merlin-render-backend)

if(MERLIN_ENABLE_MATERIALX)
  add_subdirectory(material/merlin-materialx)
endif()

if(MERLIN_ENABLE_VULKAN)
  add_subdirectory(backend/merlin-vulkan)
  add_subdirectory(adapters/merlin-headless)
  add_subdirectory(adapters/merlin-benchmark)
endif()

if(MERLIN_ENABLE_METAL)
  add_subdirectory(backend/merlin-metal)
endif()

if(MERLIN_ENABLE_HYDRA2)
  add_subdirectory(adapters/merlin-hydra2)
endif()

if((MERLIN_ENABLE_VULKAN OR MERLIN_ENABLE_METAL) AND MERLIN_BUILD_VIEWPORT)
  add_subdirectory(adapters/merlin-viewport)
endif()

merlin_add_materialx_shader_artifacts()

if(MERLIN_BUILD_TESTS)
  add_subdirectory(tests)
endif()

include(MerlinInstall)
include(MerlinPackaging)
