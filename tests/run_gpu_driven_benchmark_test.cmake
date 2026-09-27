# Opt-in hardware validation: these fixtures require bindless GPU Scene and
# indirect-count support. They deliberately fail on an unsupported device.
if(NOT DEFINED MERLIN_BENCHMARK OR NOT DEFINED MERLIN_BENCHMARK_OUTPUT_DIR)
  message(FATAL_ERROR "requires benchmark executable and output directory")
endif()
file(MAKE_DIRECTORY "${MERLIN_BENCHMARK_OUTPUT_DIR}")
foreach(fixture gpu-driven-small-objects gpu-driven-diverse-objects)
  set(output "${MERLIN_BENCHMARK_OUTPUT_DIR}/${fixture}.json")
  execute_process(
    COMMAND "${MERLIN_BENCHMARK}" --fixture "${fixture}"
            --width 256 --height 256 --steady-frames 3 --output "${output}"
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
    TIMEOUT 120
  )
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "${fixture} failed (${result})\n${stdout}\n${stderr}")
  endif()
  file(READ "${output}" json)
  string(JSON schema GET "${json}" schema)
  string(JSON name GET "${json}" fixture name)
  string(JSON meshes GET "${json}" fixture mesh_count)
  string(JSON instances GET "${json}" fixture instance_count)
  string(JSON triangles GET "${json}" fixture triangle_count)
  string(JSON count LENGTH "${json}" baselines)
  if(fixture STREQUAL "gpu-driven-diverse-objects")
    set(expected_meshes 16)
    set(expected_triangles 150000)
    set(minimum_batches 2)
    set(maximum_batches 128)
  else()
    set(expected_meshes 1)
    set(expected_triangles 100000)
    set(minimum_batches 1)
    set(maximum_batches 1)
  endif()
  if(NOT schema STREQUAL "merlin-benchmark/v3" OR NOT name STREQUAL fixture
      OR NOT meshes EQUAL expected_meshes OR NOT instances EQUAL 100000
      OR NOT triangles EQUAL expected_triangles OR NOT count EQUAL 12)
    message(FATAL_ERROR "${fixture}: incorrect report schema or scene totals")
  endif()
  set(index 0)
  foreach(draws 1000 10000 100000)
    foreach(phase update conventional gpu-driven camera-motion-gpu-driven)
      string(JSON name GET "${json}" baselines ${index} name)
      if(NOT name STREQUAL "${phase}-${draws}")
        message(FATAL_ERROR "unexpected baseline ${name} at ${index}")
      endif()
      if(phase STREQUAL "gpu-driven" OR phase STREQUAL "camera-motion-gpu-driven")
        foreach(counter upload_bytes allocation_count descriptor_update_count
            pipeline_creation_count mesh_cpu_draw_visit_count
            gpu_driven_candidate_upload_bytes gpu_driven_fallback_count)
          string(JSON value GET "${json}" baselines ${index} counters ${counter})
          if(NOT value EQUAL 0)
            message(FATAL_ERROR "${name}: ${counter} must be zero, got ${value}")
          endif()
        endforeach()
        string(JSON candidates GET "${json}" baselines ${index} counters
               gpu_driven_candidate_draw_count)
        string(JSON visible GET "${json}" baselines ${index} counters
               gpu_driven_visible_draw_count)
        string(JSON batches GET "${json}" baselines ${index} counters
               gpu_driven_indirect_draw_count)
        string(JSON samples GET "${json}" baselines ${index} samples)
        if(NOT candidates EQUAL draws OR NOT visible EQUAL draws
            OR batches LESS minimum_batches OR batches GREATER maximum_batches
            OR NOT samples EQUAL 3)
          message(FATAL_ERROR "${name}: incorrect GPU work or sample counts")
        endif()
      endif()
      math(EXPR index "${index} + 1")
    endforeach()
  endforeach()
  message(STATUS "${fixture}: image parity and structural/schema checks passed")
endforeach()
