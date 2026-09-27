# The shared raster math must keep compiling for Vulkan on Metal-only builders.
foreach(_entry gaussian_vertex gaussian_id_vertex gaussian_fragment gaussian_id_fragment)
  if(_entry MATCHES "vertex$")
    set(_stage vertex)
  else()
    set(_stage fragment)
  endif()
  execute_process(
    COMMAND "${MERLIN_SLANGC_EXECUTABLE}" "${MERLIN_GAUSSIAN_SOURCE}"
      -entry "${_entry}" -stage "${_stage}" -target spirv -profile sm_6_6
      -capability spirv_1_5 -matrix-layout-column-major -O2 -warnings-as-errors all
      -o "${MERLIN_TEST_OUTPUT_DIR}/${_entry}.spv"
    RESULT_VARIABLE _result)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "Vulkan Gaussian compilation failed: ${_entry}")
  endif()
endforeach()
