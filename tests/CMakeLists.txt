add_executable(merlin-render-world-tests
  render_world_test.cpp
)

target_link_libraries(merlin-render-world-tests PRIVATE Merlin::RenderWorld)

add_test(NAME merlin-render-world COMMAND merlin-render-world-tests)

if(TARGET merlin-headless)
  add_test(
    NAME merlin-vulkan-offscreen
    COMMAND merlin-headless
            --width 64
            --height 64
            --output "${CMAKE_CURRENT_BINARY_DIR}/merlin-smoke.ppm"
  )
endif()
