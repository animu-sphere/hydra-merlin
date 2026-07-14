file(REMOVE_RECURSE "${MERLIN_STAGE_DIR}")
execute_process(
  COMMAND "${MERLIN_CMAKE_COMMAND}" --install "${MERLIN_BUILD_DIR}"
          --config "${MERLIN_CONFIG}" --prefix "${MERLIN_STAGE_DIR}"
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error
)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR
    "Merlin install staging failed:\n${install_output}\n${install_error}")
endif()

set(plugin_path
  "${MERLIN_STAGE_DIR}/${MERLIN_INSTALL_LIBDIR}/usd/hdMerlin/resources")
set(scene
  "${MERLIN_STAGE_DIR}/${MERLIN_INSTALL_DATADIR}/merlin/tests/usdview-smoke.usda")
set(image "${MERLIN_STAGE_DIR}/usdview-first-frame.png")
set(marker "${MERLIN_STAGE_DIR}/merlin-regression.log")
set(host_trace "${MERLIN_STAGE_DIR}/merlin-usdview-trace.json")
cmake_path(CONVERT "${MERLIN_PXR_ROOT}/bin;${MERLIN_PXR_ROOT}/lib;$ENV{PATH}"
           TO_NATIVE_PATH_LIST runtime_path NORMALIZE)

execute_process(
  COMMAND "${MERLIN_CMAKE_COMMAND}" -E env
    "PXR_PLUGINPATH_NAME=${plugin_path}"
    "PYTHONPATH=${MERLIN_PXR_ROOT}/lib/python"
    "PATH=${runtime_path}"
    "MERLIN_HYDRA2_ENABLE_VALIDATION=1"
    "MERLIN_HYDRA2_REGRESSION_LOG=${marker}"
    "MERLIN_HYDRA2_SMOKE_IMAGE=${image}"
    "${MERLIN_PYTHON}" "${MERLIN_TESTUSDVIEW}" "${scene}"
    --renderer Merlin --camera /Camera
    --traceToFile "${host_trace}" --traceFormat chrome
    --testScript "${MERLIN_USDVIEW_TEST_SCRIPT}"
  RESULT_VARIABLE usdview_result
  OUTPUT_VARIABLE usdview_output
  ERROR_VARIABLE usdview_error
  TIMEOUT 50
)
if(NOT usdview_result EQUAL 0)
  message(FATAL_ERROR
    "usdview smoke failed (${usdview_result}):\n${usdview_output}\n${usdview_error}")
endif()
if(NOT EXISTS "${marker}")
  message(FATAL_ERROR
    "usdview created the delegate but Merlin did not complete the regression sequence:\n${usdview_output}\n${usdview_error}")
endif()
if(NOT EXISTS "${host_trace}")
  message(FATAL_ERROR
    "usdview did not produce the requested host Chrome trace")
endif()
file(READ "${marker}" marker_contents)
foreach(phase IN ITEMS
    baseline points topology transform visibility camera resize)
  if(NOT marker_contents MATCHES "phase=${phase} ")
    message(FATAL_ERROR
      "Merlin regression log is missing the ${phase} phase:\n${marker_contents}")
  endif()
  cmake_path(GET image STEM image_stem)
  cmake_path(GET image EXTENSION image_extension)
  cmake_path(GET image PARENT_PATH image_directory)
  set(phase_image
      "${image_directory}/${image_stem}-${phase}${image_extension}")
  if(NOT EXISTS "${phase_image}")
    message(FATAL_ERROR
      "usdview did not produce the ${phase} regression image")
  endif()
  file(SIZE "${phase_image}" image_size)
  if(image_size LESS 100)
    message(FATAL_ERROR
      "usdview ${phase} regression image is unexpectedly small")
  endif()
endforeach()

set(performance_report
    "${MERLIN_STAGE_DIR}/merlin-hydra-performance.json")
execute_process(
  COMMAND "${MERLIN_PYTHON}" "${MERLIN_HYDRA_REPORT_SCRIPT}"
          "${marker}" "${performance_report}" --host-trace "${host_trace}"
  RESULT_VARIABLE report_result
  OUTPUT_VARIABLE report_output
  ERROR_VARIABLE report_error
)
if(NOT report_result EQUAL 0 OR NOT EXISTS "${performance_report}")
  message(FATAL_ERROR
    "Hydra performance report generation failed (${report_result}):\n"
    "${report_output}\n${report_error}")
endif()
file(READ "${performance_report}" performance_json)
string(JSON performance_schema GET "${performance_json}" schema)
if(NOT performance_schema STREQUAL "merlin-hydra-performance/v1")
  message(FATAL_ERROR
    "Unexpected Hydra performance schema: ${performance_schema}")
endif()
