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

# adapter.hpp includes both presentation bridge headers and embeds their
# status/telemetry in HdMerlinViewportFrame, so they are part of the same public
# boundary and have to be held to the same rule.
set(_hydra_public_headers
    "${MERLIN_SOURCE_DIR}/adapters/merlin-hydra2/src/adapter.hpp"
    "${MERLIN_SOURCE_DIR}/adapters/merlin-hydra2/src/hgi_vulkan_bridge.hpp"
    "${MERLIN_SOURCE_DIR}/adapters/merlin-hydra2/src/hgi_metal_bridge.hpp")
foreach(_hydra_public_header IN LISTS _hydra_public_headers)
  if(EXISTS "${_hydra_public_header}")
    file(READ "${_hydra_public_header}" _contents)
    if(_contents MATCHES "<vulkan/|Vk[A-Z]|<Metal/|id[ \t]*<[ \t]*MTL|<GLFW/|<windows[.]h>")
      message(FATAL_ERROR
        "backend/window type leaked into the Hydra public boundary: ${_hydra_public_header}")
    endif()
  endif()
endforeach()
