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
set(marker "${MERLIN_STAGE_DIR}/merlin-first-frame.txt")
cmake_path(CONVERT "${MERLIN_PXR_ROOT}/bin;${MERLIN_PXR_ROOT}/lib;$ENV{PATH}"
           TO_NATIVE_PATH_LIST runtime_path NORMALIZE)

execute_process(
  COMMAND "${MERLIN_CMAKE_COMMAND}" -E env
    "PXR_PLUGINPATH_NAME=${plugin_path}"
    "PYTHONPATH=${MERLIN_PXR_ROOT}/lib/python"
    "PATH=${runtime_path}"
    "MERLIN_HYDRA2_FIRST_FRAME_MARKER=${marker}"
    "MERLIN_HYDRA2_SMOKE_IMAGE=${image}"
    "${MERLIN_PYTHON}" "${MERLIN_TESTUSDVIEW}" "${scene}"
    --renderer Merlin --camera /Camera
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
if(NOT EXISTS "${image}")
  message(FATAL_ERROR
    "usdview started but did not produce a first-frame image:\n${usdview_output}\n${usdview_error}")
endif()
if(NOT EXISTS "${marker}")
  message(FATAL_ERROR
    "usdview created the delegate but Merlin did not complete a first frame:\n${usdview_output}\n${usdview_error}")
endif()
file(READ "${marker}" marker_contents)
if(NOT marker_contents MATCHES "draw_count=[1-9][0-9]*" OR
   NOT marker_contents MATCHES "buffers_written=2" OR
   NOT marker_contents MATCHES "covered_pixels=[1-9][0-9]*")
  message(FATAL_ERROR
    "Merlin first frame did not contain rendered geometry:\n${marker_contents}")
endif()
file(SIZE "${image}" image_size)
if(image_size LESS 100)
  message(FATAL_ERROR "usdview first-frame image is unexpectedly small")
endif()
