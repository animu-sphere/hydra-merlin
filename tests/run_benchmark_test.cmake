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
if(NOT schema STREQUAL "merlin-benchmark/v3")
  message(FATAL_ERROR "unexpected benchmark schema: ${schema}")
endif()

foreach(field commit build_type compiler os gpu vulkan_api)
  string(JSON value GET "${json}" environment "${field}")
  if(value STREQUAL "")
    message(FATAL_ERROR "environment.${field} is empty")
  endif()
endforeach()
string(JSON width GET "${json}" fixture resolution width)
string(JSON height GET "${json}" fixture resolution height)
if(NOT width EQUAL 32 OR NOT height EQUAL 32)
  message(FATAL_ERROR "benchmark resolution does not match requested extent")
endif()

# The v3 reference fixture adds camera-only, topology, and AOV-combination
# gates to the shared-geometry edit sequence. Structural counters are
# deterministic; timing distributions are checked for shape, not thresholds.
set(expected_names
    first-frame steady-state camera-only edit-transform edit-visibility
    edit-material edit-points edit-topology aov-color-only aov-color-depth
    aov-all remove-mesh)
list(LENGTH expected_names expected_count)
string(JSON baseline_count LENGTH "${json}" baselines)
if(NOT baseline_count EQUAL expected_count)
  message(FATAL_ERROR
    "expected ${expected_count} baselines, got ${baseline_count}")
endif()

math(EXPR last_index "${expected_count} - 1")
foreach(index RANGE 0 ${last_index})
  list(GET expected_names ${index} expected_name)
  string(JSON actual_name GET "${json}" baselines ${index} name)
  if(NOT actual_name STREQUAL expected_name)
    message(FATAL_ERROR
      "baseline ${index} is ${actual_name}, expected ${expected_name}")
  endif()
  string(JSON total GET "${json}" baselines ${index}
         stages_ns total_frame median)
  string(JSON p95 GET "${json}" baselines ${index}
         stages_ns total_frame p95)
  string(JSON p99 GET "${json}" baselines ${index}
         stages_ns total_frame p99)
  string(JSON maximum GET "${json}" baselines ${index}
         stages_ns total_frame max)
  if(NOT total GREATER 0)
    message(FATAL_ERROR "baseline ${actual_name} has no total-frame timing")
  endif()
  if(p95 LESS total OR p99 LESS p95 OR maximum LESS p99)
    message(FATAL_ERROR
      "baseline ${actual_name} has invalid timing distribution")
  endif()
endforeach()

function(assert_counter index name expected)
  string(JSON value GET "${json}" baselines ${index} counters "${name}")
  if(NOT value EQUAL expected)
    string(JSON baseline_name GET "${json}" baselines ${index} name)
    message(FATAL_ERROR
      "${baseline_name}.${name} is ${value}, expected ${expected}")
  endif()
endfunction()

# first-frame: cold-start uploads both meshes once, suballocates four ranges,
# and creates the single pipeline.
assert_counter(0 draw_count 3)
assert_counter(0 visible_primitive_count 3)
assert_counter(0 triangle_count 4)
# Seven packed 48-byte vertices plus nine 32-bit indices.
assert_counter(0 upload_bytes 372)
assert_counter(0 buffer_suballocation_count 4)
assert_counter(0 geometry_cache_misses 2)
assert_counter(0 pipeline_creation_count 1)
string(JSON first_allocations GET "${json}" baselines 0 counters allocation_count)
if(NOT first_allocations GREATER 0)
  message(FATAL_ERROR "first-frame baseline did not record cold-start work")
endif()

# steady-state: a static scene performs zero upload, allocation, and pipeline
# work after warm-up.
assert_counter(1 draw_count 3)
assert_counter(1 upload_bytes 0)
assert_counter(1 allocation_count 0)
assert_counter(1 pipeline_creation_count 0)
assert_counter(1 scene_cache_hits 1)
assert_counter(1 geometry_cache_hits 2)
assert_counter(1 geometry_cache_misses 0)
assert_counter(1 pipeline_cache_hits 1)
assert_counter(1 wait_count 1)
assert_counter(1 resolve_count 1)
assert_counter(1 map_count 4)

# Camera-, transform-, visibility-, and material-only edits stage zero
# geometry bytes and miss no geometry caches.
foreach(index 2 3 4 5)
  assert_counter(${index} upload_bytes 0)
  assert_counter(${index} allocation_count 0)
  assert_counter(${index} geometry_cache_misses 0)
  assert_counter(${index} pipeline_creation_count 0)
endforeach()
assert_counter(2 draw_count 3)
assert_counter(3 draw_count 3)
assert_counter(4 draw_count 2)
assert_counter(5 draw_count 3)

# edit-points: only the edited mesh's vertex payload is re-uploaded, in place.
assert_counter(6 draw_count 3)
assert_counter(6 upload_bytes 144)
assert_counter(6 geometry_cache_misses 1)
assert_counter(6 geometry_cache_hits 1)
assert_counter(6 buffer_suballocation_count 0)
assert_counter(6 buffer_range_release_count 0)
assert_counter(6 allocation_count 0)

# edit-topology: only the edited mesh's three uint32 indices are staged.
assert_counter(7 draw_count 3)
assert_counter(7 upload_bytes 12)
assert_counter(7 geometry_cache_misses 1)
assert_counter(7 geometry_cache_hits 1)
assert_counter(7 buffer_suballocation_count 0)
assert_counter(7 allocation_count 0)

# AOV combinations prove color-only requests map and transfer no depth/ID
# products, while the full reference path remains four tightly-packed AOVs.
assert_counter(8 readback_bytes 4096)
assert_counter(8 cpu_readback_aov_count 1)
assert_counter(8 map_count 1)
assert_counter(9 readback_bytes 8192)
assert_counter(9 cpu_readback_aov_count 2)
assert_counter(9 map_count 2)
assert_counter(10 readback_bytes 16384)
assert_counter(10 cpu_readback_aov_count 4)
assert_counter(10 map_count 4)

# remove-mesh: removal stages nothing and releases exactly the removed mesh's
# vertex and index ranges.
assert_counter(11 draw_count 2)
assert_counter(11 upload_bytes 0)
assert_counter(11 buffer_range_release_count 2)
assert_counter(11 geometry_cache_hits 1)
assert_counter(11 geometry_cache_misses 0)

string(JSON timestamp_queries GET "${json}" environment timestamp_queries)
if(timestamp_queries)
  string(JSON gpu_ns GET "${json}" baselines 1
         stages_ns gpu_execution median)
  if(NOT gpu_ns GREATER 0)
    message(FATAL_ERROR "timestamp-capable GPU reported no execution time")
  endif()
endif()
