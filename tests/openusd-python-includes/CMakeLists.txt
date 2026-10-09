cmake_minimum_required(VERSION 3.24)
project(MerlinOpenUSDPythonIncludes NONE)
include("${MERLIN_SOURCE_DIR}/cmake/MerlinOpenUSDPython.cmake")

set(_producer "${CMAKE_CURRENT_BINARY_DIR}/missing-producer/Python313/Include")
set(_consumer "${CMAKE_CURRENT_BINARY_DIR}/consumer-python")
set(_sdk "${CMAKE_CURRENT_BINARY_DIR}/sdk")
set(_other "${CMAKE_CURRENT_BINARY_DIR}/other-dependency")
file(MAKE_DIRECTORY "${_consumer}" "${_sdk}" "${_other}")

# Non-Python SDKs and SDKs without producer system includes remain usable.
merlin_rebind_openusd_python_includes()
add_library(Python3::Python INTERFACE IMPORTED)
set_property(TARGET Python3::Python PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${_consumer}")
add_library(python INTERFACE IMPORTED)
set(PXR_LIBRARIES python hd sdkOnly nonTargetLibrary)
merlin_rebind_openusd_python_includes()

add_library(hd INTERFACE IMPORTED)
set_target_properties(hd PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${_sdk};${_producer};${_consumer};${_other}"
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_producer};${_other}")
set_target_properties(python PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${_sdk};${_producer}"
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_producer}")
add_library(sdkOnly INTERFACE IMPORTED)
set_property(TARGET sdkOnly PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${_sdk}")
add_library(unrelated INTERFACE IMPORTED)
set_property(TARGET unrelated PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${_producer}")

merlin_rebind_openusd_python_includes()
# Repeating package/dependency lookup must not duplicate include directories.
merlin_rebind_openusd_python_includes()
foreach(_case "hd|INTERFACE_INCLUDE_DIRECTORIES|${_sdk};${_consumer};${_other}"
    "hd|INTERFACE_SYSTEM_INCLUDE_DIRECTORIES|${_consumer};${_other}"
    "python|INTERFACE_INCLUDE_DIRECTORIES|${_sdk};${_consumer}"
    "python|INTERFACE_SYSTEM_INCLUDE_DIRECTORIES|${_consumer}"
    "sdkOnly|INTERFACE_INCLUDE_DIRECTORIES|${_sdk}"
    "unrelated|INTERFACE_INCLUDE_DIRECTORIES|${_producer}")
  string(REPLACE "|" ";" _fields "${_case}")
  list(POP_FRONT _fields _target _property)
  get_target_property(_actual "${_target}" "${_property}")
  if(NOT "${_actual}" STREQUAL "${_fields}")
    message(FATAL_ERROR "${_target}.${_property}: expected '${_fields}', got '${_actual}'")
  endif()
endforeach()
# Generation itself must also accept a consumer of the imported Hydra target.
add_library(consumer INTERFACE)
target_link_libraries(consumer INTERFACE hd python sdkOnly)
