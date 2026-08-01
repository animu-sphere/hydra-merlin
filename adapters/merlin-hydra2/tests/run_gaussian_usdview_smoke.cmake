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
    "Merlin Gaussian install staging failed:\n${install_output}\n${install_error}")
endif()

set(plugin_path
  "${MERLIN_STAGE_DIR}/${MERLIN_INSTALL_LIBDIR}/usd/hdMerlin/resources")
set(image "${MERLIN_STAGE_DIR}/gaussian-first-frame.png")
set(marker "${MERLIN_STAGE_DIR}/gaussian-regression.log")
cmake_path(CONVERT "${MERLIN_PXR_ROOT}/bin;${MERLIN_PXR_ROOT}/lib;$ENV{PATH}"
           TO_NATIVE_PATH_LIST runtime_path NORMALIZE)
set(hgi_environment)
set(gaussian_iterations 4)
if(MERLIN_FORCE_HGI_VULKAN)
  list(APPEND hgi_environment
    "HGI_ENABLE_VULKAN=1"
    "HGIVULKAN_DEBUG=1")
  # One normal event iteration followed by waitForConvergence must be enough:
  # the RenderBuffer itself is responsible for requesting another frame while
  # the first asynchronous GPU copy has not produced displayable contents.
  set(gaussian_iterations 1)
endif()
execute_process(
  COMMAND "${MERLIN_CMAKE_COMMAND}" -E env
    ${hgi_environment}
    "PXR_PLUGINPATH_NAME=${plugin_path}"
    "PYTHONPATH=${MERLIN_PXR_ROOT}/lib/python"
    "PATH=${runtime_path}"
    "MERLIN_HYDRA2_ENABLE_VALIDATION=1"
    "MERLIN_HYDRA2_TEST_BACKEND=vulkan"
    "MERLIN_HYDRA2_REGRESSION_LOG=${marker}"
    "MERLIN_HYDRA2_SMOKE_IMAGE=${image}"
    "MERLIN_GAUSSIAN_USDVIEW_ITERATIONS=${gaussian_iterations}"
    "${MERLIN_PYTHON}" "${MERLIN_TESTUSDVIEW}" "${MERLIN_GAUSSIAN_SAMPLE}"
    --renderer Merlin
    --testScript "${MERLIN_GAUSSIAN_USDVIEW_TEST_SCRIPT}"
  RESULT_VARIABLE usdview_result
  OUTPUT_VARIABLE usdview_output
  ERROR_VARIABLE usdview_error
  TIMEOUT 50
)
if(NOT usdview_result EQUAL 0)
  message(FATAL_ERROR
    "Gaussian usdview smoke failed (${usdview_result}):\n"
    "${usdview_output}\n${usdview_error}")
endif()
if(MERLIN_GAUSSIAN_EXPECT_POLICY_FALLBACK)
  if(NOT usdview_error MATCHES
       "hydra.gaussian.projection-hint-unavailable" OR
     NOT usdview_error MATCHES
       "hydra.gaussian.sorting-hint-unavailable")
    message(FATAL_ERROR
      "Gaussian policy fallback diagnostics were not reported:\n"
      "${usdview_output}\n${usdview_error}")
  endif()
endif()
if(NOT EXISTS "${marker}" OR NOT EXISTS "${image}")
  message(FATAL_ERROR
    "Gaussian usdview smoke produced incomplete evidence:\n"
    "${usdview_output}\n${usdview_error}")
endif()
file(READ "${marker}" marker_contents)
if(NOT marker_contents MATCHES "gaussian_resources=1")
  message(FATAL_ERROR
    "Gaussian particleField did not reach the renderer snapshot:\n"
    "${marker_contents}")
endif()
if(MERLIN_FORCE_HGI_VULKAN)
  if(NOT marker_contents MATCHES "hgi_transfer_mode=gpu-copy" OR
     NOT marker_contents MATCHES
       "hgi_gpu_copy_completion_count=[1-9][0-9]*" OR
     NOT marker_contents MATCHES "hgi_coarse_wait_count=0" OR
     marker_contents MATCHES "hgi_coarse_wait_count=[1-9][0-9]*")
    message(FATAL_ERROR
      "Gaussian HgiVulkan presentation did not converge through asynchronous GPU copy:\n"
      "${marker_contents}")
  endif()
endif()
