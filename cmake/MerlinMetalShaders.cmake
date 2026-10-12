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
  set(_metal_forward_air "")
  set(_metal_records "")
  set(_metal_entries
    "gaussian-metal.slang|gaussian_metal_vertex|vertex|gaussian.vertex|gaussian-reference|prepared_stream+ellipse+color+ids"
    "gaussian-metal.slang|gaussian_metal_fragment|fragment|gaussian.fragment|gaussian-reference|prepared_stream+ellipse+color+ids"
    "gaussian-prepare-metal.slang|gaussian_prepare_compact|compute|gaussian.prepare|gaussian-prepare|projection+sh+compaction"
    "gaussian-gather-metal.slang|gaussian_metal_gather|compute|gaussian.gather|gaussian-gather|sorted_stream+indirect_draw")
  foreach(_sort keys histogram scan_blocks scan_add scatter verify)
    list(APPEND _metal_entries
      "gaussian-sort-metal.slang|gaussian_sort_${_sort}|compute|gaussian.sort-${_sort}|gaussian-sort-${_sort}|deterministic_radix_sort")
  endforeach()
  foreach(_tile count emit ranges verify)
    list(APPEND _metal_entries
      "gaussian-tile-metal.slang|gaussian_tile_${_tile}|compute|gaussian.tile-${_tile}|gaussian-tile-${_tile}|stable_tile_binning")
  endforeach()
  list(APPEND _metal_entries
    "gaussian-tile-raster-metal.slang|gaussian_tile_raster_select|compute|gaussian.tile-raster-select|gaussian-tile-raster-select|tile_compute_raster"
    "gaussian-tile-raster-metal.slang|gaussian_tile_raster|compute|gaussian.tile-raster|gaussian-tile-raster|tile_compute_raster")
  list(APPEND _metal_entries
    "forward-metal.slang|merlin_vertex|vertex|forward.vertex|forward|native_metal_binding"
    "forward-metal.slang|merlin_fragment_conventional|fragment|forward.conventional|forward-conventional|native_metal_binding"
    "forward-metal.slang|merlin_fragment_argument_buffer|fragment|forward.argument-buffer|forward-argument-buffer|native_metal_binding"
    "forward-gpu-scene-metal.slang|merlin_vertex_gpu_scene|vertex|forward.gpu-scene-vertex|forward-gpu-scene|gpu_scene"
    "forward-gpu-scene-metal.slang|merlin_fragment_gpu_scene|fragment|forward.gpu-scene-fragment|forward-gpu-scene|gpu_scene"
    "color-convert-metal.slang|merlin_convert_color|compute|forward.color-convert|color-convert|color_copy"
    "presentation-metal.slang|merlin_presentation_vertex|vertex|presentation.vertex|presentation|native_presentation"
    "presentation-metal.slang|merlin_presentation_fragment|fragment|presentation.fragment|presentation|native_presentation")
  foreach(_record IN LISTS _metal_entries)
    string(REPLACE "|" ";" _fields "${_record}")
    list(GET _fields 0 _source)
    list(GET _fields 1 _entry)
    list(GET _fields 2 _stage)
    list(GET _fields 3 _name)
    list(GET _fields 4 _permutation)
    list(GET _fields 5 _features)
    set(_msl "${_metal_output}/${_name}.metal")
    set(_reflection "${_msl}.reflection.json")
    set(_depfile "${_msl}.d")
    set(_metal_compile_options "")
    if(NOT _source MATCHES "^gaussian")
      set(_metal_compile_options -Werror)
    endif()
    set(_metal_normalize_command "")
    if(_source STREQUAL "forward-gpu-scene-metal.slang")
      set(_metal_normalize_command COMMAND ${CMAKE_COMMAND}
        "-DMERLIN_METAL_SOURCE=${_msl}"
        -P "${PROJECT_SOURCE_DIR}/cmake/normalize-metal-readonly.cmake")
    endif()
    if(_stage STREQUAL "compute")
      # Preparation classifies non-finite inputs. Preserve those checks.
      # Slang emits unused temporaries for the return value of atomics.
      set(_metal_compile_options -fno-fast-math -Wno-unused-variable -Werror)
    endif()
    add_custom_command(
      OUTPUT "${_msl}" "${_reflection}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_metal_output}"
      COMMAND "${MERLIN_SLANGC_EXECUTABLE}" "${_source}"
        -entry "${_entry}" -stage "${_stage}" -target metal -profile metallib_2_4
        -matrix-layout-column-major -O2 -warnings-as-errors all
        -reflection-json "${_reflection}" -depfile "${_depfile}" -o "${_msl}"
      ${_metal_normalize_command}
      DEPENDS "${_metal_source}/${_source}"
        "${PROJECT_SOURCE_DIR}/cmake/normalize-metal-readonly.cmake"
        "${_metal_shared}/gaussian-raster-common.slang"
        "${_metal_shared}/gaussian-raster-abi.slang"
        "${_metal_shared}/forward-policy.slang"
      DEPFILE "${_depfile}"
      WORKING_DIRECTORY "${_metal_source}"
      VERBATIM)
    add_custom_command(
      OUTPUT "${_msl}.air"
      COMMAND "${MERLIN_XCRUN_EXECUTABLE}" -sdk macosx metal
        -std=macos-metal2.4
        ${_metal_compile_options}
        "-mmacosx-version-min=${_metal_deployment_target}"
        "-fmodules-cache-path=${CMAKE_CURRENT_BINARY_DIR}/metal-module-cache"
        -c "${_msl}" -o "${_msl}.air"
      DEPENDS "${_msl}"
      VERBATIM)
    list(APPEND _metal_artifacts "${_msl}" "${_reflection}")
    if(_source MATCHES "^gaussian")
      list(APPEND _metal_air "${_msl}.air")
    else()
      list(APPEND _metal_forward_air "${_msl}.air")
    endif()
    string(APPEND _metal_records
      "${_msl}|${_reflection}|${_depfile}|${_source}|${_entry}|${_stage}|metal|metallib_2_4|none|${_permutation}|${_features}\n")
  endforeach()
  set(_metal_libraries "")
  set(_metal_checksums "")
  set(_metal_headers "")
  foreach(_group gaussian forward)
    if(_group STREQUAL "gaussian")
      set(_group_air "${_metal_air}")
      set(_group_symbol kGaussianMetalLibrary)
    else()
      set(_group_air "${_metal_forward_air}")
      set(_group_symbol kForwardMetalLibrary)
    endif()
    set(_library "${_metal_output}/${_group}.metallib")
    set(_header "${CMAKE_CURRENT_BINARY_DIR}/generated/${_group}_metallib.hpp")
    add_custom_command(
      OUTPUT "${_library}"
      COMMAND "${MERLIN_XCRUN_EXECUTABLE}" -sdk macosx metallib
        ${_group_air} -o "${_library}"
      DEPENDS ${_group_air}
      VERBATIM)
    add_custom_command(
      OUTPUT "${_library}.sha256"
      COMMAND ${CMAKE_COMMAND} "-DMERLIN_INPUT=${_library}"
        "-DMERLIN_OUTPUT=${_library}.sha256"
        -P "${PROJECT_SOURCE_DIR}/cmake/write-sha256.cmake"
      DEPENDS "${_library}" "${PROJECT_SOURCE_DIR}/cmake/write-sha256.cmake"
      VERBATIM)
    add_custom_command(
      OUTPUT "${_header}"
      COMMAND ${CMAKE_COMMAND}
        "-DMERLIN_METALLIB=${_library}"
        "-DMERLIN_METAL_HEADER=${_header}"
        "-DMERLIN_METAL_SYMBOL=${_group_symbol}"
        -P "${PROJECT_SOURCE_DIR}/cmake/embed-metal-library.cmake"
      DEPENDS "${_library}" "${PROJECT_SOURCE_DIR}/cmake/embed-metal-library.cmake"
      VERBATIM)
    list(APPEND _metal_libraries "${_library}")
    list(APPEND _metal_checksums "${_library}.sha256")
    list(APPEND _metal_headers "${_header}")
  endforeach()
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
    ${_metal_headers} "${_metal_manifest}" ${_metal_checksums})
  add_dependencies(merlin-metal merlin-metal-shaders)
  target_include_directories(merlin-metal PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
  install(FILES ${_metal_artifacts} ${_metal_libraries} ${_metal_checksums}
    "${_metal_manifest}"
    DESTINATION "${CMAKE_INSTALL_BINDIR}/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}/metal")
  set(MERLIN_METAL_SHADER_OUTPUT_DIR "${_metal_output}" PARENT_SCOPE)
endmacro()
