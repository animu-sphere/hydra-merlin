include_guard(GLOBAL)

macro(merlin_add_metal_shaders)
  merlin_find_slang_dependencies()
  find_program(MERLIN_XCRUN_EXECUTABLE xcrun REQUIRED)
  set(_metal_deployment_target "${CMAKE_OSX_DEPLOYMENT_TARGET}")
  if(NOT _metal_deployment_target)
    set(_metal_deployment_target "14.0")
  endif()
  set(_metal_source "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
  set(_metal_shared "${PROJECT_SOURCE_DIR}/core/merlin-render-backend/shaders")
  set(_metal_output "${CMAKE_CURRENT_BINARY_DIR}/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}")
  set(_metal_artifacts "")
  set(_metal_air "")
  set(_metal_records "")
  foreach(_stage vertex fragment)
    set(_entry "gaussian_metal_${_stage}")
    set(_msl "${_metal_output}/gaussian.${_stage}.metal")
    set(_reflection "${_msl}.reflection.json")
    set(_depfile "${_msl}.d")
    add_custom_command(
      OUTPUT "${_msl}" "${_reflection}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_metal_output}"
      COMMAND "${MERLIN_SLANGC_EXECUTABLE}" gaussian-metal.slang
        -entry "${_entry}" -stage "${_stage}" -target metal -profile metallib_2_4
        -matrix-layout-column-major -O2 -warnings-as-errors all
        -reflection-json "${_reflection}" -depfile "${_depfile}" -o "${_msl}"
      DEPENDS "${_metal_source}/gaussian-metal.slang"
        "${_metal_shared}/gaussian-raster-common.slang"
        "${_metal_shared}/gaussian-raster-abi.slang"
      DEPFILE "${_depfile}"
      WORKING_DIRECTORY "${_metal_source}"
      VERBATIM)
    add_custom_command(
      OUTPUT "${_msl}.air"
      COMMAND "${MERLIN_XCRUN_EXECUTABLE}" -sdk macosx metal
        -std=macos-metal2.4
        "-mmacosx-version-min=${_metal_deployment_target}"
        "-fmodules-cache-path=${CMAKE_CURRENT_BINARY_DIR}/metal-module-cache"
        -c "${_msl}" -o "${_msl}.air"
      DEPENDS "${_msl}"
      VERBATIM)
    list(APPEND _metal_artifacts "${_msl}" "${_reflection}")
    list(APPEND _metal_air "${_msl}.air")
    string(APPEND _metal_records
      "${_msl}|${_reflection}|${_depfile}|gaussian-metal.slang|${_entry}|${_stage}|metal|metallib_2_4|none|gaussian-reference|prepared_stream+ellipse+color+ids\n")
  endforeach()
  set(_metal_library "${_metal_output}/gaussian.metallib")
  add_custom_command(
    OUTPUT "${_metal_library}"
    COMMAND "${MERLIN_XCRUN_EXECUTABLE}" -sdk macosx metallib
      ${_metal_air} -o "${_metal_library}"
    DEPENDS ${_metal_air}
    VERBATIM)
  add_custom_command(
    OUTPUT "${_metal_library}.sha256"
    COMMAND ${CMAKE_COMMAND} "-DMERLIN_INPUT=${_metal_library}"
      "-DMERLIN_OUTPUT=${_metal_library}.sha256"
      -P "${PROJECT_SOURCE_DIR}/cmake/write-sha256.cmake"
    DEPENDS "${_metal_library}" "${PROJECT_SOURCE_DIR}/cmake/write-sha256.cmake"
    VERBATIM)
  set(_metal_header "${CMAKE_CURRENT_BINARY_DIR}/generated/gaussian_metallib.hpp")
  add_custom_command(
    OUTPUT "${_metal_header}"
    COMMAND ${CMAKE_COMMAND}
      "-DMERLIN_METALLIB=${_metal_library}"
      "-DMERLIN_METAL_HEADER=${_metal_header}"
      -P "${PROJECT_SOURCE_DIR}/cmake/embed-metal-library.cmake"
    DEPENDS "${_metal_library}" "${PROJECT_SOURCE_DIR}/cmake/embed-metal-library.cmake"
    VERBATIM)
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/metal-shader-records.txt"
    CONTENT "${_metal_records}")
  set(_metal_manifest "${_metal_output}/manifest.json")
  add_custom_command(
    OUTPUT "${_metal_manifest}"
    COMMAND ${CMAKE_COMMAND}
      "-DMERLIN_SHADER_MANIFEST=${_metal_manifest}"
      "-DMERLIN_SHADER_SCHEMA_VERSION=${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}"
      "-DMERLIN_SHADER_MODULE_IDENTITY_SCHEMA=${MERLIN_SHADER_MODULE_IDENTITY_SCHEMA}"
      "-DMERLIN_SHADER_ARTIFACT_KEY_SCHEMA=${MERLIN_SHADER_ARTIFACT_KEY_SCHEMA}"
      "-DMERLIN_SHADER_ABI_VERSION=${MERLIN_SHADER_ABI_VERSION}"
      "-DMERLIN_SHADER_RECORDS_FILE=${CMAKE_CURRENT_BINARY_DIR}/metal-shader-records.txt"
      "-DMERLIN_SHADER_SOURCE_DIR=${_metal_source}"
      "-DMERLIN_SHARED_SHADER_SOURCE_DIR=${_metal_shared}"
      "-DMERLIN_SLANG_VERSION=${_merlin_slang_version}"
      "-DMERLIN_SLANG_REQUIRED_SERIES=${MERLIN_SLANG_REQUIRED_SERIES}"
      -DMERLIN_SLANG_MATRIX_LAYOUT=column-major -DMERLIN_SLANG_OPTIMIZATION=O2
      -DMERLIN_SLANG_DEBUG_INFO=false -DMERLIN_VULKAN_SDK_VERSION=unknown
      "-DMERLIN_CMAKE_GENERATOR=${CMAKE_GENERATOR}"
      -DMERLIN_ENVIRONMENT_HDR= -DMERLIN_SHADER_FORWARD_FALLBACK=OFF
      -P "${PROJECT_SOURCE_DIR}/cmake/write-shader-manifest.cmake"
    DEPENDS ${_metal_artifacts} "${CMAKE_CURRENT_BINARY_DIR}/metal-shader-records.txt"
      "${PROJECT_SOURCE_DIR}/cmake/write-shader-manifest.cmake"
    VERBATIM)
  add_custom_target(merlin-metal-shaders DEPENDS
    "${_metal_header}" "${_metal_manifest}" "${_metal_library}.sha256")
  add_dependencies(merlin-metal merlin-metal-shaders)
  target_include_directories(merlin-metal PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
  install(FILES ${_metal_artifacts} "${_metal_library}" "${_metal_manifest}"
    "${_metal_library}.sha256"
    DESTINATION "${CMAKE_INSTALL_BINDIR}/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}/metal")
  set(MERLIN_METAL_SHADER_OUTPUT_DIR "${_metal_output}" PARENT_SCOPE)
endmacro()
