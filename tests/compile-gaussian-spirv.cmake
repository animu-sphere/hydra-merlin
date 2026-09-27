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

# Metal-only builders also compile the production Vulkan wrappers of shared
# preparation and sorting, so changes cannot silently break the other target.
get_filename_component(_source_dir "${MERLIN_GAUSSIAN_SOURCE}" DIRECTORY)
foreach(_kernel prepare_compact sort_keys sort_histogram sort_scan_blocks sort_scan_add sort_scatter sort_verify)
  if(_kernel MATCHES "^prepare")
    set(_source gaussian-prepare.slang)
  else()
    set(_source gaussian-sort.slang)
  endif()
  execute_process(
    COMMAND "${MERLIN_SLANGC_EXECUTABLE}" "${_source_dir}/${_source}"
      -entry "gaussian_${_kernel}" -stage compute -target spirv -profile sm_6_6
      -capability spirv_1_5 -matrix-layout-column-major -O2 -warnings-as-errors all
      -o "${MERLIN_TEST_OUTPUT_DIR}/gaussian_${_kernel}.spv"
    RESULT_VARIABLE _result)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "Vulkan Gaussian compilation failed: gaussian_${_kernel}")
  endif()
endforeach()
