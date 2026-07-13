cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED MERLIN_RELEASE_VERSION OR
   NOT MERLIN_RELEASE_VERSION MATCHES "^[0-9]+[.][0-9]+[.][0-9]+$")
  message(FATAL_ERROR
    "MERLIN_RELEASE_VERSION must be stable SemVer (MAJOR.MINOR.PATCH)")
endif()

if(NOT DEFINED MERLIN_SOURCE_DIR)
  get_filename_component(MERLIN_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()
if(NOT DEFINED MERLIN_RELEASE_DATE)
  string(TIMESTAMP MERLIN_RELEASE_DATE "%Y-%m-%d" UTC)
endif()
if(NOT MERLIN_RELEASE_DATE MATCHES "^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]$")
  message(FATAL_ERROR "MERLIN_RELEASE_DATE must use YYYY-MM-DD")
endif()

set(_merlin_version_file "${MERLIN_SOURCE_DIR}/VERSION")
set(_merlin_changelog_file "${MERLIN_SOURCE_DIR}/CHANGELOG.md")
set(_merlin_openstrata_file "${MERLIN_SOURCE_DIR}/openstrata.toml")
if(NOT EXISTS "${_merlin_version_file}" OR
   NOT EXISTS "${_merlin_changelog_file}" OR
   NOT EXISTS "${_merlin_openstrata_file}")
  message(FATAL_ERROR
    "MERLIN_SOURCE_DIR must contain VERSION, CHANGELOG.md, and openstrata.toml")
endif()

file(READ "${_merlin_version_file}" _merlin_current_version)
string(STRIP "${_merlin_current_version}" _merlin_current_version)
if(NOT _merlin_current_version MATCHES
   "^[0-9]+[.][0-9]+[.][0-9]+$")
  message(FATAL_ERROR "VERSION does not contain stable SemVer")
endif()
if(MERLIN_RELEASE_VERSION VERSION_LESS _merlin_current_version)
  message(FATAL_ERROR
    "release ${MERLIN_RELEASE_VERSION} cannot go backwards from VERSION ${_merlin_current_version}")
endif()

file(READ "${_merlin_openstrata_file}" _merlin_openstrata)
string(REGEX MATCHALL
  "version[ \t]*=[ \t]*\"[0-9]+[.][0-9]+[.][0-9]+\""
  _merlin_openstrata_version_entries "${_merlin_openstrata}")
list(LENGTH _merlin_openstrata_version_entries
  _merlin_openstrata_version_entry_count)
if(NOT _merlin_openstrata_version_entry_count EQUAL 1)
  message(FATAL_ERROR
    "openstrata.toml must contain exactly one stable project version")
endif()
string(REGEX MATCH
  "version[ \t]*=[ \t]*\"([0-9]+[.][0-9]+[.][0-9]+)\""
  _merlin_openstrata_version_match "${_merlin_openstrata}")
if(NOT "${CMAKE_MATCH_1}" STREQUAL "${_merlin_current_version}")
  message(FATAL_ERROR
    "openstrata.toml version ${CMAKE_MATCH_1} does not match VERSION ${_merlin_current_version}")
endif()
string(REGEX REPLACE
  "version[ \t]*=[ \t]*\"[0-9]+[.][0-9]+[.][0-9]+\""
  "version = \"${MERLIN_RELEASE_VERSION}\""
  _merlin_openstrata "${_merlin_openstrata}")

file(READ "${_merlin_changelog_file}" _merlin_changelog)
string(REPLACE "\r\n" "\n" _merlin_changelog "${_merlin_changelog}")

set(_merlin_unreleased_heading "## [Unreleased]\n")
string(FIND "${_merlin_changelog}" "${_merlin_unreleased_heading}"
  _merlin_unreleased_position)
if(_merlin_unreleased_position LESS 0)
  message(FATAL_ERROR "CHANGELOG.md has no Unreleased section")
endif()

string(LENGTH "${_merlin_unreleased_heading}" _merlin_heading_length)
math(EXPR _merlin_unreleased_body_position
  "${_merlin_unreleased_position} + ${_merlin_heading_length}")
string(SUBSTRING "${_merlin_changelog}"
  ${_merlin_unreleased_body_position} -1 _merlin_after_unreleased)
string(FIND "${_merlin_after_unreleased}" "\n## ["
  _merlin_next_section_position)
if(_merlin_next_section_position LESS 0)
  message(FATAL_ERROR "CHANGELOG.md has no released section after Unreleased")
endif()
string(SUBSTRING "${_merlin_after_unreleased}" 0
  ${_merlin_next_section_position} _merlin_unreleased_body)
if(NOT _merlin_unreleased_body MATCHES "### [A-Za-z]+")
  message(FATAL_ERROR "CHANGELOG.md Unreleased section has no release notes")
endif()

string(REGEX MATCH
  "## \\[([0-9]+[.][0-9]+[.][0-9]+)\\] - [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]"
  _merlin_latest_release "${_merlin_changelog}")
if(NOT _merlin_latest_release)
  message(FATAL_ERROR "CHANGELOG.md has no previous stable release")
endif()
set(_merlin_previous_version "${CMAKE_MATCH_1}")
if(NOT MERLIN_RELEASE_VERSION VERSION_GREATER _merlin_previous_version)
  message(FATAL_ERROR
    "release ${MERLIN_RELEASE_VERSION} must be newer than ${_merlin_previous_version}")
endif()

set(_merlin_release_heading "## [${MERLIN_RELEASE_VERSION}] - ${MERLIN_RELEASE_DATE}")
string(FIND "${_merlin_changelog}" "${_merlin_release_heading}"
  _merlin_existing_release)
if(NOT _merlin_existing_release LESS 0)
  message(FATAL_ERROR "CHANGELOG.md already contains ${_merlin_release_heading}")
endif()

set(_merlin_compare_root
  "https://github.com/animu-sphere/hydra-merlin/compare")
set(_merlin_old_unreleased_link
  "[Unreleased]: ${_merlin_compare_root}/v${_merlin_previous_version}...main")
string(FIND "${_merlin_changelog}" "${_merlin_old_unreleased_link}"
  _merlin_link_position)
if(_merlin_link_position LESS 0)
  message(FATAL_ERROR
    "CHANGELOG.md Unreleased comparison link does not start at v${_merlin_previous_version}")
endif()

string(REPLACE "${_merlin_unreleased_heading}"
  "${_merlin_unreleased_heading}\n${_merlin_release_heading}\n"
  _merlin_changelog "${_merlin_changelog}")
set(_merlin_new_unreleased_link
  "[Unreleased]: ${_merlin_compare_root}/v${MERLIN_RELEASE_VERSION}...main")
set(_merlin_release_link
  "[${MERLIN_RELEASE_VERSION}]: ${_merlin_compare_root}/v${_merlin_previous_version}...v${MERLIN_RELEASE_VERSION}")
string(REPLACE "${_merlin_old_unreleased_link}"
  "${_merlin_new_unreleased_link}\n${_merlin_release_link}"
  _merlin_changelog "${_merlin_changelog}")

if(MERLIN_DRY_RUN)
  message(STATUS
    "Would prepare hdMerlin ${MERLIN_RELEASE_VERSION} (${MERLIN_RELEASE_DATE}) from ${_merlin_previous_version}")
else()
  file(WRITE "${_merlin_version_file}" "${MERLIN_RELEASE_VERSION}\n")
  file(WRITE "${_merlin_openstrata_file}" "${_merlin_openstrata}")
  file(WRITE "${_merlin_changelog_file}" "${_merlin_changelog}")
  message(STATUS
    "Prepared hdMerlin ${MERLIN_RELEASE_VERSION} (${MERLIN_RELEASE_DATE}) from ${_merlin_previous_version}")
endif()
