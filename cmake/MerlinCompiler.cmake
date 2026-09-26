include_guard(GLOBAL)

add_library(merlin-compiler-options INTERFACE)
if(MSVC)
  target_compile_options(merlin-compiler-options INTERFACE /W4 /permissive- /EHsc)
else()
  target_compile_options(merlin-compiler-options INTERFACE -Wall -Wextra -Wpedantic)
endif()

# PRIVATE alone still records static-library dependencies in install exports.
# BUILD_INTERFACE keeps this development policy out of the installed SDK.
function(merlin_target_defaults target)
  target_link_libraries(${target} PRIVATE
    $<BUILD_INTERFACE:merlin-compiler-options>)
  set_target_properties(${target} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
  )
endfunction()
