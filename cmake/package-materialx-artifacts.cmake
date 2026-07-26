# Retain the accepted cross-target MaterialX slice as installable build
# evidence. This is product packaging, not a test fixture: callers include this
# file whenever MaterialX and a compatible slangc are available.

if(NOT TARGET merlin-materialx-artifact-generator)
  message(FATAL_ERROR
    "MaterialX artifact packaging requires the internal generator target")
endif()
if(NOT MERLIN_MATERIALX_SLANGC_EXECUTABLE)
  message(FATAL_ERROR "MaterialX artifact packaging requires slangc")
endif()

set(_merlin_materialx_artifact_source_dir
    "${PROJECT_SOURCE_DIR}/material/merlin-materialx/artifacts")
set(_merlin_materialx_prototype_document
    "${_merlin_materialx_artifact_source_dir}/prototype.mtlx")
set(_merlin_materialx_standard_document
    "${_merlin_materialx_artifact_source_dir}/standard-surface.mtlx")
set(_merlin_materialx_prototype_wrapper
    "${_merlin_materialx_artifact_source_dir}/compile-wrapper.slang")
set(_merlin_materialx_standard_wrapper
    "${_merlin_materialx_artifact_source_dir}/standard-surface-wrapper.slang")

set(_merlin_materialx_package
    "${CMAKE_BINARY_DIR}/generated-material-artifacts/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}/materialx")
set(_merlin_packaged_prototype
    "${_merlin_materialx_package}/materialx-prototype.slang")
set(_merlin_packaged_standard_surface
    "${_merlin_materialx_package}/materialx-standard-surface.slang")
add_custom_command(
  OUTPUT
    "${_merlin_packaged_prototype}"
    "${_merlin_packaged_standard_surface}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
    "${_merlin_materialx_package}"
  COMMAND "$<TARGET_FILE:merlin-materialx-artifact-generator>"
    "${_merlin_materialx_data_root}"
    "${_merlin_materialx_prototype_document}"
    "${_merlin_packaged_prototype}"
    "${_merlin_materialx_standard_document}"
    "${_merlin_packaged_standard_surface}"
  DEPENDS
    merlin-materialx-artifact-generator
    "${_merlin_materialx_prototype_document}"
    "${_merlin_materialx_standard_document}"
  COMMENT "Generating retained MaterialX material modules"
  VERBATIM
)

set(_merlin_packaged_prototype_spv
    "${_merlin_materialx_package}/materialx-prototype.spv")
set(_merlin_packaged_prototype_metal
    "${_merlin_materialx_package}/materialx-prototype.metal")
set(_merlin_packaged_standard_spv
    "${_merlin_materialx_package}/materialx-standard-surface.spv")
set(_merlin_packaged_standard_metal
    "${_merlin_materialx_package}/materialx-standard-surface.metal")
add_custom_command(
  OUTPUT
    "${_merlin_packaged_prototype_spv}"
    "${_merlin_packaged_prototype_spv}.reflection.json"
  COMMAND "${MERLIN_MATERIALX_SLANGC_EXECUTABLE}"
    "${_merlin_materialx_prototype_wrapper}"
    -I "${_merlin_materialx_package}"
    -entry merlin_materialx_test_fragment -stage fragment
    -target spirv -profile sm_6_6 -capability spirv_1_5
    -matrix-layout-column-major -O2 -warnings-as-errors all
    -reflection-json
      "${_merlin_packaged_prototype_spv}.reflection.json"
    -o "${_merlin_packaged_prototype_spv}"
  DEPENDS
    "${_merlin_packaged_prototype}"
    "${_merlin_materialx_prototype_wrapper}"
    "${MERLIN_MATERIALX_SLANGC_EXECUTABLE}"
  COMMENT "Packaging MaterialX prototype SPIR-V evidence"
  VERBATIM
)
add_custom_command(
  OUTPUT
    "${_merlin_packaged_prototype_metal}"
    "${_merlin_packaged_prototype_metal}.reflection.json"
  COMMAND "${MERLIN_MATERIALX_SLANGC_EXECUTABLE}"
    "${_merlin_materialx_prototype_wrapper}"
    -I "${_merlin_materialx_package}"
    -entry merlin_materialx_test_fragment -stage fragment
    -target metal -profile metallib_2_4
    -matrix-layout-column-major -O2 -warnings-as-errors all
    -reflection-json
      "${_merlin_packaged_prototype_metal}.reflection.json"
    -o "${_merlin_packaged_prototype_metal}"
  DEPENDS
    "${_merlin_packaged_prototype}"
    "${_merlin_materialx_prototype_wrapper}"
    "${MERLIN_MATERIALX_SLANGC_EXECUTABLE}"
  COMMENT "Packaging MaterialX prototype Metal evidence"
  VERBATIM
)
add_custom_command(
  OUTPUT
    "${_merlin_packaged_standard_spv}"
    "${_merlin_packaged_standard_spv}.reflection.json"
  COMMAND "${MERLIN_MATERIALX_SLANGC_EXECUTABLE}"
    "${_merlin_materialx_standard_wrapper}"
    -I "${_merlin_materialx_package}"
    -entry merlin_materialx_standard_surface_test_fragment
    -stage fragment -target spirv -profile sm_6_6
    -capability spirv_1_5
    -matrix-layout-column-major -O2 -warnings-as-errors all
    -reflection-json
      "${_merlin_packaged_standard_spv}.reflection.json"
    -o "${_merlin_packaged_standard_spv}"
  DEPENDS
    "${_merlin_packaged_standard_surface}"
    "${_merlin_materialx_standard_wrapper}"
    "${MERLIN_MATERIALX_SLANGC_EXECUTABLE}"
  COMMENT "Packaging MaterialX Standard Surface SPIR-V evidence"
  VERBATIM
)
add_custom_command(
  OUTPUT
    "${_merlin_packaged_standard_metal}"
    "${_merlin_packaged_standard_metal}.reflection.json"
  COMMAND "${MERLIN_MATERIALX_SLANGC_EXECUTABLE}"
    "${_merlin_materialx_standard_wrapper}"
    -I "${_merlin_materialx_package}"
    -entry merlin_materialx_standard_surface_test_fragment
    -stage fragment -target metal -profile metallib_2_4
    -matrix-layout-column-major -O2 -warnings-as-errors all
    -reflection-json
      "${_merlin_packaged_standard_metal}.reflection.json"
    -o "${_merlin_packaged_standard_metal}"
  DEPENDS
    "${_merlin_packaged_standard_surface}"
    "${_merlin_materialx_standard_wrapper}"
    "${MERLIN_MATERIALX_SLANGC_EXECUTABLE}"
  COMMENT "Packaging MaterialX Standard Surface Metal evidence"
  VERBATIM
)

add_custom_target(merlin-materialx-retained-artifacts ALL
  DEPENDS
    "${_merlin_packaged_prototype_spv}"
    "${_merlin_packaged_prototype_spv}.reflection.json"
    "${_merlin_packaged_prototype_metal}"
    "${_merlin_packaged_prototype_metal}.reflection.json"
    "${_merlin_packaged_standard_spv}"
    "${_merlin_packaged_standard_spv}.reflection.json"
    "${_merlin_packaged_standard_metal}"
    "${_merlin_packaged_standard_metal}.reflection.json"
)
install(DIRECTORY "${_merlin_materialx_package}/"
  DESTINATION
    "${CMAKE_INSTALL_DATADIR}/merlin/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}/materialx"
)
set(MERLIN_MATERIALX_ARTIFACTS_ENABLED ON)
