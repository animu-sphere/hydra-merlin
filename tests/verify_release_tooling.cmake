if(NOT DEFINED MERLIN_SOURCE_DIR OR
   NOT DEFINED MERLIN_TEST_BINARY_DIR OR
   NOT DEFINED MERLIN_CMAKE_COMMAND)
  message(FATAL_ERROR "Missing release-tooling test arguments")
endif()

set(_fixture "${MERLIN_TEST_BINARY_DIR}/release-tooling-fixture")
set(_fixture_version "9.8.7")
file(REMOVE_RECURSE "${_fixture}")
file(MAKE_DIRECTORY "${_fixture}")
file(WRITE "${_fixture}/VERSION" "9.8.7\n")
file(WRITE "${_fixture}/openstrata.toml"
  "[project]\nname = \"hdMerlin\"\nversion = \"9.8.7\"\n\n"
  "[requires]\nplatform = \"cy2026\"\nprofile = \"core\"\n")
file(WRITE "${_fixture}/CHANGELOG.md"
  "# Changelog\n\n"
  "## [Unreleased]\n\n"
  "### Added\n\n"
  "- Release tooling fixture.\n\n"
  "## [9.8.6] - 2098-12-31\n\n"
  "### Added\n\n"
  "- Previous fixture.\n\n"
  "[Unreleased]: https://github.com/animu-sphere/hydra-merlin/compare/v9.8.6...main\n"
  "[9.8.6]: https://github.com/animu-sphere/hydra-merlin/releases/tag/v9.8.6\n")

execute_process(
  COMMAND "${MERLIN_CMAKE_COMMAND}"
    "-DMERLIN_RELEASE_VERSION:STRING=${_fixture_version}"
    "-DMERLIN_RELEASE_DATE:STRING=2099-01-02"
    "-DMERLIN_SOURCE_DIR:PATH=${_fixture}"
    -P "${MERLIN_SOURCE_DIR}/cmake/prepare-release.cmake"
  RESULT_VARIABLE _prepare_result
)
if(NOT _prepare_result EQUAL 0)
  message(FATAL_ERROR "prepare-release fixture failed: ${_prepare_result}")
endif()

file(READ "${_fixture}/VERSION" _prepared_version)
string(STRIP "${_prepared_version}" _prepared_version)
if(NOT "${_prepared_version}" STREQUAL "${_fixture_version}")
  message(FATAL_ERROR "prepare-release wrote ${_prepared_version}")
endif()
file(READ "${_fixture}/openstrata.toml" _prepared_openstrata)
if(NOT _prepared_openstrata MATCHES
   "version[ \t]*=[ \t]*\"${_fixture_version}\"")
  message(FATAL_ERROR "prepare-release did not update openstrata.toml")
endif()
file(READ "${_fixture}/CHANGELOG.md" _prepared_changelog)
if(NOT _prepared_changelog MATCHES
   "## \\[${_fixture_version}\\] - 2099-01-02")
  message(FATAL_ERROR "prepare-release did not create the stable heading")
endif()

execute_process(
  COMMAND "${MERLIN_CMAKE_COMMAND}"
    "-DMERLIN_RELEASE_TAG:STRING=v${_fixture_version}"
    "-DMERLIN_SOURCE_DIR:PATH=${_fixture}"
    -P "${MERLIN_SOURCE_DIR}/cmake/verify-release-tag.cmake"
  RESULT_VARIABLE _verify_result
)
if(NOT _verify_result EQUAL 0)
  message(FATAL_ERROR "prepared release contract failed: ${_verify_result}")
endif()

execute_process(
  COMMAND "${MERLIN_CMAKE_COMMAND}"
    "-DMERLIN_RELEASE_TAG:STRING=v99.99.99"
    "-DMERLIN_SOURCE_DIR:PATH=${_fixture}"
    -P "${MERLIN_SOURCE_DIR}/cmake/verify-release-tag.cmake"
  RESULT_VARIABLE _mismatch_result
  OUTPUT_QUIET ERROR_QUIET
)
if(_mismatch_result EQUAL 0)
  message(FATAL_ERROR "mismatched release tag was accepted")
endif()
