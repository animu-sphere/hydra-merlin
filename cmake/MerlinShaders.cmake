# Configure in the owning directory to preserve artifact paths and outputs.
# Macros intentionally retain the backend directory and its PARENT_SCOPE exports.
include_guard(GLOBAL)

macro(merlin_add_vulkan_shaders)
  set(MERLIN_SHADER_OUTPUT_DIR
      "${CMAKE_CURRENT_BINARY_DIR}/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}")
  set(MERLIN_ENVIRONMENT_HDR
      "${CMAKE_CURRENT_SOURCE_DIR}/assets/environment.hdr")
  set(MERLIN_VERTEX_SPV "${MERLIN_SHADER_OUTPUT_DIR}/triangle.vert.spv")
  set(MERLIN_FRAGMENT_SPV "${MERLIN_SHADER_OUTPUT_DIR}/triangle.frag.spv")
  set(MERLIN_BINDLESS_VERTEX_SPV
      "${MERLIN_SHADER_OUTPUT_DIR}/triangle.bindless.vert.spv")
  set(MERLIN_BINDLESS_FRAGMENT_SPV
      "${MERLIN_SHADER_OUTPUT_DIR}/triangle.bindless.frag.spv")
  set(MERLIN_GPU_SCENE_VERTEX_SPV
      "${MERLIN_SHADER_OUTPUT_DIR}/triangle.gpu-scene.vert.spv")
  set(MERLIN_GPU_SCENE_FRAGMENT_SPV
      "${MERLIN_SHADER_OUTPUT_DIR}/triangle.gpu-scene.frag.spv")
  set(MERLIN_GPU_DRIVEN_INDEXED_COMPUTE_SPV
      "${MERLIN_SHADER_OUTPUT_DIR}/gpu-driven-indexed.comp.spv")
  set(MERLIN_GPU_DRIVEN_VERTEX_SPV
      "${MERLIN_SHADER_OUTPUT_DIR}/gpu-driven-forward.vert.spv")
  set(MERLIN_GPU_DRIVEN_FRAGMENT_SPV
      "${MERLIN_SHADER_OUTPUT_DIR}/gpu-driven-forward.frag.spv")
  set(MERLIN_GAUSSIAN_PREPARE_COMPUTE_SPV
      "${MERLIN_SHADER_OUTPUT_DIR}/gaussian-prepare.comp.spv")
  set(MERLIN_GAUSSIAN_SORT_KEYS_COMPUTE_SPV
      "${MERLIN_SHADER_OUTPUT_DIR}/gaussian-sort-keys.comp.spv")
  set(MERLIN_GAUSSIAN_SORT_HISTOGRAM_COMPUTE_SPV
      "${MERLIN_SHADER_OUTPUT_DIR}/gaussian-sort-histogram.comp.spv")
  set(MERLIN_GAUSSIAN_SORT_SCAN_COMPUTE_SPV
      "${MERLIN_SHADER_OUTPUT_DIR}/gaussian-sort-scan.comp.spv")
  set(MERLIN_GAUSSIAN_SORT_SCAN_ADD_COMPUTE_SPV
      "${MERLIN_SHADER_OUTPUT_DIR}/gaussian-sort-scan-add.comp.spv")
  set(MERLIN_GAUSSIAN_SORT_SCATTER_COMPUTE_SPV
      "${MERLIN_SHADER_OUTPUT_DIR}/gaussian-sort-scatter.comp.spv")
  set(MERLIN_GAUSSIAN_SORT_VERIFY_COMPUTE_SPV
      "${MERLIN_SHADER_OUTPUT_DIR}/gaussian-sort-verify.comp.spv")
  set(MERLIN_GAUSSIAN_VERTEX_SPV
      "${MERLIN_SHADER_OUTPUT_DIR}/gaussian.vert.spv")
  set(MERLIN_GAUSSIAN_FRAGMENT_SPV
      "${MERLIN_SHADER_OUTPUT_DIR}/gaussian.frag.spv")
  set(MERLIN_GAUSSIAN_ID_FRAGMENT_SPV
      "${MERLIN_SHADER_OUTPUT_DIR}/gaussian-id.frag.spv")
  set(MERLIN_GAUSSIAN_ID_VERTEX_SPV
      "${MERLIN_SHADER_OUTPUT_DIR}/gaussian-id.vert.spv")
  set(MERLIN_VERTEX_METAL "${MERLIN_SHADER_OUTPUT_DIR}/triangle.vert.metal")
  set(MERLIN_FRAGMENT_METAL "${MERLIN_SHADER_OUTPUT_DIR}/triangle.frag.metal")

  set(_merlin_shader_source_dir "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
  set(_merlin_shader_common
      "${_merlin_shader_source_dir}/forward-common.slang")
  set(_merlin_shader_forward
      "${_merlin_shader_source_dir}/forward.slang")
  set(_merlin_shader_bindless
      "${_merlin_shader_source_dir}/forward-bindless.slang")
  set(_merlin_shader_gpu_scene
      "${_merlin_shader_source_dir}/forward-gpu-scene.slang")
  set(_merlin_shader_gpu_driven_indexed
      "${_merlin_shader_source_dir}/gpu-driven-indexed.slang")
  set(_merlin_shader_gpu_driven_forward
      "${_merlin_shader_source_dir}/forward-gpu-driven.slang")
  set(_merlin_shader_gaussian_prepare
      "${_merlin_shader_source_dir}/gaussian-prepare.slang")
  set(_merlin_shader_gaussian_sort
      "${_merlin_shader_source_dir}/gaussian-sort.slang")
  set(_merlin_gpu_scene_abi
      "${PROJECT_SOURCE_DIR}/core/merlin-render-backend/shaders/gpu-scene-abi.slang")
  set(_merlin_shader_gaussian
      "${_merlin_shader_source_dir}/gaussian.slang")

  # Compile policy shared by every target. These feed both the slangc command
  # line and the manifest cache keys, so changing one moves the other.
  set(_merlin_slang_spirv_profile "sm_6_6")
  set(_merlin_slang_metal_profile "metallib_2_4")
  set(_merlin_slang_matrix_layout "column-major")
  set(_merlin_slang_optimization "O2")
  # Recorded in the artifact key, so it has to drive the compile rather than only
  # describe it: packaged artifacts are release builds without debug info.
  set(_merlin_slang_debug_info OFF)
  if(_merlin_slang_debug_info)
    set(_merlin_slang_debug_arguments -g)
    set(_merlin_slang_debug_info_json "true")
  else()
    set(_merlin_slang_debug_arguments "")
    set(_merlin_slang_debug_info_json "false")
  endif()

  set(_merlin_conventional_features "material_constants+base_color_texture")
  set(_merlin_bindless_features
      "${_merlin_conventional_features}+bindless_resources+non_uniform_resource_indexing")

  # _capabilities is the literal -capability argument, or "none" when the target
  # takes no capability flag. Every field below is also recorded verbatim for the
  # manifest, so the provenance and the compile share one source of truth.
  function(_merlin_compile_shader _output _source _entry _stage _target _profile
                                 _capabilities _permutation _features)
    set(_reflection "${_output}.reflection.json")
    set(_depfile "${_output}.d")
    set(_capability_arguments "")
    if(NOT _capabilities STREQUAL "none")
      set(_capability_arguments -capability "${_capabilities}")
    endif()
    add_custom_command(
      OUTPUT "${_output}" "${_reflection}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${MERLIN_SHADER_OUTPUT_DIR}"
      COMMAND "${MERLIN_SLANGC_EXECUTABLE}" "${_source}"
              -entry "${_entry}" -stage "${_stage}"
              -target "${_target}" -profile "${_profile}"
              ${_capability_arguments}
              "-matrix-layout-${_merlin_slang_matrix_layout}"
              "-${_merlin_slang_optimization}"
              ${_merlin_slang_debug_arguments}
              -warnings-as-errors all
              -reflection-json "${_reflection}"
              -depfile "${_depfile}"
              -o "${_output}"
      DEPENDS
        "${_merlin_shader_source_dir}/${_source}"
        "${_merlin_shader_common}"
      DEPFILE "${_depfile}"
      WORKING_DIRECTORY "${_merlin_shader_source_dir}"
      COMMENT "Slang ${_target} ${_entry}"
      VERBATIM
    )
    set_property(GLOBAL APPEND PROPERTY MERLIN_SHADER_RECORDS
      "${_output}|${_reflection}|${_depfile}|${_source}|${_entry}|${_stage}|${_target}|${_profile}|${_capabilities}|${_permutation}|${_features}")
  endfunction()

  _merlin_compile_shader("${MERLIN_VERTEX_SPV}" forward.slang
    forward_vertex vertex spirv "${_merlin_slang_spirv_profile}"
    spirv_1_5 forward-conventional "${_merlin_conventional_features}")
  _merlin_compile_shader("${MERLIN_FRAGMENT_SPV}" forward.slang
    forward_fragment fragment spirv "${_merlin_slang_spirv_profile}"
    spirv_1_5 forward-conventional "${_merlin_conventional_features}")
  _merlin_compile_shader("${MERLIN_BINDLESS_VERTEX_SPV}" forward-bindless.slang
    forward_bindless_vertex vertex spirv "${_merlin_slang_spirv_profile}"
    spirv_1_5+spvShaderNonUniformEXT forward-bindless
    "${_merlin_bindless_features}")
  _merlin_compile_shader("${MERLIN_BINDLESS_FRAGMENT_SPV}" forward-bindless.slang
    forward_bindless_fragment fragment spirv "${_merlin_slang_spirv_profile}"
    spirv_1_5+spvShaderNonUniformEXT forward-bindless
    "${_merlin_bindless_features}")
  _merlin_compile_shader("${MERLIN_GPU_SCENE_VERTEX_SPV}" forward-gpu-scene.slang
    forward_gpu_scene_vertex vertex spirv "${_merlin_slang_spirv_profile}"
    spirv_1_5+spvShaderNonUniformEXT forward-gpu-scene
    "${_merlin_bindless_features}+gpu_scene_abi_v1")
  _merlin_compile_shader("${MERLIN_GPU_SCENE_FRAGMENT_SPV}" forward-gpu-scene.slang
    forward_gpu_scene_fragment fragment spirv "${_merlin_slang_spirv_profile}"
    spirv_1_5+spvShaderNonUniformEXT forward-gpu-scene
    "${_merlin_bindless_features}+gpu_scene_abi_v1")
  _merlin_compile_shader("${MERLIN_GPU_DRIVEN_INDEXED_COMPUTE_SPV}"
    gpu-driven-indexed.slang gpu_driven_indexed_compact compute spirv
    "${_merlin_slang_spirv_profile}" spirv_1_5 gpu-driven-indexed-parallel
    "gpu_scene_abi_v1+visibility_mask_culling+frustum_culling+parallel_atomic_compaction")
  _merlin_compile_shader("${MERLIN_GPU_DRIVEN_VERTEX_SPV}"
    forward-gpu-driven.slang forward_gpu_driven_vertex vertex spirv
    "${_merlin_slang_spirv_profile}" spirv_1_5+spvShaderNonUniformEXT
    gpu-driven-forward "${_merlin_bindless_features}+gpu_scene_abi_v1")
  _merlin_compile_shader("${MERLIN_GPU_DRIVEN_FRAGMENT_SPV}"
    forward-gpu-driven.slang forward_gpu_driven_fragment fragment spirv
    "${_merlin_slang_spirv_profile}" spirv_1_5+spvShaderNonUniformEXT
    gpu-driven-forward "${_merlin_bindless_features}+gpu_scene_abi_v1")
  _merlin_compile_shader("${MERLIN_GAUSSIAN_PREPARE_COMPUTE_SPV}"
    gaussian-prepare.slang gaussian_prepare_compact compute spirv
    "${_merlin_slang_spirv_profile}" spirv_1_5 gaussian-prepare-compact
    "persistent_gaussian_attributes+projection+culling+parallel_atomic_compaction")
  _merlin_compile_shader("${MERLIN_GAUSSIAN_SORT_KEYS_COMPUTE_SPV}"
    gaussian-sort.slang gaussian_sort_keys compute spirv
    "${_merlin_slang_spirv_profile}" spirv_1_5 gaussian-sort-keys
    "prepared_record_keys+candidate_identity_tie_break")
  _merlin_compile_shader("${MERLIN_GAUSSIAN_SORT_HISTOGRAM_COMPUTE_SPV}"
    gaussian-sort.slang gaussian_sort_histogram compute spirv
    "${_merlin_slang_spirv_profile}" spirv_1_5 gaussian-sort-histogram
    "radix_8bit+digit_major_histogram")
  _merlin_compile_shader("${MERLIN_GAUSSIAN_SORT_SCAN_COMPUTE_SPV}"
    gaussian-sort.slang gaussian_sort_scan_blocks compute spirv
    "${_merlin_slang_spirv_profile}" spirv_1_5 gaussian-sort-scan
    "exclusive_block_scan")
  _merlin_compile_shader("${MERLIN_GAUSSIAN_SORT_SCAN_ADD_COMPUTE_SPV}"
    gaussian-sort.slang gaussian_sort_scan_add compute spirv
    "${_merlin_slang_spirv_profile}" spirv_1_5 gaussian-sort-scan-add
    "exclusive_block_scan")
  _merlin_compile_shader("${MERLIN_GAUSSIAN_SORT_SCATTER_COMPUTE_SPV}"
    gaussian-sort.slang gaussian_sort_scatter compute spirv
    "${_merlin_slang_spirv_profile}" spirv_1_5 gaussian-sort-scatter
    "radix_8bit+stable_scatter")
  _merlin_compile_shader("${MERLIN_GAUSSIAN_SORT_VERIFY_COMPUTE_SPV}"
    gaussian-sort.slang gaussian_sort_verify compute spirv
    "${_merlin_slang_spirv_profile}" spirv_1_5 gaussian-sort-verify
    "order_verification+identity_checksum")
  _merlin_compile_shader("${MERLIN_GAUSSIAN_VERTEX_SPV}" gaussian.slang
    gaussian_vertex vertex spirv "${_merlin_slang_spirv_profile}"
    spirv_1_5 gaussian-mvp "prepared_stream+procedural_quad")
  _merlin_compile_shader("${MERLIN_GAUSSIAN_ID_VERTEX_SPV}" gaussian.slang
    gaussian_id_vertex vertex spirv "${_merlin_slang_spirv_profile}"
    spirv_1_5 gaussian-mvp "prepared_stream+procedural_quad+particle_id")
  _merlin_compile_shader("${MERLIN_GAUSSIAN_FRAGMENT_SPV}" gaussian.slang
    gaussian_fragment fragment spirv "${_merlin_slang_spirv_profile}"
    spirv_1_5 gaussian-mvp "elliptical_falloff+alpha_compositing")
  _merlin_compile_shader("${MERLIN_GAUSSIAN_ID_FRAGMENT_SPV}" gaussian.slang
    gaussian_id_fragment fragment spirv "${_merlin_slang_spirv_profile}"
    spirv_1_5 gaussian-mvp "elliptical_falloff+prim_id+particle_id")
  _merlin_compile_shader("${MERLIN_VERTEX_METAL}" forward.slang
    forward_vertex vertex metal "${_merlin_slang_metal_profile}"
    none forward-conventional "${_merlin_conventional_features}")
  _merlin_compile_shader("${MERLIN_FRAGMENT_METAL}" forward.slang
    forward_fragment fragment metal "${_merlin_slang_metal_profile}"
    none forward-conventional "${_merlin_conventional_features}")

  get_property(_merlin_shader_records GLOBAL PROPERTY MERLIN_SHADER_RECORDS)
  list(JOIN _merlin_shader_records "\n" _merlin_shader_records_text)
  set(_merlin_shader_records_file
      "${CMAKE_CURRENT_BINARY_DIR}/shader-records.txt")
  file(GENERATE OUTPUT "${_merlin_shader_records_file}"
    CONTENT "${_merlin_shader_records_text}\n")

  set(MERLIN_SHADER_MANIFEST "${MERLIN_SHADER_OUTPUT_DIR}/manifest.json")
  set(_merlin_shader_reflections
    "${MERLIN_VERTEX_SPV}.reflection.json"
    "${MERLIN_FRAGMENT_SPV}.reflection.json"
    "${MERLIN_BINDLESS_VERTEX_SPV}.reflection.json"
    "${MERLIN_BINDLESS_FRAGMENT_SPV}.reflection.json"
    "${MERLIN_GPU_SCENE_VERTEX_SPV}.reflection.json"
    "${MERLIN_GPU_SCENE_FRAGMENT_SPV}.reflection.json"
    "${MERLIN_GPU_DRIVEN_INDEXED_COMPUTE_SPV}.reflection.json"
    "${MERLIN_GPU_DRIVEN_VERTEX_SPV}.reflection.json"
    "${MERLIN_GPU_DRIVEN_FRAGMENT_SPV}.reflection.json"
    "${MERLIN_GAUSSIAN_PREPARE_COMPUTE_SPV}.reflection.json"
    "${MERLIN_GAUSSIAN_SORT_KEYS_COMPUTE_SPV}.reflection.json"
    "${MERLIN_GAUSSIAN_SORT_HISTOGRAM_COMPUTE_SPV}.reflection.json"
    "${MERLIN_GAUSSIAN_SORT_SCAN_COMPUTE_SPV}.reflection.json"
    "${MERLIN_GAUSSIAN_SORT_SCAN_ADD_COMPUTE_SPV}.reflection.json"
    "${MERLIN_GAUSSIAN_SORT_SCATTER_COMPUTE_SPV}.reflection.json"
    "${MERLIN_GAUSSIAN_SORT_VERIFY_COMPUTE_SPV}.reflection.json"
    "${MERLIN_GAUSSIAN_VERTEX_SPV}.reflection.json"
    "${MERLIN_GAUSSIAN_ID_VERTEX_SPV}.reflection.json"
    "${MERLIN_GAUSSIAN_FRAGMENT_SPV}.reflection.json"
    "${MERLIN_GAUSSIAN_ID_FRAGMENT_SPV}.reflection.json"
    "${MERLIN_VERTEX_METAL}.reflection.json"
    "${MERLIN_FRAGMENT_METAL}.reflection.json"
  )
  add_custom_command(
    OUTPUT "${MERLIN_SHADER_MANIFEST}"
    COMMAND ${CMAKE_COMMAND}
      "-DMERLIN_SHADER_MANIFEST:FILEPATH=${MERLIN_SHADER_MANIFEST}"
      "-DMERLIN_SHADER_SCHEMA_VERSION:STRING=${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}"
      "-DMERLIN_SHADER_MODULE_IDENTITY_SCHEMA:STRING=${MERLIN_SHADER_MODULE_IDENTITY_SCHEMA}"
      "-DMERLIN_SHADER_ARTIFACT_KEY_SCHEMA:STRING=${MERLIN_SHADER_ARTIFACT_KEY_SCHEMA}"
      "-DMERLIN_SHADER_ABI_VERSION:STRING=${MERLIN_SHADER_ABI_VERSION}"
      "-DMERLIN_SHADER_RECORDS_FILE:FILEPATH=${_merlin_shader_records_file}"
      "-DMERLIN_SHADER_SOURCE_DIR:PATH=${_merlin_shader_source_dir}"
      "-DMERLIN_SHARED_SHADER_SOURCE_DIR:PATH=${PROJECT_SOURCE_DIR}/core/merlin-render-backend/shaders"
      "-DMERLIN_SLANG_VERSION:STRING=${_merlin_slang_version}"
      "-DMERLIN_SLANG_REQUIRED_SERIES:STRING=${MERLIN_SLANG_REQUIRED_SERIES}"
      "-DMERLIN_SLANG_MATRIX_LAYOUT:STRING=${_merlin_slang_matrix_layout}"
      "-DMERLIN_SLANG_OPTIMIZATION:STRING=${_merlin_slang_optimization}"
      "-DMERLIN_SLANG_DEBUG_INFO:STRING=${_merlin_slang_debug_info_json}"
      "-DMERLIN_VULKAN_SDK_VERSION:STRING=${_merlin_vulkan_sdk_version}"
      "-DMERLIN_CMAKE_GENERATOR:STRING=${CMAKE_GENERATOR}"
      "-DMERLIN_ENVIRONMENT_HDR:FILEPATH=${MERLIN_ENVIRONMENT_HDR}"
      -P "${PROJECT_SOURCE_DIR}/cmake/write-shader-manifest.cmake"
    DEPENDS
      "${MERLIN_VERTEX_SPV}" "${MERLIN_FRAGMENT_SPV}"
      "${MERLIN_BINDLESS_VERTEX_SPV}" "${MERLIN_BINDLESS_FRAGMENT_SPV}"
      "${MERLIN_GPU_SCENE_VERTEX_SPV}" "${MERLIN_GPU_SCENE_FRAGMENT_SPV}"
      "${MERLIN_GPU_DRIVEN_INDEXED_COMPUTE_SPV}"
      "${MERLIN_GPU_DRIVEN_VERTEX_SPV}" "${MERLIN_GPU_DRIVEN_FRAGMENT_SPV}"
      "${MERLIN_GAUSSIAN_PREPARE_COMPUTE_SPV}"
      "${MERLIN_GAUSSIAN_SORT_KEYS_COMPUTE_SPV}"
      "${MERLIN_GAUSSIAN_SORT_HISTOGRAM_COMPUTE_SPV}"
      "${MERLIN_GAUSSIAN_SORT_SCAN_COMPUTE_SPV}"
      "${MERLIN_GAUSSIAN_SORT_SCAN_ADD_COMPUTE_SPV}"
      "${MERLIN_GAUSSIAN_SORT_SCATTER_COMPUTE_SPV}"
      "${MERLIN_GAUSSIAN_SORT_VERIFY_COMPUTE_SPV}"
      "${MERLIN_GAUSSIAN_VERTEX_SPV}" "${MERLIN_GAUSSIAN_FRAGMENT_SPV}"
      "${MERLIN_GAUSSIAN_ID_VERTEX_SPV}"
      "${MERLIN_GAUSSIAN_ID_FRAGMENT_SPV}"
      "${MERLIN_VERTEX_METAL}" "${MERLIN_FRAGMENT_METAL}"
      ${_merlin_shader_reflections}
      "${_merlin_shader_forward}" "${_merlin_shader_bindless}"
      "${_merlin_shader_gpu_scene}" "${_merlin_gpu_scene_abi}"
      "${_merlin_shader_gpu_driven_indexed}"
      "${_merlin_shader_gpu_driven_forward}"
      "${_merlin_shader_gaussian_prepare}"
      "${_merlin_shader_gaussian_sort}"
      "${_merlin_shader_gaussian}"
      "${_merlin_shader_common}"
      "${MERLIN_ENVIRONMENT_HDR}"
      "${_merlin_shader_records_file}"
      "${PROJECT_SOURCE_DIR}/cmake/write-shader-manifest.cmake"
    COMMENT "Writing deterministic shader artifact manifest"
    VERBATIM
  )

  add_custom_target(merlin-vulkan-shaders
    DEPENDS
      "${MERLIN_VERTEX_SPV}"
      "${MERLIN_FRAGMENT_SPV}"
      "${MERLIN_BINDLESS_VERTEX_SPV}"
      "${MERLIN_BINDLESS_FRAGMENT_SPV}"
      "${MERLIN_GPU_SCENE_VERTEX_SPV}"
      "${MERLIN_GPU_SCENE_FRAGMENT_SPV}"
      "${MERLIN_GPU_DRIVEN_INDEXED_COMPUTE_SPV}"
      "${MERLIN_GPU_DRIVEN_VERTEX_SPV}"
      "${MERLIN_GPU_DRIVEN_FRAGMENT_SPV}"
      "${MERLIN_GAUSSIAN_PREPARE_COMPUTE_SPV}"
      "${MERLIN_GAUSSIAN_SORT_KEYS_COMPUTE_SPV}"
      "${MERLIN_GAUSSIAN_SORT_HISTOGRAM_COMPUTE_SPV}"
      "${MERLIN_GAUSSIAN_SORT_SCAN_COMPUTE_SPV}"
      "${MERLIN_GAUSSIAN_SORT_SCAN_ADD_COMPUTE_SPV}"
      "${MERLIN_GAUSSIAN_SORT_SCATTER_COMPUTE_SPV}"
      "${MERLIN_GAUSSIAN_SORT_VERIFY_COMPUTE_SPV}"
      "${MERLIN_GAUSSIAN_VERTEX_SPV}"
      "${MERLIN_GAUSSIAN_ID_VERTEX_SPV}"
      "${MERLIN_GAUSSIAN_FRAGMENT_SPV}"
      "${MERLIN_GAUSSIAN_ID_FRAGMENT_SPV}"
      "${MERLIN_VERTEX_METAL}"
      "${MERLIN_FRAGMENT_METAL}"
      ${_merlin_shader_reflections}
      "${MERLIN_SHADER_MANIFEST}"
  )
  add_dependencies(merlin-vulkan merlin-vulkan-shaders)

  set(_merlin_shader_package_files
    "${MERLIN_VERTEX_SPV}"
    "${MERLIN_FRAGMENT_SPV}"
    "${MERLIN_BINDLESS_VERTEX_SPV}"
    "${MERLIN_BINDLESS_FRAGMENT_SPV}"
    "${MERLIN_GPU_SCENE_VERTEX_SPV}"
    "${MERLIN_GPU_SCENE_FRAGMENT_SPV}"
    "${MERLIN_GPU_DRIVEN_INDEXED_COMPUTE_SPV}"
    "${MERLIN_GPU_DRIVEN_VERTEX_SPV}"
    "${MERLIN_GPU_DRIVEN_FRAGMENT_SPV}"
    "${MERLIN_GAUSSIAN_PREPARE_COMPUTE_SPV}"
    "${MERLIN_GAUSSIAN_SORT_KEYS_COMPUTE_SPV}"
    "${MERLIN_GAUSSIAN_SORT_HISTOGRAM_COMPUTE_SPV}"
    "${MERLIN_GAUSSIAN_SORT_SCAN_COMPUTE_SPV}"
    "${MERLIN_GAUSSIAN_SORT_SCAN_ADD_COMPUTE_SPV}"
    "${MERLIN_GAUSSIAN_SORT_SCATTER_COMPUTE_SPV}"
    "${MERLIN_GAUSSIAN_SORT_VERIFY_COMPUTE_SPV}"
    "${MERLIN_GAUSSIAN_VERTEX_SPV}"
    "${MERLIN_GAUSSIAN_ID_VERTEX_SPV}"
    "${MERLIN_GAUSSIAN_FRAGMENT_SPV}"
    "${MERLIN_GAUSSIAN_ID_FRAGMENT_SPV}"
    "${MERLIN_VERTEX_METAL}"
    "${MERLIN_FRAGMENT_METAL}"
    ${_merlin_shader_reflections}
    "${MERLIN_SHADER_MANIFEST}"
    "${MERLIN_ENVIRONMENT_HDR}"
  )
  install(FILES ${_merlin_shader_package_files}
    DESTINATION
      "${CMAKE_INSTALL_BINDIR}/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}")

  set(MERLIN_VERTEX_SPV "${MERLIN_VERTEX_SPV}" PARENT_SCOPE)
  set(MERLIN_FRAGMENT_SPV "${MERLIN_FRAGMENT_SPV}" PARENT_SCOPE)
  set(MERLIN_BINDLESS_VERTEX_SPV "${MERLIN_BINDLESS_VERTEX_SPV}" PARENT_SCOPE)
  set(MERLIN_BINDLESS_FRAGMENT_SPV
      "${MERLIN_BINDLESS_FRAGMENT_SPV}" PARENT_SCOPE)
  set(MERLIN_GPU_SCENE_VERTEX_SPV
      "${MERLIN_GPU_SCENE_VERTEX_SPV}" PARENT_SCOPE)
  set(MERLIN_GPU_SCENE_FRAGMENT_SPV
      "${MERLIN_GPU_SCENE_FRAGMENT_SPV}" PARENT_SCOPE)
  set(MERLIN_GPU_DRIVEN_INDEXED_COMPUTE_SPV
      "${MERLIN_GPU_DRIVEN_INDEXED_COMPUTE_SPV}" PARENT_SCOPE)
  set(MERLIN_GPU_DRIVEN_VERTEX_SPV
      "${MERLIN_GPU_DRIVEN_VERTEX_SPV}" PARENT_SCOPE)
  set(MERLIN_GPU_DRIVEN_FRAGMENT_SPV
      "${MERLIN_GPU_DRIVEN_FRAGMENT_SPV}" PARENT_SCOPE)
  set(MERLIN_GAUSSIAN_PREPARE_COMPUTE_SPV
      "${MERLIN_GAUSSIAN_PREPARE_COMPUTE_SPV}" PARENT_SCOPE)
  set(MERLIN_GAUSSIAN_SORT_KEYS_COMPUTE_SPV
      "${MERLIN_GAUSSIAN_SORT_KEYS_COMPUTE_SPV}" PARENT_SCOPE)
  set(MERLIN_GAUSSIAN_SORT_HISTOGRAM_COMPUTE_SPV
      "${MERLIN_GAUSSIAN_SORT_HISTOGRAM_COMPUTE_SPV}" PARENT_SCOPE)
  set(MERLIN_GAUSSIAN_SORT_SCAN_COMPUTE_SPV
      "${MERLIN_GAUSSIAN_SORT_SCAN_COMPUTE_SPV}" PARENT_SCOPE)
  set(MERLIN_GAUSSIAN_SORT_SCAN_ADD_COMPUTE_SPV
      "${MERLIN_GAUSSIAN_SORT_SCAN_ADD_COMPUTE_SPV}" PARENT_SCOPE)
  set(MERLIN_GAUSSIAN_SORT_SCATTER_COMPUTE_SPV
      "${MERLIN_GAUSSIAN_SORT_SCATTER_COMPUTE_SPV}" PARENT_SCOPE)
  set(MERLIN_GAUSSIAN_SORT_VERIFY_COMPUTE_SPV
      "${MERLIN_GAUSSIAN_SORT_VERIFY_COMPUTE_SPV}" PARENT_SCOPE)
  set(MERLIN_GAUSSIAN_VERTEX_SPV
      "${MERLIN_GAUSSIAN_VERTEX_SPV}" PARENT_SCOPE)
  set(MERLIN_GAUSSIAN_ID_VERTEX_SPV
      "${MERLIN_GAUSSIAN_ID_VERTEX_SPV}" PARENT_SCOPE)
  set(MERLIN_GAUSSIAN_FRAGMENT_SPV
      "${MERLIN_GAUSSIAN_FRAGMENT_SPV}" PARENT_SCOPE)
  set(MERLIN_GAUSSIAN_ID_FRAGMENT_SPV
      "${MERLIN_GAUSSIAN_ID_FRAGMENT_SPV}" PARENT_SCOPE)
  set(MERLIN_VERTEX_METAL "${MERLIN_VERTEX_METAL}" PARENT_SCOPE)
  set(MERLIN_FRAGMENT_METAL "${MERLIN_FRAGMENT_METAL}" PARENT_SCOPE)
  set(MERLIN_SHADER_REFLECTIONS "${_merlin_shader_reflections}" PARENT_SCOPE)
  set(MERLIN_SHADER_MANIFEST "${MERLIN_SHADER_MANIFEST}" PARENT_SCOPE)
  set(MERLIN_SHADER_PACKAGE_FILES
      "${_merlin_shader_package_files}" PARENT_SCOPE)
  set(MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION
      "${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}" PARENT_SCOPE)
endmacro()

macro(merlin_add_materialx_shader_artifacts)
  set(MERLIN_MATERIALX_ARTIFACTS_ENABLED OFF)
  if(MERLIN_ENABLE_MATERIALX)
    if(MERLIN_SLANGC_EXECUTABLE)
      set(MERLIN_MATERIALX_SLANGC_EXECUTABLE
          "${MERLIN_SLANGC_EXECUTABLE}")
    else()
      find_program(MERLIN_MATERIALX_SLANGC_EXECUTABLE
        NAMES slangc
        HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
        DOC "Slang compiler used for retained MaterialX target artifacts"
      )
    endif()
    if(MERLIN_MATERIALX_SLANGC_EXECUTABLE)
      include(cmake/package-materialx-artifacts.cmake)
    else()
      message(STATUS
        "slangc was not found; retained MaterialX target artifacts are disabled")
    endif()
  endif()
endmacro()
