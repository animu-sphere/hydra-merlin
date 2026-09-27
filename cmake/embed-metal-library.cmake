file(READ "${MERLIN_METALLIB}" _bytes HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_bytes}")
file(WRITE "${MERLIN_METAL_HEADER}"
  "// Generated from the compiled Metal library; do not edit.\n"
  "alignas(16) static const unsigned char kGaussianMetalLibrary[] = {${_bytes}};\n")
