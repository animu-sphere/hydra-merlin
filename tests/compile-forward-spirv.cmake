# Metal-only builders still validate every Vulkan Forward consumer of Core
# policy. GPU execution is separately gated; compilation is not image evidence.
foreach(_variant "" "-bindless" "-gpu-scene" "-gpu-driven")
  foreach(_stage vertex fragment)
    string(REPLACE "-" "_" _suffix "${_variant}")
    set(_entry "forward${_suffix}_${_stage}")
    set(_output "${MERLIN_TEST_OUTPUT_DIR}/forward${_variant}.${_stage}.spv")
    execute_process(
      COMMAND "${MERLIN_SLANGC_EXECUTABLE}"
        "${MERLIN_FORWARD_SOURCE_DIR}/forward${_variant}.slang"
        -entry "${_entry}" -stage "${_stage}" -target spirv -profile sm_6_6
        -matrix-layout-column-major -O2 -warnings-as-errors all -o "${_output}"
      RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
    if(NOT _result EQUAL 0)
      message(FATAL_ERROR "Forward ${_entry} compile failed: ${_stdout}${_stderr}")
    endif()
  endforeach()
endforeach()
