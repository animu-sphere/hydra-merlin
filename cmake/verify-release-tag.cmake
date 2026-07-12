if(NOT DEFINED MERLIN_RELEASE_TAG OR NOT DEFINED MERLIN_SOURCE_DIR)
  message(FATAL_ERROR "MERLIN_RELEASE_TAG and MERLIN_SOURCE_DIR are required")
endif()

if(NOT "${MERLIN_RELEASE_TAG}" MATCHES "^v([0-9]+[.][0-9]+[.][0-9]+)$")
  message(FATAL_ERROR
    "release tags must use the stable SemVer form vMAJOR.MINOR.PATCH")
endif()
set(_merlin_tag_version "${CMAKE_MATCH_1}")

file(READ "${MERLIN_SOURCE_DIR}/VERSION" _merlin_project_version)
string(STRIP "${_merlin_project_version}" _merlin_project_version)
if(NOT _merlin_project_version MATCHES
   "^[0-9]+[.][0-9]+[.][0-9]+$")
  message(FATAL_ERROR "VERSION does not contain stable SemVer")
endif()

if(NOT "${_merlin_tag_version}" STREQUAL "${_merlin_project_version}")
  message(FATAL_ERROR
    "release tag ${MERLIN_RELEASE_TAG} does not match VERSION ${_merlin_project_version}")
endif()

file(READ "${MERLIN_SOURCE_DIR}/CHANGELOG.md" _merlin_changelog)
if(NOT _merlin_changelog MATCHES
   "## \\[${_merlin_project_version}\\] - [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]")
  message(FATAL_ERROR
    "CHANGELOG.md has no dated ${_merlin_project_version} release section; run scripts/prepare-release.ps1")
endif()
if(NOT _merlin_changelog MATCHES
   "\\[${_merlin_project_version}\\]: https://github.com/animu-sphere/hydra-merlin/compare/v[0-9]+[.][0-9]+[.][0-9]+[.][.][.]v${_merlin_project_version}")
  message(FATAL_ERROR
    "CHANGELOG.md has no ${_merlin_project_version} comparison link")
endif()
