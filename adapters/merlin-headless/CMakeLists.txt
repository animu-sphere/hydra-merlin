add_executable(merlin-headless
  src/main.cpp
)
merlin_target_defaults(merlin-headless)

target_link_libraries(merlin-headless
  PRIVATE
    Merlin::RenderWorld
    Merlin::Vulkan
)

add_dependencies(merlin-headless merlin-vulkan-shaders)

add_custom_target(merlin-headless-shader-package
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "$<TARGET_FILE_DIR:merlin-headless>/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${MERLIN_SHADER_PACKAGE_FILES}
          "$<TARGET_FILE_DIR:merlin-headless>/shaders/v${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}"
  DEPENDS ${MERLIN_SHADER_PACKAGE_FILES}
  VERBATIM
)
add_dependencies(merlin-headless-shader-package merlin-vulkan-shaders)
add_dependencies(merlin-headless merlin-headless-shader-package)

merlin_add_renderer_report()

install(TARGETS merlin-headless
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
