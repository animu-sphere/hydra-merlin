find_package(pxr CONFIG REQUIRED)

add_library(hdMerlin MODULE
  src/renderer_plugin.cpp
)

target_link_libraries(hdMerlin
  PRIVATE
    Merlin::RenderWorld
    Merlin::RenderExtraction
    hd
)

target_compile_features(hdMerlin PRIVATE cxx_std_20)
target_compile_definitions(hdMerlin PRIVATE
  NOMINMAX
  _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING
)
set_target_properties(hdMerlin PROPERTIES PREFIX "")

add_custom_command(TARGET hdMerlin POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "$<TARGET_FILE_DIR:hdMerlin>/resources"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/resources/plugInfo.json"
          "$<TARGET_FILE_DIR:hdMerlin>/resources/plugInfo.json"
  VERBATIM
)

if(MERLIN_BUILD_TESTS)
  add_executable(merlin-hydra2-discovery-test
    tests/plugin_discovery_test.cpp
  )
  target_link_libraries(merlin-hydra2-discovery-test PRIVATE hd plug)
  target_compile_features(merlin-hydra2-discovery-test PRIVATE cxx_std_20)
  target_compile_definitions(merlin-hydra2-discovery-test PRIVATE
    NOMINMAX
    _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING
  )
  add_dependencies(merlin-hydra2-discovery-test hdMerlin)
  add_test(
    NAME merlin-hydra2-discovery
    COMMAND merlin-hydra2-discovery-test
            "$<TARGET_FILE_DIR:hdMerlin>/resources"
  )
  get_filename_component(_merlin_pxr_root "${PXR_INCLUDE_DIRS}/.." ABSOLUTE)
  set_tests_properties(merlin-hydra2-discovery PROPERTIES
    ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:${_merlin_pxr_root}/bin;PATH=path_list_prepend:${_merlin_pxr_root}/lib"
  )
endif()
