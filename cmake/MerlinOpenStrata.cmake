include_guard(GLOBAL)

function(merlin_validate_openstrata_metadata)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/openstrata.toml")
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/openstrata.toml"
    _merlin_openstrata_project)
  string(REGEX MATCH
    "name[ \t]*=[ \t]*\"([^\"]+)\""
    _merlin_openstrata_name_match "${_merlin_openstrata_project}")
  set(_merlin_openstrata_name "${CMAKE_MATCH_1}")
  string(REGEX MATCH
    "version[ \t]*=[ \t]*\"([0-9]+[.][0-9]+[.][0-9]+)\""
    _merlin_openstrata_version_match "${_merlin_openstrata_project}")
  set(_merlin_openstrata_version "${CMAKE_MATCH_1}")
  if(NOT _merlin_openstrata_name STREQUAL "hdMerlin" OR
     NOT _merlin_openstrata_version STREQUAL MERLIN_PROJECT_VERSION)
    message(FATAL_ERROR
      "openstrata.toml project identity must be hdMerlin ${MERLIN_PROJECT_VERSION}")
  endif()
endfunction()

# Call from the directory that owns merlin-headless (POST_BUILD scope).
function(merlin_add_renderer_report)
  if(MERLIN_GENERATE_RENDERER_REPORT AND NOT CMAKE_CROSSCOMPILING)
    add_custom_command(TARGET merlin-headless POST_BUILD
      COMMAND "$<TARGET_FILE:merlin-headless>"
              --report "${CMAKE_BINARY_DIR}/renderer-report.json"
              --output "${CMAKE_BINARY_DIR}/renderer-smoke.ppm"
      COMMENT "Writing OpenStrata renderer-report.json"
      VERBATIM
    )
  endif()
endfunction()
