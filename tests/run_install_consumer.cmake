if(NOT DEFINED MERLIN_CMAKE_COMMAND OR
   NOT DEFINED MERLIN_BUILD_DIR OR
   NOT DEFINED MERLIN_TEST_BINARY_DIR OR
   NOT DEFINED MERLIN_INSTALL_LIBDIR OR
   NOT DEFINED MERLIN_GENERATOR)
  message(FATAL_ERROR "Missing Merlin install-consumer test arguments")
endif()

set(_stage_dir "${MERLIN_TEST_BINARY_DIR}/install-consumer-prefix")
set(_consumer_build_dir "${MERLIN_TEST_BINARY_DIR}/install-consumer-build")

set(_config_args)
if(NOT "${MERLIN_CONFIG}" STREQUAL "")
  list(APPEND _config_args --config "${MERLIN_CONFIG}")
endif()

execute_process(
  COMMAND "${MERLIN_CMAKE_COMMAND}" --install "${MERLIN_BUILD_DIR}"
          --prefix "${_stage_dir}" ${_config_args}
  RESULT_VARIABLE _install_result
)
if(NOT _install_result EQUAL 0)
  message(FATAL_ERROR "Merlin install step failed: ${_install_result}")
endif()

set(_generator_args -G "${MERLIN_GENERATOR}")
if(NOT "${MERLIN_GENERATOR_PLATFORM}" STREQUAL "")
  list(APPEND _generator_args -A "${MERLIN_GENERATOR_PLATFORM}")
endif()
if(NOT "${MERLIN_GENERATOR_TOOLSET}" STREQUAL "")
  list(APPEND _generator_args -T "${MERLIN_GENERATOR_TOOLSET}")
endif()

execute_process(
  COMMAND "${MERLIN_CMAKE_COMMAND}"
          -S "${MERLIN_SOURCE_DIR}/tests/install-consumer"
          -B "${_consumer_build_dir}"
          ${_generator_args}
          "-DMerlin_DIR=${_stage_dir}/${MERLIN_INSTALL_LIBDIR}/cmake/Merlin"
  RESULT_VARIABLE _configure_result
)
if(NOT _configure_result EQUAL 0)
  message(FATAL_ERROR "Merlin consumer configure failed: ${_configure_result}")
endif()

execute_process(
  COMMAND "${MERLIN_CMAKE_COMMAND}" --build "${_consumer_build_dir}"
          ${_config_args}
  RESULT_VARIABLE _build_result
)
if(NOT _build_result EQUAL 0)
  message(FATAL_ERROR "Merlin consumer build failed: ${_build_result}")
endif()

set(_consumer_executable
    "${_consumer_build_dir}/merlin-install-consumer${CMAKE_EXECUTABLE_SUFFIX}")
if(NOT "${MERLIN_CONFIG}" STREQUAL "")
  set(_consumer_executable
      "${_consumer_build_dir}/${MERLIN_CONFIG}/merlin-install-consumer${CMAKE_EXECUTABLE_SUFFIX}")
endif()

execute_process(
  COMMAND "${_consumer_executable}"
  RESULT_VARIABLE _run_result
)
if(NOT _run_result EQUAL 0)
  message(FATAL_ERROR "Merlin consumer run failed: ${_run_result}")
endif()
