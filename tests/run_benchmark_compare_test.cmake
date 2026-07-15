foreach(variable IN ITEMS
    MERLIN_PYTHON MERLIN_COMPARE_SCRIPT MERLIN_BENCHMARK_OUTPUT
    MERLIN_COMPARISON_OUTPUT)
  if(NOT DEFINED ${variable})
    message(FATAL_ERROR "${variable} is required")
  endif()
endforeach()

execute_process(
  COMMAND "${MERLIN_PYTHON}" "${MERLIN_COMPARE_SCRIPT}"
          "${MERLIN_BENCHMARK_OUTPUT}" "${MERLIN_BENCHMARK_OUTPUT}"
          --output "${MERLIN_COMPARISON_OUTPUT}"
  RESULT_VARIABLE self_result
  OUTPUT_VARIABLE self_output
  ERROR_VARIABLE self_error
)
if(NOT self_result EQUAL 0)
  message(FATAL_ERROR
    "self-comparison failed (${self_result}):\n${self_output}\n${self_error}")
endif()
file(READ "${MERLIN_COMPARISON_OUTPUT}" comparison)
string(JSON status GET "${comparison}" status)
if(NOT status STREQUAL "pass")
  message(FATAL_ERROR "self-comparison status is ${status}")
endif()

file(READ "${MERLIN_BENCHMARK_OUTPUT}" benchmark)

# Additive v3 counters must not make a valid report from the previous release
# look like a structural regression merely because that baseline predates the
# fields.
set(legacy_benchmark "${benchmark}")
string(JSON baseline_count LENGTH "${legacy_benchmark}" baselines)
math(EXPR last_baseline "${baseline_count} - 1")
foreach(index RANGE 0 ${last_baseline})
  foreach(counter IN ITEMS
      snapshot_visited_records snapshot_copied_records
      snapshot_rebuilt_draws snapshot_fully_rebuilt_tables
      bindless_sampled_image_descriptor_update_count
      bindless_sampler_descriptor_update_count)
    string(JSON legacy_benchmark REMOVE "${legacy_benchmark}"
           baselines ${index} counters "${counter}")
  endforeach()
endforeach()
set(legacy_input "${MERLIN_COMPARISON_OUTPUT}.legacy-input.json")
set(legacy_output "${MERLIN_COMPARISON_OUTPUT}.legacy.json")
file(WRITE "${legacy_input}" "${legacy_benchmark}\n")
execute_process(
  COMMAND "${MERLIN_PYTHON}" "${MERLIN_COMPARE_SCRIPT}"
          "${legacy_input}" "${MERLIN_BENCHMARK_OUTPUT}"
          --output "${legacy_output}"
  RESULT_VARIABLE legacy_result
  OUTPUT_VARIABLE legacy_stdout
  ERROR_VARIABLE legacy_error
)
if(NOT legacy_result EQUAL 0)
  message(FATAL_ERROR
    "legacy v3 baseline caused a false regression (${legacy_result}):\n"
    "${legacy_stdout}\n${legacy_error}")
endif()
file(READ "${legacy_output}" legacy_comparison)
string(JSON legacy_status GET "${legacy_comparison}" status)
if(NOT legacy_status STREQUAL "pass")
  message(FATAL_ERROR "legacy v3 comparison status is ${legacy_status}")
endif()

# Device image-memory requirements are implementation-dependent and must not
# turn the default structural comparison into a cross-GPU false regression.
string(JSON old_image_bytes GET "${benchmark}"
       baselines 0 counters image_allocation_bytes)
math(EXPR new_image_bytes "${old_image_bytes} + 1")
string(JSON portable_mutation SET "${benchmark}"
       baselines 0 counters image_allocation_bytes ${new_image_bytes})
set(portable_input "${MERLIN_COMPARISON_OUTPUT}.portable-input.json")
set(portable_output "${MERLIN_COMPARISON_OUTPUT}.portable.json")
file(WRITE "${portable_input}" "${portable_mutation}\n")
execute_process(
  COMMAND "${MERLIN_PYTHON}" "${MERLIN_COMPARE_SCRIPT}"
          "${MERLIN_BENCHMARK_OUTPUT}" "${portable_input}"
          --output "${portable_output}"
  RESULT_VARIABLE portable_result
  OUTPUT_VARIABLE portable_stdout
  ERROR_VARIABLE portable_error
)
if(NOT portable_result EQUAL 0)
  message(FATAL_ERROR
    "device-dependent image bytes caused a structural regression "
    "(${portable_result}):\n${portable_stdout}\n${portable_error}")
endif()

string(JSON structural_mutation SET "${benchmark}"
       baselines 1 counters upload_bytes 1)
set(structural_input "${MERLIN_COMPARISON_OUTPUT}.structural-input.json")
set(structural_output "${MERLIN_COMPARISON_OUTPUT}.structural.json")
file(WRITE "${structural_input}" "${structural_mutation}\n")
execute_process(
  COMMAND "${MERLIN_PYTHON}" "${MERLIN_COMPARE_SCRIPT}"
          "${MERLIN_BENCHMARK_OUTPUT}" "${structural_input}"
          --output "${structural_output}"
  RESULT_VARIABLE structural_result
  OUTPUT_VARIABLE structural_stdout
  ERROR_VARIABLE structural_error
)
if(NOT structural_result EQUAL 2)
  message(FATAL_ERROR
    "structural regression returned ${structural_result}, expected 2:\n"
    "${structural_stdout}\n${structural_error}")
endif()
file(READ "${structural_output}" structural_comparison)
string(JSON structural_kind GET "${structural_comparison}"
       regressions 0 kind)
if(NOT structural_kind STREQUAL "structural")
  message(FATAL_ERROR
    "expected structural regression, got ${structural_kind}")
endif()

string(JSON old_median GET "${benchmark}"
       baselines 1 stages_ns command_recording median)
math(EXPR new_median "${old_median} * 2 + 1")
string(JSON timing_mutation SET "${benchmark}"
       baselines 1 stages_ns command_recording median ${new_median})
set(timing_input "${MERLIN_COMPARISON_OUTPUT}.timing-input.json")
set(timing_output "${MERLIN_COMPARISON_OUTPUT}.timing.json")
file(WRITE "${timing_input}" "${timing_mutation}\n")
execute_process(
  COMMAND "${MERLIN_PYTHON}" "${MERLIN_COMPARE_SCRIPT}"
          "${MERLIN_BENCHMARK_OUTPUT}" "${timing_input}"
          --timing-threshold-percent 10 --output "${timing_output}"
  RESULT_VARIABLE timing_result
  OUTPUT_VARIABLE timing_stdout
  ERROR_VARIABLE timing_error
)
if(NOT timing_result EQUAL 2)
  message(FATAL_ERROR
    "timing regression returned ${timing_result}, expected 2:\n"
    "${timing_stdout}\n${timing_error}")
endif()
file(READ "${timing_output}" timing_comparison)
string(JSON timing_kind GET "${timing_comparison}" regressions 0 kind)
if(NOT timing_kind STREQUAL "timing")
  message(FATAL_ERROR "expected timing regression, got ${timing_kind}")
endif()
