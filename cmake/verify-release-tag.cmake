if(NOT DEFINED MERLIN_RELEASE_TAG OR NOT DEFINED MERLIN_SOURCE_DIR)
  message(FATAL_ERROR "MERLIN_RELEASE_TAG and MERLIN_SOURCE_DIR are required")
endif()

if(NOT "${MERLIN_RELEASE_TAG}" MATCHES "^v([0-9]+[.][0-9]+[.][0-9]+)$")
  message(FATAL_ERROR
    "release tags must use the stable SemVer form vMAJOR.MINOR.PATCH")
endif()
set(_merlin_tag_version "${CMAKE_MATCH_1}")

file(READ "${MERLIN_SOURCE_DIR}/CMakeLists.txt" _merlin_root_cmake)
string(REGEX MATCH
  "project[(]hdMerlin[ \t\r\n]+VERSION[ \t\r\n]+([0-9]+[.][0-9]+[.][0-9]+)"
  _merlin_project_match "${_merlin_root_cmake}")
if(NOT _merlin_project_match)
  message(FATAL_ERROR "could not read the hdMerlin project version")
endif()

if(NOT "${_merlin_tag_version}" VERSION_EQUAL "${CMAKE_MATCH_1}")
  message(FATAL_ERROR
    "release tag ${MERLIN_RELEASE_TAG} does not match project version ${CMAKE_MATCH_1}")
endif()
