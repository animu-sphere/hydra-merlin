if(NOT DEFINED MERLIN_METAL_SYMBOL)
  set(MERLIN_METAL_SYMBOL kGaussianMetalLibrary)
endif()
if(NOT MERLIN_METAL_SYMBOL MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
  message(FATAL_ERROR "MERLIN_METAL_SYMBOL must be a C++ identifier")
endif()
file(READ "${MERLIN_METALLIB}" _bytes HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_bytes}")
file(WRITE "${MERLIN_METAL_HEADER}"
  "// Generated from the compiled Metal library; do not edit.\n"
  "alignas(16) static const unsigned char ${MERLIN_METAL_SYMBOL}[] = {${_bytes}};\n")
