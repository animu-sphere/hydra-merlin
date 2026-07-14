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
