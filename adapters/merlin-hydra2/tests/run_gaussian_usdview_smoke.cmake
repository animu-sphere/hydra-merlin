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
if(MERLIN_GAUSSIAN_REFERENCE_IMAGE)
  list(APPEND hgi_environment
    "MERLIN_GAUSSIAN_REFERENCE_IMAGE=${MERLIN_GAUSSIAN_REFERENCE_IMAGE}")
endif()
if(MERLIN_GAUSSIAN_GPU_MODE)
  list(APPEND hgi_environment
    "MERLIN_GAUSSIAN_USDVIEW_GPU_MODE=${MERLIN_GAUSSIAN_GPU_MODE}")
endif()
if(MERLIN_GAUSSIAN_GPU_RASTER)
  list(APPEND hgi_environment
    "MERLIN_GAUSSIAN_USDVIEW_GPU_RASTER=${MERLIN_GAUSSIAN_GPU_RASTER}")
endif()
set(gaussian_iterations 4)
set(trace_arguments)
if(MERLIN_HYDRA_REPORT_SCRIPT)
  # Enough camera-motion frames per phase for the report to compare host
  # presentation between the CPU-sorted reference and the GPU policy.
  list(APPEND hgi_environment "MERLIN_GAUSSIAN_USDVIEW_MOTION_FRAMES=40")
  set(host_trace "${MERLIN_STAGE_DIR}/gaussian-usdview-trace.json")
  set(trace_arguments --traceToFile "${host_trace}" --traceFormat chrome)
endif()
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
    ${trace_arguments}
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
if(MERLIN_GAUSSIAN_GPU_MODE)
  # The test script sends one name outside the vocabulary before selecting
  # the policy; Hydra must report that rejection rather than store it.
  if(NOT usdview_error MATCHES
       "code=renderer-settings.invalid-gpu-driven-gaussian-mode disposition=rejected source=merlin:gpuDrivenGaussian:mode")
    message(FATAL_ERROR
      "Invalid Gaussian render setting was not rejected explicitly:\n"
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
if(MERLIN_HYDRA_REPORT_SCRIPT)
  set(performance_report "${MERLIN_STAGE_DIR}/gaussian-hydra-performance.json")
  execute_process(
    COMMAND "${MERLIN_PYTHON}" "${MERLIN_HYDRA_REPORT_SCRIPT}"
            "${marker}" "${performance_report}" --host-trace "${host_trace}"
    RESULT_VARIABLE report_result
    OUTPUT_VARIABLE report_output
    ERROR_VARIABLE report_error
  )
  if(NOT report_result EQUAL 0 OR NOT EXISTS "${performance_report}")
    message(FATAL_ERROR
      "Gaussian Hydra performance report generation failed (${report_result}):\n"
      "${report_output}\n${report_error}")
  endif()
  # Prints the per-phase medians that separate host presentation (render
  # buffer resolve and map, texture upload, composition, present) from the
  # renderer's own GPU execution and readback.
  file(READ "${performance_report}" performance_json)
  string(JSON phase_count LENGTH "${performance_json}" phases)
  math(EXPR last_phase "${phase_count} - 1")
  set(reported_phases)
  foreach(index RANGE ${last_phase})
    string(JSON phase GET "${performance_json}" phases ${index} name)
    string(JSON samples GET "${performance_json}" phases ${index} samples)
    list(APPEND reported_phases "${phase}")
    set(summary "${phase} (${samples} frames):")
    foreach(stage IN ITEMS render_pass_execute gpu_execution readback
        render_buffer_resolve render_buffer_map host_upload host_composite
        presentation)
      string(JSON available GET "${performance_json}"
        phases ${index} stages ${stage} available)
      if(available)
        string(JSON kind GET "${performance_json}"
          phases ${index} stages ${stage} sample_kind)
        string(JSON median GET "${performance_json}"
          phases ${index} stages ${stage} summary_ns median)
        math(EXPR median_us "${median} / 1000")
        string(APPEND summary " ${stage}=${median_us}us")
        if(kind STREQUAL "trace_scope")
          string(APPEND summary "(trace)")
        endif()
      endif()
    endforeach()
    message(STATUS "${summary}")
  endforeach()
  foreach(phase IN ITEMS cpu-sorted-stream "gpu-${MERLIN_GAUSSIAN_GPU_RASTER}")
    if(NOT phase IN_LIST reported_phases)
      message(FATAL_ERROR
        "Gaussian Hydra performance report is missing the ${phase} phase")
    endif()
  endforeach()
endif()
