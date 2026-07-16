add_executable(merlin-headless
  src/main.cpp
)

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

if(MERLIN_GENERATE_RENDERER_REPORT AND NOT CMAKE_CROSSCOMPILING)
  add_custom_command(TARGET merlin-headless POST_BUILD
    COMMAND "$<TARGET_FILE:merlin-headless>"
            --report "${CMAKE_BINARY_DIR}/renderer-report.json"
            --output "${CMAKE_BINARY_DIR}/renderer-smoke.ppm"
    COMMENT "Writing OpenStrata renderer-report.json"
    VERBATIM
  )
endif()

install(TARGETS merlin-headless
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
