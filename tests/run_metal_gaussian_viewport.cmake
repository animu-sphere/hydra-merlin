execute_process(COMMAND "${MERLIN_VIEWPORT}"
    --backend metal --usd "${MERLIN_SCENE}" --hidden --frames 8 --vsync off
    --validate --gaussian-gpu require --gaussian-raster tiled
    --benchmark "${MERLIN_REPORT}"
  RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr TIMEOUT 45)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Metal Gaussian viewport failed (${result}):\n${stdout}\n${stderr}")
endif()
file(READ "${MERLIN_REPORT}" report)
foreach(key IN ITEMS frames frames_presented gaussian_gpu_tiled_frames gaussian_float_color_frames)
  string(JSON value GET "${report}" ${key})
  if(NOT value EQUAL 8)
    message(FATAL_ERROR "Metal viewport ${key} expected 8, got ${value}")
  endif()
endforeach()
foreach(key IN ITEMS image_readback_bytes gaussian_gpu_overflow_frames
    gaussian_cpu_fallback_frames validation_messages)
  string(JSON value GET "${report}" ${key})
  if(NOT value EQUAL 0)
    message(FATAL_ERROR "Metal viewport ${key} expected 0, got ${value}")
  endif()
endforeach()
# Tiled control/counters remain readable even when no image AOV is downloaded.
string(JSON bytes GET "${report}" readback_bytes)
if(NOT bytes EQUAL 864)
  message(FATAL_ERROR "Metal viewport expected 8 * 108 telemetry bytes, got ${bytes}")
endif()
