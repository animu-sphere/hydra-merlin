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
assert_counter(0 vertex_upload_bytes 336)
assert_counter(0 index_upload_bytes 36)
assert_counter(0 texture_upload_bytes 0)
# Each copy begins at a 16-byte-aligned ring offset.
assert_counter(0 upload_ring_reserved_bytes 384)
assert_counter(0 buffer_suballocation_count 4)
assert_counter(0 geometry_range_reuse_count 0)
assert_counter(0 geometry_arena_growth_count 2)
assert_counter(0 geometry_arena_growth_bytes 524288)
assert_counter(0 upload_ring_growth_count 1)
assert_counter(0 upload_ring_growth_bytes 262144)
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
assert_counter(1 vertex_upload_bytes 0)
assert_counter(1 index_upload_bytes 0)
assert_counter(1 texture_upload_bytes 0)
assert_counter(1 upload_ring_reserved_bytes 0)
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
assert_counter(6 vertex_upload_bytes 144)
assert_counter(6 index_upload_bytes 0)
assert_counter(6 upload_ring_reserved_bytes 144)
assert_counter(6 geometry_range_reuse_count 1)
assert_counter(6 geometry_cache_misses 1)
assert_counter(6 geometry_cache_hits 1)
assert_counter(6 buffer_suballocation_count 0)
assert_counter(6 buffer_range_release_count 0)
assert_counter(6 allocation_count 0)

# edit-topology: only the edited mesh's three uint32 indices are staged.
assert_counter(7 draw_count 3)
assert_counter(7 upload_bytes 12)
assert_counter(7 vertex_upload_bytes 0)
assert_counter(7 index_upload_bytes 12)
assert_counter(7 upload_ring_reserved_bytes 16)
assert_counter(7 geometry_range_reuse_count 1)
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

# Top-level residency exposes persistent arena fragmentation/retirement and
# mapped-ring growth independently of per-frame work.
foreach(arena IN ITEMS vertex_arena index_arena)
  string(JSON capacity GET "${json}" residency geometry ${arena}
         capacity_bytes)
  string(JSON resident GET "${json}" residency geometry ${arena}
         resident_bytes)
  string(JSON free GET "${json}" residency geometry ${arena} free_bytes)
  string(JSON largest_free GET "${json}" residency geometry ${arena}
         largest_free_span_bytes)
  string(JSON blocks GET "${json}" residency geometry ${arena} blocks)
  string(JSON growths GET "${json}" residency geometry ${arena} growths)
  math(EXPR accounted "${resident} + ${free}")
  if(NOT capacity EQUAL accounted OR largest_free GREATER free)
    message(FATAL_ERROR "${arena} residency byte accounting is inconsistent")
  endif()
  if(NOT blocks EQUAL 1 OR NOT growths EQUAL 1)
    message(FATAL_ERROR "${arena} did not retain its initial arena block")
  endif()
endforeach()
string(JSON pending_retirements GET "${json}" residency geometry
       pending_range_retirements)
string(JSON ring_capacity GET "${json}" residency upload_ring capacity_bytes)
string(JSON ring_peak GET "${json}" residency upload_ring
       peak_in_flight_bytes)
string(JSON ring_active GET "${json}" residency upload_ring active_regions)
string(JSON ring_growths GET "${json}" residency upload_ring growths)
string(JSON ring_reservations GET "${json}" residency upload_ring reservations)
if(NOT pending_retirements EQUAL 0 OR NOT ring_active EQUAL 0)
  message(FATAL_ERROR "resolved benchmark retained completion-protected work")
endif()
if(ring_peak GREATER ring_capacity OR NOT ring_growths EQUAL 1 OR
   NOT ring_reservations GREATER 0)
  message(FATAL_ERROR "upload-ring residency telemetry is inconsistent")
endif()

string(JSON heap_capacity GET "${json}" residency memory_budget
       heap_capacity_bytes)
string(JSON heap_budget GET "${json}" residency memory_budget
       heap_budget_bytes)
string(JSON renderer_current GET "${json}" residency memory_budget
       renderer_allocated_bytes)
string(JSON renderer_peak GET "${json}" residency memory_budget
       renderer_peak_allocated_bytes)
string(JSON memory_queries GET "${json}" residency memory_budget queries)
if(NOT heap_capacity GREATER 0 OR NOT heap_budget GREATER 0 OR
   NOT renderer_current GREATER 0 OR renderer_current GREATER renderer_peak OR
   NOT memory_queries GREATER 0)
  message(FATAL_ERROR "VRAM budget telemetry is inconsistent")
endif()

string(JSON async_transfer GET "${json}" residency transfer_queue asynchronous)
string(JSON graphics_family GET "${json}" residency transfer_queue
       graphics_family)
string(JSON transfer_family GET "${json}" residency transfer_queue
       transfer_family)
string(JSON transfer_submissions GET "${json}" residency transfer_queue
       submissions)
string(JSON transfer_bytes GET "${json}" residency transfer_queue
       uploaded_bytes)
string(JSON transfer_timeline GET "${json}" residency transfer_queue
       latest_timeline_value)
if(async_transfer)
  if(graphics_family EQUAL transfer_family OR
     NOT transfer_submissions GREATER 0 OR NOT transfer_bytes GREATER 0 OR
     NOT transfer_timeline EQUAL transfer_submissions)
    message(FATAL_ERROR "asynchronous transfer evidence is inconsistent")
  endif()
elseif(NOT graphics_family EQUAL transfer_family OR
       NOT transfer_submissions EQUAL 0 OR NOT transfer_timeline EQUAL 0)
  message(FATAL_ERROR "single-queue transfer fallback is inconsistent")
endif()

string(JSON timestamp_queries GET "${json}" environment timestamp_queries)
if(timestamp_queries)
  string(JSON gpu_ns GET "${json}" baselines 1
         stages_ns gpu_execution median)
  if(NOT gpu_ns GREATER 0)
    message(FATAL_ERROR "timestamp-capable GPU reported no execution time")
  endif()
endif()
