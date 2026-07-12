if(NOT DEFINED MERLIN_INPUT OR NOT DEFINED MERLIN_OUTPUT)
  message(FATAL_ERROR "MERLIN_INPUT and MERLIN_OUTPUT are required")
endif()

file(SHA256 "${MERLIN_INPUT}" _merlin_sha256)
get_filename_component(_merlin_input_name "${MERLIN_INPUT}" NAME)
file(WRITE "${MERLIN_OUTPUT}" "${_merlin_sha256}  ${_merlin_input_name}\n")
