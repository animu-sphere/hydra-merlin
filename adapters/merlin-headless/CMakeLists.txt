add_executable(merlin-headless
  src/main.cpp
)

target_link_libraries(merlin-headless
  PRIVATE
    Merlin::RenderWorld
    Merlin::Vulkan
)

add_dependencies(merlin-headless merlin-vulkan-shaders)

add_custom_command(TARGET merlin-headless POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "$<TARGET_FILE_DIR:merlin-headless>/shaders"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${MERLIN_VERTEX_SPV}"
          "$<TARGET_FILE_DIR:merlin-headless>/shaders/triangle.vert.spv"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${MERLIN_FRAGMENT_SPV}"
          "$<TARGET_FILE_DIR:merlin-headless>/shaders/triangle.frag.spv"
  VERBATIM
)

install(TARGETS merlin-headless
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(FILES "${MERLIN_VERTEX_SPV}" "${MERLIN_FRAGMENT_SPV}"
  DESTINATION "${CMAKE_INSTALL_BINDIR}/shaders"
)
