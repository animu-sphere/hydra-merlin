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
  set(_shader_dir
      "${_stage_dir}/${MERLIN_INSTALL_BINDIR}/shaders/v1")
  foreach(_shader_file
      triangle.vert.spv triangle.frag.spv
      triangle.bindless.vert.spv triangle.bindless.frag.spv
      triangle.vert.metal triangle.frag.metal
      triangle.vert.spv.reflection.json triangle.frag.spv.reflection.json
      triangle.bindless.vert.spv.reflection.json
      triangle.bindless.frag.spv.reflection.json
      triangle.vert.metal.reflection.json
      triangle.frag.metal.reflection.json manifest.json environment.hdr)
    if(NOT EXISTS "${_shader_dir}/${_shader_file}")
      message(FATAL_ERROR
        "installed shader artifact is missing: ${_shader_dir}/${_shader_file}")
    endif()
  endforeach()
  file(READ "${_shader_dir}/manifest.json" _shader_manifest)
  string(JSON _shader_schema ERROR_VARIABLE _shader_json_error
         GET "${_shader_manifest}" schema_version)
  if(_shader_json_error OR NOT _shader_schema EQUAL 1)
    message(FATAL_ERROR
      "installed shader manifest is invalid: ${_shader_json_error}")
  endif()
  file(SHA256 "${_shader_dir}/environment.hdr" _environment_sha256)
  if(NOT _environment_sha256 STREQUAL
     "4897697c757edc524dc9b7bcc692e8e05a7f02dbede3e30d2291dc0831dece17")
    message(FATAL_ERROR
      "installed environment.hdr has unexpected SHA-256: ${_environment_sha256}")
  endif()
  foreach(_notice_file IN ITEMS
      "${_stage_dir}/${MERLIN_INSTALL_DATADIR}/merlin/licenses/THIRD_PARTY_NOTICES.md"
      "${_stage_dir}/${MERLIN_INSTALL_DATADIR}/merlin/licenses/openusd/LICENSE.txt"
      "${_stage_dir}/${MERLIN_INSTALL_DATADIR}/merlin/licenses/openusd/NOTICE.txt")
    if(NOT EXISTS "${_notice_file}")
      message(FATAL_ERROR
        "installed third-party notice is missing: ${_notice_file}")
    endif()
  endforeach()
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

# MaterialX-enabled installs compile and link the public compiler boundary from
# the isolated prefix. This catches missing static transitive dependencies and
# sibling MaterialX package discovery without requiring a renderer backend.
set(_materialx_targets
    "${_stage_dir}/${MERLIN_INSTALL_LIBDIR}/cmake/Merlin/MerlinMaterialXTargets.cmake")
if(EXISTS "${_materialx_targets}")
  set(_materialx_consumer_build_dir
      "${MERLIN_TEST_BINARY_DIR}/materialx-install-consumer-build")
  execute_process(
    COMMAND "${MERLIN_CMAKE_COMMAND}"
            -S "${MERLIN_SOURCE_DIR}/tests/materialx-install-consumer"
            -B "${_materialx_consumer_build_dir}"
            ${_generator_args}
            "-DCMAKE_PREFIX_PATH=${_stage_dir}"
            "-DMerlin_DIR=${_stage_dir}/${MERLIN_INSTALL_LIBDIR}/cmake/Merlin"
    RESULT_VARIABLE _materialx_configure_result
  )
  if(NOT _materialx_configure_result EQUAL 0)
    message(FATAL_ERROR
      "Merlin MaterialX consumer configure failed: ${_materialx_configure_result}")
  endif()
  execute_process(
    COMMAND "${MERLIN_CMAKE_COMMAND}" --build
            "${_materialx_consumer_build_dir}" ${_config_args}
    RESULT_VARIABLE _materialx_build_result
  )
  if(NOT _materialx_build_result EQUAL 0)
    message(FATAL_ERROR
      "Merlin MaterialX consumer build failed: ${_materialx_build_result}")
  endif()
  set(_materialx_consumer_executable
      "${_materialx_consumer_build_dir}/merlin-materialx-install-consumer${CMAKE_EXECUTABLE_SUFFIX}")
  if(MERLIN_MULTI_CONFIG AND NOT "${MERLIN_CONFIG}" STREQUAL "")
    set(_materialx_consumer_executable
        "${_materialx_consumer_build_dir}/${MERLIN_CONFIG}/merlin-materialx-install-consumer${CMAKE_EXECUTABLE_SUFFIX}")
  endif()
  execute_process(
    COMMAND "${_materialx_consumer_executable}"
    RESULT_VARIABLE _materialx_run_result
  )
  if(NOT _materialx_run_result EQUAL 0)
    message(FATAL_ERROR
      "Merlin MaterialX consumer run failed: ${_materialx_run_result}")
  endif()
endif()

# Vulkan-enabled installs additionally compile the v0.4.0 execution and image
# artifact public headers through the isolated package export.
if(EXISTS "${_headless}")
  set(_vulkan_consumer_build_dir
      "${MERLIN_TEST_BINARY_DIR}/vulkan-install-consumer-build")
  execute_process(
    COMMAND "${MERLIN_CMAKE_COMMAND}"
            -S "${MERLIN_SOURCE_DIR}/tests/vulkan-install-consumer"
            -B "${_vulkan_consumer_build_dir}"
            ${_generator_args}
            "-DMerlin_DIR=${_stage_dir}/${MERLIN_INSTALL_LIBDIR}/cmake/Merlin"
    RESULT_VARIABLE _vulkan_configure_result
  )
  if(NOT _vulkan_configure_result EQUAL 0)
    message(FATAL_ERROR
      "Merlin Vulkan consumer configure failed: ${_vulkan_configure_result}")
  endif()
  execute_process(
    COMMAND "${MERLIN_CMAKE_COMMAND}" --build "${_vulkan_consumer_build_dir}"
            ${_config_args}
    RESULT_VARIABLE _vulkan_build_result
  )
  if(NOT _vulkan_build_result EQUAL 0)
    message(FATAL_ERROR
      "Merlin Vulkan consumer build failed: ${_vulkan_build_result}")
  endif()
  set(_vulkan_consumer_executable
      "${_vulkan_consumer_build_dir}/merlin-vulkan-install-consumer${CMAKE_EXECUTABLE_SUFFIX}")
  if(MERLIN_MULTI_CONFIG AND NOT "${MERLIN_CONFIG}" STREQUAL "")
    set(_vulkan_consumer_executable
        "${_vulkan_consumer_build_dir}/${MERLIN_CONFIG}/merlin-vulkan-install-consumer${CMAKE_EXECUTABLE_SUFFIX}"
    )
  endif()
  execute_process(
    COMMAND "${_vulkan_consumer_executable}"
    RESULT_VARIABLE _vulkan_run_result
  )
  if(NOT _vulkan_run_result EQUAL 0)
    message(FATAL_ERROR
      "Merlin Vulkan consumer run failed: ${_vulkan_run_result}")
  endif()
endif()
