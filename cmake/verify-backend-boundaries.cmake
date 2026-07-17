if(NOT DEFINED MERLIN_SOURCE_DIR)
  message(FATAL_ERROR "MERLIN_SOURCE_DIR is required")
endif()

file(GLOB_RECURSE _core_headers
  "${MERLIN_SOURCE_DIR}/core/merlin-render-world/include/*.h"
  "${MERLIN_SOURCE_DIR}/core/merlin-render-world/include/*.hpp"
  "${MERLIN_SOURCE_DIR}/core/merlin-render-extraction/include/*.h"
  "${MERLIN_SOURCE_DIR}/core/merlin-render-extraction/include/*.hpp"
  "${MERLIN_SOURCE_DIR}/core/merlin-render-backend/include/*.h"
  "${MERLIN_SOURCE_DIR}/core/merlin-render-backend/include/*.hpp")

foreach(_header IN LISTS _core_headers)
  file(READ "${_header}" _contents)
  if(_contents MATCHES "<vulkan/|Vk[A-Z]|<Metal/|id[ \t]*<[ \t]*MTL|<GLFW/|<windows[.]h>|pxr::|PXR_")
    message(FATAL_ERROR
      "host/backend-specific type leaked into Core public header: ${_header}")
  endif()
endforeach()

set(_hydra_public_header
    "${MERLIN_SOURCE_DIR}/adapters/merlin-hydra2/src/adapter.hpp")
if(EXISTS "${_hydra_public_header}")
  file(READ "${_hydra_public_header}" _contents)
  if(_contents MATCHES "<vulkan/|Vk[A-Z]|<Metal/|id[ \t]*<[ \t]*MTL|<GLFW/|<windows[.]h>")
    message(FATAL_ERROR
      "backend/window type leaked into the Hydra public boundary: ${_hydra_public_header}")
  endif()
endif()
