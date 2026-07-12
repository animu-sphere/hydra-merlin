if(NOT DEFINED MERLIN_BENCHMARK OR NOT DEFINED MERLIN_BENCHMARK_OUTPUT)
  message(FATAL_ERROR "benchmark test requires executable and output paths")
endif()

execute_process(
  COMMAND "${MERLIN_BENCHMARK}"
          --width 32
          --height 32
          --steady-frames 3
          --output "${MERLIN_BENCHMARK_OUTPUT}"
  RESULT_VARIABLE benchmark_result
  OUTPUT_VARIABLE benchmark_stdout
  ERROR_VARIABLE benchmark_stderr
)
if(NOT benchmark_result EQUAL 0)
  message(FATAL_ERROR
    "merlin-benchmark failed (${benchmark_result})\n"
    "stdout:\n${benchmark_stdout}\nstderr:\n${benchmark_stderr}")
endif()

file(READ "${MERLIN_BENCHMARK_OUTPUT}" json)
string(JSON schema GET "${json}" schema)
if(NOT schema STREQUAL "merlin-benchmark/v1")
  message(FATAL_ERROR "unexpected benchmark schema: ${schema}")
endif()

foreach(field commit build_type compiler os gpu vulkan_api)
  string(JSON value GET "${json}" environment "${field}")
  if(value STREQUAL "")
    message(FATAL_ERROR "environment.${field} is empty")
  endif()
endforeach()
string(JSON width GET "${json}" environment resolution width)
string(JSON height GET "${json}" environment resolution height)
if(NOT width EQUAL 32 OR NOT height EQUAL 32)
  message(FATAL_ERROR "benchmark resolution does not match requested extent")
endif()

string(JSON baseline_count LENGTH "${json}" baselines)
if(NOT baseline_count EQUAL 3)
  message(FATAL_ERROR "expected three baselines, got ${baseline_count}")
endif()

set(expected_names first-frame steady-state scene-edit)
foreach(index RANGE 0 2)
  list(GET expected_names ${index} expected_name)
  string(JSON actual_name GET "${json}" baselines ${index} name)
  if(NOT actual_name STREQUAL expected_name)
    message(FATAL_ERROR
      "baseline ${index} is ${actual_name}, expected ${expected_name}")
  endif()
  string(JSON draws GET "${json}" baselines ${index} counters draw_count)
  string(JSON triangles GET "${json}" baselines ${index} counters triangle_count)
  string(JSON readback GET "${json}" baselines ${index} counters readback_bytes)
  string(JSON total GET "${json}" baselines ${index} cpu_ns total_frame)
  if(NOT draws EQUAL 1 OR NOT triangles EQUAL 1)
    message(FATAL_ERROR "baseline ${actual_name} has invalid draw counters")
  endif()
  if(NOT readback EQUAL 8192)
    message(FATAL_ERROR "baseline ${actual_name} has invalid readback bytes")
  endif()
  if(NOT total GREATER 0)
    message(FATAL_ERROR "baseline ${actual_name} has no total-frame timing")
  endif()
endforeach()

string(JSON first_upload GET "${json}" baselines 0 counters upload_bytes)
string(JSON first_allocations GET "${json}" baselines 0 counters allocation_count)
string(JSON first_pipelines GET "${json}" baselines 0 counters pipeline_creation_count)
if(NOT first_upload GREATER 0 OR NOT first_allocations GREATER 0 OR
   NOT first_pipelines EQUAL 1)
  message(FATAL_ERROR "first-frame baseline did not record cold-start work")
endif()

foreach(field upload_bytes allocation_count pipeline_creation_count)
  string(JSON value GET "${json}" baselines 1 counters "${field}")
  if(NOT value EQUAL 0)
    message(FATAL_ERROR "steady-state ${field} must be zero, got ${value}")
  endif()
endforeach()
string(JSON scene_hits GET "${json}" baselines 1 counters scene_cache_hits)
string(JSON pipeline_hits GET "${json}" baselines 1 counters pipeline_cache_hits)
if(NOT scene_hits EQUAL 1 OR NOT pipeline_hits EQUAL 1)
  message(FATAL_ERROR "steady-state baseline did not hit backend caches")
endif()

string(JSON edit_upload GET "${json}" baselines 2 counters upload_bytes)
string(JSON edit_pipelines GET "${json}" baselines 2 counters pipeline_creation_count)
if(NOT edit_upload GREATER 0 OR NOT edit_pipelines EQUAL 0)
  message(FATAL_ERROR "scene-edit baseline has invalid structural work")
endif()
