# Opt-in install-tree host evidence. Requires a GUI-capable OpenUSD runtime.
foreach(required MERLIN_BUILD_DIR MERLIN_STAGE_DIR MERLIN_PXR_ROOT
    MERLIN_PYTHON MERLIN_TESTUSDVIEW MERLIN_GAUSSIAN_SAMPLE)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "requires ${required}")
  endif()
endforeach()
foreach(path MERLIN_BUILD_DIR MERLIN_STAGE_DIR MERLIN_PXR_ROOT MERLIN_PYTHON
    MERLIN_TESTUSDVIEW MERLIN_GAUSSIAN_SAMPLE)
  get_filename_component(${path} "${${path}}" ABSOLUTE)
endforeach()
if(NOT DEFINED MERLIN_GAUSSIAN_FRAMES)
  set(MERLIN_GAUSSIAN_FRAMES 40)
endif()
if(NOT DEFINED MERLIN_CONFIG OR "${MERLIN_CONFIG}" STREQUAL "")
  set(MERLIN_CONFIG Release)
endif()
if(NOT DEFINED MERLIN_GAUSSIAN_VALIDATE)
  set(MERLIN_GAUSSIAN_VALIDATE 0)
endif()
if(MERLIN_GAUSSIAN_VALIDATE)
  set(validation 1)
else()
  set(validation 0)
endif()
get_filename_component(source "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(MAKE_DIRECTORY "${MERLIN_STAGE_DIR}")
file(SHA256 "${MERLIN_GAUSSIAN_SAMPLE}" scene_sha256)
file(SHA256 "${MERLIN_PXR_ROOT}/pxrConfig.cmake" runtime_config_sha256)
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${MERLIN_BUILD_DIR}"
  --config "${MERLIN_CONFIG}" --prefix "${MERLIN_STAGE_DIR}"
  RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "install failed: ${stdout}\n${stderr}")
endif()
file(GLOB renderer_libraries "${MERLIN_STAGE_DIR}/lib/usd/hdMerlin/hdMerlin.dll"
  "${MERLIN_STAGE_DIR}/lib/usd/hdMerlin/libhdMerlin.so"
  "${MERLIN_STAGE_DIR}/lib/usd/hdMerlin/libhdMerlin.dylib")
list(LENGTH renderer_libraries renderer_library_count)
if(NOT renderer_library_count EQUAL 1)
  message(FATAL_ERROR "cannot identify the installed hdMerlin library")
endif()
list(GET renderer_libraries 0 renderer_library)
file(SHA256 "${renderer_library}" renderer_sha256)
file(WRITE "${MERLIN_STAGE_DIR}/capture-provenance.json"
  "{\n  \"schema\": \"merlin-gaussian-host-capture/v1\",\n"
  "  \"scene_sha256\": \"${scene_sha256}\",\n"
  "  \"runtime_config_sha256\": \"${runtime_config_sha256}\",\n"
  "  \"renderer_sha256\": \"${renderer_sha256}\",\n"
  "  \"validation_enabled\": ${validation},\n"
  "  \"measured_frames\": ${MERLIN_GAUSSIAN_FRAMES}\n}\n")
cmake_path(CONVERT "${MERLIN_PXR_ROOT}/bin;${MERLIN_PXR_ROOT}/lib;$ENV{PATH}"
  TO_NATIVE_PATH_LIST runtime_path NORMALIZE)
set(marker "${MERLIN_STAGE_DIR}/gaussian-regression.log")
set(trace "${MERLIN_STAGE_DIR}/gaussian-usdview-trace.json")
set(host_environment)
if(MERLIN_FORCE_HGI_VULKAN)
  list(APPEND host_environment "HGI_ENABLE_VULKAN=1" "HGIVULKAN_DEBUG=${validation}"
    "MERLIN_GAUSSIAN_EXPECT_TRANSFER_MODE=gpu-copy")
else()
  list(APPEND host_environment "HGI_ENABLE_VULKAN=0" "HGIVULKAN_DEBUG=0"
    "MERLIN_GAUSSIAN_EXPECT_TRANSFER_MODE=cpu-readback")
endif()
file(REMOVE "${marker}" "${trace}" "${MERLIN_STAGE_DIR}/gaussian-host-verification.json")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env
  ${host_environment}
  "PXR_PLUGINPATH_NAME=${MERLIN_STAGE_DIR}/lib/usd/hdMerlin/resources"
  "PYTHONPATH=${MERLIN_PXR_ROOT}/lib/python" "PATH=${runtime_path}"
  "MERLIN_HYDRA2_ENABLE_VALIDATION=${validation}"
  "PYTHONDONTWRITEBYTECODE=1"
  "MERLIN_HYDRA2_TEST_BACKEND=vulkan"
  "MERLIN_HYDRA2_REGRESSION_LOG=${marker}"
  "MERLIN_HYDRA2_SMOKE_IMAGE=${MERLIN_STAGE_DIR}/gaussian.png"
  "MERLIN_GAUSSIAN_BENCHMARK_FRAMES=${MERLIN_GAUSSIAN_FRAMES}"
  "${MERLIN_PYTHON}" "${MERLIN_TESTUSDVIEW}" "${MERLIN_GAUSSIAN_SAMPLE}"
  --renderer Merlin --traceToFile "${trace}" --traceFormat chrome
  --testScript "${source}/adapters/merlin-hydra2/tests/gaussian_usdview_benchmark.py"
  WORKING_DIRECTORY "${MERLIN_STAGE_DIR}"
  RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr TIMEOUT 600)
file(WRITE "${MERLIN_STAGE_DIR}/usdview.log" "${stdout}\n${stderr}")
if(NOT result EQUAL 0 OR NOT EXISTS "${MERLIN_STAGE_DIR}/gaussian-host-verification.json")
  message(FATAL_ERROR "Gaussian host comparison failed (${result}): ${stdout}\n${stderr}")
endif()
execute_process(COMMAND "${MERLIN_PYTHON}" "${source}/scripts/hydra-performance-report.py"
  "${marker}" "${MERLIN_STAGE_DIR}/gaussian-hydra-performance.json" --host-trace "${trace}"
  RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "host report failed: ${stdout}\n${stderr}")
endif()
message(STATUS "Gaussian host comparison passed: ${MERLIN_STAGE_DIR}")
