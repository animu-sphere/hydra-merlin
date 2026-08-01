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

set(hgi_environment)
if(MERLIN_FORCE_HGI_VULKAN)
  list(APPEND hgi_environment
    "HGI_ENABLE_VULKAN=1"
    "HGIVULKAN_DEBUG=1")
endif()

execute_process(
  COMMAND "${MERLIN_CMAKE_COMMAND}" -E env
    ${hgi_environment}
    "PXR_PLUGINPATH_NAME=${plugin_path}"
    "PYTHONPATH=${MERLIN_PXR_ROOT}/lib/python"
    "PATH=${runtime_path}"
    "MERLIN_HYDRA2_ENABLE_VALIDATION=1"
    "MERLIN_HYDRA2_TEST_BACKEND=${MERLIN_HYDRA2_TEST_BACKEND}"
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
set(hgi_metal_gpu_copy FALSE)
if(marker_contents MATCHES "hgi_metal_transfer_mode=gpu-copy")
  set(hgi_metal_gpu_copy TRUE)
endif()
foreach(phase IN ITEMS
    baseline points topology primvar transform visibility camera
    material_parameter diagnostic recovery remove readd resize)
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

# Version-4 logs produced before the snapshot counters were added remain valid.
# The current report generator must treat those additive fields as zero rather
# than rejecting historical evidence with a missing-key error.
file(READ "${marker}" legacy_log)
foreach(counter IN ITEMS
    snapshot_visited_records snapshot_copied_records
    snapshot_rebuilt_draws snapshot_fully_rebuilt_tables)
  string(REGEX REPLACE " ${counter}=[0-9]+" "" legacy_log "${legacy_log}")
endforeach()
set(legacy_marker "${MERLIN_STAGE_DIR}/merlin-regression-v4-legacy.log")
set(legacy_performance_report
    "${MERLIN_STAGE_DIR}/merlin-hydra-performance-v4-legacy.json")
file(WRITE "${legacy_marker}" "${legacy_log}")
execute_process(
  COMMAND "${MERLIN_PYTHON}" "${MERLIN_HYDRA_REPORT_SCRIPT}"
          "${legacy_marker}" "${legacy_performance_report}"
  RESULT_VARIABLE legacy_report_result
  OUTPUT_VARIABLE legacy_report_output
  ERROR_VARIABLE legacy_report_error
)
if(NOT legacy_report_result EQUAL 0 OR
   NOT EXISTS "${legacy_performance_report}")
  message(FATAL_ERROR
    "Legacy Hydra performance report generation failed "
    "(${legacy_report_result}):\n"
    "${legacy_report_output}\n${legacy_report_error}")
endif()
file(READ "${performance_report}" performance_json)
string(JSON performance_schema GET "${performance_json}" schema)
if(NOT performance_schema STREQUAL "merlin-hydra-performance/v1")
  message(FATAL_ERROR
    "Unexpected Hydra performance schema: ${performance_schema}")
endif()

# RenderBuffer resolve/map, CPU-to-Hgi upload, and GPU copy require current-frame
# host trace evidence rather than lagged delegate telemetry. A Vulkan Hgi
# consumes the bridge-owned texture directly, so unlike the GL fallback it must
# not Map the color RenderBuffer.
string(JSON performance_phase_count LENGTH "${performance_json}" phases)
math(EXPR performance_last_phase "${performance_phase_count} - 1")
set(baseline_phase_index "")
foreach(index RANGE 0 ${performance_last_phase})
  string(JSON phase_name GET "${performance_json}" phases ${index} name)
  if(phase_name STREQUAL "baseline")
    set(baseline_phase_index ${index})
    break()
  endif()
endforeach()
if(baseline_phase_index STREQUAL "")
  message(FATAL_ERROR "Hydra performance report has no baseline phase")
endif()
set(required_trace_stages render_buffer_resolve)
if(MERLIN_FORCE_HGI_VULKAN)
  list(APPEND required_trace_stages gpu_copy)
elseif(NOT hgi_metal_gpu_copy)
  list(APPEND required_trace_stages host_upload)
endif()
foreach(stage IN LISTS required_trace_stages)
  string(JSON sample_kind GET "${performance_json}" phases
         ${baseline_phase_index} stages ${stage} sample_kind)
  string(JSON available GET "${performance_json}" phases
         ${baseline_phase_index} stages ${stage} available)
  if(NOT sample_kind STREQUAL "trace_scope" OR NOT available)
    message(FATAL_ERROR
      "Hydra ${stage} evidence is not sourced from the host trace")
  endif()
endforeach()

string(JSON map_sample_kind GET "${performance_json}" phases
       ${baseline_phase_index} stages render_buffer_map sample_kind)
string(JSON map_available GET "${performance_json}" phases
       ${baseline_phase_index} stages render_buffer_map available)
file(READ "${host_trace}" host_trace_json)
if(MERLIN_FORCE_HGI_VULKAN)
  if(NOT host_trace_json MATCHES "HgiVulkan::_SubmitCmds")
    message(FATAL_ERROR
      "Forced Vulkan Hgi smoke did not execute the HgiVulkan driver")
  endif()
  if(map_available)
    message(FATAL_ERROR
      "Vulkan Hgi target path unexpectedly mapped the color RenderBuffer")
  endif()
  if(NOT marker_contents MATCHES
     "cpu_readback_aov_count=3 .*aov_image_export_count=1")
    message(FATAL_ERROR
      "Vulkan Hgi GPU copy did not replace exactly one CPU AOV readback")
  endif()
  if(NOT marker_contents MATCHES
     "hgi_gpu_copy_completion_count=[1-9][0-9]*")
    message(FATAL_ERROR
      "Vulkan Hgi GPU copy did not report host-consumption completion")
  endif()
  if(NOT marker_contents MATCHES "hgi_transfer_mode=gpu-copy" OR
     NOT marker_contents MATCHES
       "hgi_direct_share_rejection=public-texture-import-unavailable" OR
     NOT marker_contents MATCHES
       "hgi_direct_share_evaluation_count=[1-9][0-9]*" OR
     NOT marker_contents MATCHES
       "hgi_direct_share_rejection_count=[1-9][0-9]*")
    message(FATAL_ERROR
      "Vulkan Hgi did not report the v0.13.1 direct-share rejection")
  endif()
  if(NOT marker_contents MATCHES "hgi_coarse_wait_count=0" OR
     marker_contents MATCHES "hgi_coarse_wait_count=[1-9][0-9]*")
    message(FATAL_ERROR
      "Vulkan Hgi GPU copy did not report its coarse-wait counter")
  endif()
  foreach(index RANGE 0 ${performance_last_phase})
    string(JSON phase_name GET "${performance_json}" phases ${index} name)
    if(phase_name STREQUAL "unlabeled")
      continue()
    endif()
    foreach(stage IN ITEMS gpu_copy)
      string(JSON available GET "${performance_json}" phases ${index}
             stages ${stage} available)
      if(NOT available)
        message(FATAL_ERROR
          "Vulkan Hgi ${phase_name} phase has no ${stage} evidence")
      endif()
    endforeach()
    foreach(stage IN ITEMS render_buffer_map host_upload)
      string(JSON available GET "${performance_json}" phases ${index}
             stages ${stage} available)
      if(available)
        message(FATAL_ERROR
          "Vulkan Hgi ${phase_name} phase unexpectedly used ${stage}")
      endif()
    endforeach()
  endforeach()
elseif(hgi_metal_gpu_copy)
  if(map_available)
    message(FATAL_ERROR
      "HgiMetal target path unexpectedly mapped the color RenderBuffer")
  endif()
  if(NOT marker_contents MATCHES
     "hgi_metal_gpu_copy_completion_count=[1-9][0-9]*" OR
     NOT marker_contents MATCHES "hgi_metal_transfer_mode=gpu-copy" OR
     NOT marker_contents MATCHES
       "hgi_metal_direct_share_rejection=completion-retention-unavailable" OR
     NOT marker_contents MATCHES
       "hgi_metal_direct_share_evaluation_count=[1-9][0-9]*" OR
     NOT marker_contents MATCHES
       "hgi_metal_direct_share_rejection_count=[1-9][0-9]*" OR
     NOT marker_contents MATCHES "hgi_metal_coarse_wait_count=0")
    message(FATAL_ERROR
      "HgiMetal GPU-copy path did not report the expected bridge telemetry")
  endif()
  foreach(index RANGE 0 ${performance_last_phase})
    string(JSON phase_name GET "${performance_json}" phases ${index} name)
    if(phase_name STREQUAL "unlabeled")
      continue()
    endif()
    foreach(stage IN ITEMS render_buffer_map host_upload)
      string(JSON available GET "${performance_json}" phases ${index}
             stages ${stage} available)
      if(available)
        message(FATAL_ERROR
          "HgiMetal ${phase_name} phase unexpectedly used ${stage}")
      endif()
    endforeach()
  endforeach()
else()
  if(NOT map_sample_kind STREQUAL "trace_scope" OR NOT map_available)
    message(FATAL_ERROR
      "Hydra render_buffer_map evidence is not sourced from the host trace")
  endif()
endif()
