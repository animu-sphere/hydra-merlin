cmake_minimum_required(VERSION 3.24)

project(MerlinInstallConsumer LANGUAGES CXX)

if(MERLIN_TEST_REQUIRED_COMPONENT)
  find_package(Merlin 0.1 REQUIRED COMPONENTS ${MERLIN_TEST_REQUIRED_COMPONENT})
  message(FATAL_ERROR "Unsupported required component was accepted")
endif()

find_package(Merlin 0.1 REQUIRED COMPONENTS Core RenderExtraction RenderBackend
  OPTIONAL_COMPONENTS UnknownComponent Hydra)
if(Merlin_UnknownComponent_FOUND OR Merlin_Hydra_FOUND)
  message(FATAL_ERROR "Unsupported optional component was reported as found")
endif()
foreach(_backend IN ITEMS Vulkan Metal MaterialX)
  if(TARGET Merlin::${_backend})
    message(FATAL_ERROR "Core consumption unexpectedly loaded ${_backend}")
  endif()
endforeach()
foreach(_core IN ITEMS RenderWorld RenderExtraction RenderBackend)
  get_target_property(_links Merlin::${_core} INTERFACE_LINK_LIBRARIES)
  if(_links MATCHES "merlin-compiler-options")
    message(FATAL_ERROR "Compiler policy leaked into the installed SDK")
  endif()
endforeach()

add_executable(merlin-install-consumer main.cpp)
target_link_libraries(merlin-install-consumer PRIVATE
  Merlin::RenderExtraction
  Merlin::RenderBackend
)
