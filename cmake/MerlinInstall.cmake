include(CMakePackageConfigHelpers)

set(_merlin_install_cmakedir "${CMAKE_INSTALL_LIBDIR}/cmake/Merlin")

set(_merlin_core_targets
  merlin-render-world
  merlin-render-extraction
  merlin-render-backend
)

install(TARGETS ${_merlin_core_targets}
  EXPORT MerlinTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
if(TARGET merlin-vulkan)
  install(TARGETS merlin-vulkan
    EXPORT MerlinVulkanTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  )
endif()
if(TARGET merlin-metal)
  install(TARGETS merlin-metal
    EXPORT MerlinMetalTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  )
endif()
if(TARGET merlin-materialx)
  install(TARGETS merlin-materialx
    EXPORT MerlinMaterialXTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  )
endif()
install(DIRECTORY core/merlin-render-world/include/
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY core/merlin-render-extraction/include/
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY core/merlin-render-backend/include/
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
if(TARGET merlin-vulkan)
  install(DIRECTORY backend/merlin-vulkan/include/
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
endif()
if(TARGET merlin-metal)
  install(DIRECTORY backend/merlin-metal/include/
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
endif()
if(TARGET merlin-materialx)
  install(DIRECTORY material/merlin-materialx/include/
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
endif()

install(EXPORT MerlinTargets
  FILE MerlinTargets.cmake
  NAMESPACE Merlin::
  DESTINATION "${_merlin_install_cmakedir}"
)
if(TARGET merlin-vulkan)
  install(EXPORT MerlinVulkanTargets
    FILE MerlinVulkanTargets.cmake
    NAMESPACE Merlin::
    DESTINATION "${_merlin_install_cmakedir}"
  )
endif()
if(TARGET merlin-metal)
  install(EXPORT MerlinMetalTargets
    FILE MerlinMetalTargets.cmake
    NAMESPACE Merlin::
    DESTINATION "${_merlin_install_cmakedir}"
  )
endif()
if(TARGET merlin-materialx)
  install(EXPORT MerlinMaterialXTargets
    FILE MerlinMaterialXTargets.cmake
    NAMESPACE Merlin::
    DESTINATION "${_merlin_install_cmakedir}"
  )
endif()

configure_package_config_file(
  cmake/MerlinConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/MerlinConfig.cmake"
  INSTALL_DESTINATION "${_merlin_install_cmakedir}"
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/MerlinConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/MerlinConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/MerlinConfigVersion.cmake"
  DESTINATION "${_merlin_install_cmakedir}"
)
