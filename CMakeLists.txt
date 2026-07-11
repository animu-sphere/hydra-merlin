cmake_minimum_required(VERSION 3.24)

project(hdMerlin VERSION 0.1.0 LANGUAGES CXX)

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
