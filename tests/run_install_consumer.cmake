if(NOT DEFINED MERLIN_CMAKE_COMMAND OR
   NOT DEFINED MERLIN_SOURCE_DIR OR
   NOT DEFINED MERLIN_BUILD_DIR OR
   NOT DEFINED MERLIN_TEST_BINARY_DIR OR
   NOT DEFINED MERLIN_INSTALL_LIBDIR OR
   NOT DEFINED MERLIN_INSTALL_BINDIR OR
   NOT DEFINED MERLIN_INSTALL_DATADIR OR
   NOT DEFINED MERLIN_EXECUTABLE_SUFFIX OR
   NOT DEFINED MERLIN_EXPECTED_VERSION OR
   NOT DEFINED MERLIN_GENERATOR OR
   NOT DEFINED MERLIN_MULTI_CONFIG)
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

# When the Vulkan runtime product is present, exercise the same renderer
# evidence path from the isolated install tree. This upgrades the canonical
# build report's install-tree assertion from an explained skip to a pass before
# `ost validate` consumes it. Core-only installs intentionally have no headless
# product and continue with the package-consumer checks below.
set(_headless
    "${_stage_dir}/${MERLIN_INSTALL_BINDIR}/merlin-headless${MERLIN_EXECUTABLE_SUFFIX}")
if(EXISTS "${_headless}")
  execute_process(
    COMMAND "${_headless}"
      --report "${MERLIN_BUILD_DIR}/renderer-report.json"
      --output "${MERLIN_TEST_BINARY_DIR}/renderer-install-smoke.ppm"
      --install-tree
    RESULT_VARIABLE _renderer_install_result
  )
  if(NOT _renderer_install_result EQUAL 0)
    message(FATAL_ERROR
      "installed renderer evidence failed: ${_renderer_install_result}")
  endif()
endif()

set(_metadata_file
    "${_stage_dir}/${MERLIN_INSTALL_DATADIR}/merlin/merlin-release-metadata.json")
if(NOT EXISTS "${_metadata_file}")
  message(FATAL_ERROR
    "installed release metadata is missing: ${_metadata_file}")
endif()

set(_version_file
    "${_stage_dir}/${MERLIN_INSTALL_DATADIR}/merlin/VERSION")
if(NOT EXISTS "${_version_file}")
  message(FATAL_ERROR "installed VERSION is missing: ${_version_file}")
endif()
file(READ "${_version_file}" _installed_version)
string(STRIP "${_installed_version}" _installed_version)
if(NOT "${_installed_version}" STREQUAL "${MERLIN_EXPECTED_VERSION}")
  message(FATAL_ERROR
    "installed VERSION ${_installed_version} != ${MERLIN_EXPECTED_VERSION}")
endif()

set(_generator_args -G "${MERLIN_GENERATOR}")
if(NOT "${MERLIN_GENERATOR_PLATFORM}" STREQUAL "")
  list(APPEND _generator_args -A "${MERLIN_GENERATOR_PLATFORM}")
endif()
if(NOT "${MERLIN_GENERATOR_TOOLSET}" STREQUAL "")
  list(APPEND _generator_args -T "${MERLIN_GENERATOR_TOOLSET}")
endif()
if(NOT MERLIN_MULTI_CONFIG AND NOT "${MERLIN_CONFIG}" STREQUAL "")
  list(APPEND _generator_args "-DCMAKE_BUILD_TYPE=${MERLIN_CONFIG}")
endif()

execute_process(
  COMMAND "${MERLIN_CMAKE_COMMAND}"
          -S "${MERLIN_SOURCE_DIR}/tests/install-consumer"
          -B "${_consumer_build_dir}"
          ${_generator_args}
          "-DMerlin_DIR=${_stage_dir}/${MERLIN_INSTALL_LIBDIR}/cmake/Merlin"
          -DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=TRUE
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
if(MERLIN_MULTI_CONFIG AND NOT "${MERLIN_CONFIG}" STREQUAL "")
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
