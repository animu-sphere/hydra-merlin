# Native Metal source/reflection gate without requiring Xcode or a GPU.
foreach(_required MERLIN_SLANGC_EXECUTABLE MERLIN_SOURCE_DIR MERLIN_TEST_OUTPUT_DIR MERLIN_PYTHON)
  if(NOT DEFINED ${_required})
    message(FATAL_ERROR "${_required} is required")
  endif()
endforeach()
file(MAKE_DIRECTORY "${MERLIN_TEST_OUTPUT_DIR}")
set(_entries
  "forward-metal.slang|merlin_vertex|vertex|forward.vertex"
  "forward-metal.slang|merlin_fragment_conventional|fragment|forward.conventional"
  "forward-metal.slang|merlin_fragment_argument_buffer|fragment|forward.argument-buffer"
  "forward-gpu-scene-metal.slang|merlin_vertex_gpu_scene|vertex|forward.gpu-scene-vertex"
  "forward-gpu-scene-metal.slang|merlin_fragment_gpu_scene|fragment|forward.gpu-scene-fragment"
  "color-convert-metal.slang|merlin_convert_color|compute|forward.color-convert"
  "presentation-metal.slang|merlin_presentation_vertex|vertex|presentation.vertex"
  "presentation-metal.slang|merlin_presentation_fragment|fragment|presentation.fragment")
foreach(_record IN LISTS _entries)
  string(REPLACE "|" ";" _fields "${_record}")
  list(GET _fields 0 _source)
  list(GET _fields 1 _entry)
  list(GET _fields 2 _stage)
  list(GET _fields 3 _name)
  set(_output "${MERLIN_TEST_OUTPUT_DIR}/${_name}.metal")
  execute_process(
    COMMAND "${MERLIN_SLANGC_EXECUTABLE}" "${_source}"
      -entry "${_entry}" -stage "${_stage}" -target metal -profile metallib_2_4
      -matrix-layout-column-major -O2 -warnings-as-errors all
      -reflection-json "${_output}.reflection.json" -o "${_output}"
    WORKING_DIRECTORY "${MERLIN_SOURCE_DIR}/backend/merlin-metal/shaders"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "${_entry} MSL generation failed: ${_stdout}\n${_stderr}")
  endif()
  if(_source STREQUAL "forward-gpu-scene-metal.slang")
    # Normalization uses its own explicit source variable.
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DMERLIN_METAL_SOURCE=${_output}"
      -P "${MERLIN_SOURCE_DIR}/cmake/normalize-metal-readonly.cmake"
      RESULT_VARIABLE _result)
    if(NOT _result EQUAL 0)
      message(FATAL_ERROR "MSL normalization failed for ${_entry}")
    endif()
  endif()
endforeach()
execute_process(COMMAND "${MERLIN_PYTHON}" -B "${MERLIN_SOURCE_DIR}/tests/metal_forward_abi_test.py"
  "${MERLIN_TEST_OUTPUT_DIR}" RESULT_VARIABLE _result)
if(NOT _result EQUAL 0)
  message(FATAL_ERROR "Native Metal Forward reflection ABI check failed")
endif()
