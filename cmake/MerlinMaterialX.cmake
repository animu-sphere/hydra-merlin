find_package(MaterialX ${MERLIN_MATERIALX_MIN_VERSION} CONFIG QUIET)
if(TARGET MaterialXGenSlang)
  set(MERLIN_MATERIALX_DETECTED_VERSION "${MaterialX_VERSION}")
else()
  if(CMAKE_VERSION VERSION_LESS 3.26)
    message(FATAL_ERROR
      "MaterialX ${MERLIN_MATERIALX_MIN_VERSION} source builds require CMake 3.26 or newer")
  endif()

  set(MATERIALX_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
  set(MATERIALX_BUILD_VIEWER OFF CACHE BOOL "" FORCE)
  set(MATERIALX_BUILD_GRAPH_EDITOR OFF CACHE BOOL "" FORCE)
  set(MATERIALX_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(MATERIALX_BUILD_GEN_GLSL OFF CACHE BOOL "" FORCE)
  set(MATERIALX_BUILD_GEN_OSL OFF CACHE BOOL "" FORCE)
  set(MATERIALX_BUILD_GEN_MDL OFF CACHE BOOL "" FORCE)
  set(MATERIALX_BUILD_GEN_MSL OFF CACHE BOOL "" FORCE)
  set(MATERIALX_BUILD_GEN_SLANG ON CACHE BOOL "" FORCE)
  set(MATERIALX_BUILD_RENDER OFF CACHE BOOL "" FORCE)
  set(MATERIALX_BUILD_RENDER_PLATFORMS OFF CACHE BOOL "" FORCE)
  set(MATERIALX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(MATERIALX_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

  if(MERLIN_MATERIALX_SOURCE_DIR)
    cmake_path(ABSOLUTE_PATH MERLIN_MATERIALX_SOURCE_DIR
               NORMALIZE OUTPUT_VARIABLE _merlin_materialx_source)
    if(NOT EXISTS "${_merlin_materialx_source}/source/MaterialXGenSlang/CMakeLists.txt")
      message(FATAL_ERROR
        "MERLIN_MATERIALX_SOURCE_DIR does not contain MaterialXGenSlang: ${_merlin_materialx_source}")
    endif()
    # MaterialX supports embedding, but its package-generation code uses
    # CMAKE_PROJECT_NAME (the top-level project by default). Scope the name
    # so its install exports remain MaterialXConfig/MaterialXTargets.
    set(_merlin_parent_cmake_project_name "${CMAKE_PROJECT_NAME}")
    set(CMAKE_PROJECT_NAME MaterialX)
    add_subdirectory("${_merlin_materialx_source}"
      "${CMAKE_CURRENT_BINARY_DIR}/_deps/materialx-build")
    set(CMAKE_PROJECT_NAME "${_merlin_parent_cmake_project_name}")
    set(_merlin_materialx_data_root "${_merlin_materialx_source}")
  elseif(MERLIN_FETCH_MATERIALX)
    include(FetchContent)
    FetchContent_Declare(merlin_materialx_upstream
      GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/MaterialX.git
      GIT_TAG ${MERLIN_MATERIALX_GENSLANG_REVISION}
      GIT_SHALLOW FALSE
      GIT_PROGRESS TRUE
    )
    set(_merlin_parent_cmake_project_name "${CMAKE_PROJECT_NAME}")
    set(CMAKE_PROJECT_NAME MaterialX)
    FetchContent_MakeAvailable(merlin_materialx_upstream)
    set(CMAKE_PROJECT_NAME "${_merlin_parent_cmake_project_name}")
    set(_merlin_materialx_data_root "${merlin_materialx_upstream_SOURCE_DIR}")
  else()
    message(FATAL_ERROR
      "MERLIN_ENABLE_MATERIALX requires a MaterialX ${MERLIN_MATERIALX_MIN_VERSION}+ package with MaterialXGenSlang, MERLIN_MATERIALX_SOURCE_DIR, or MERLIN_FETCH_MATERIALX=ON")
  endif()
  get_target_property(_merlin_materialx_source_version
    MaterialXCore VERSION)
  if(NOT _merlin_materialx_source_version)
    message(FATAL_ERROR
      "The selected MaterialX source does not report its library version")
  endif()
  if(_merlin_materialx_source_version VERSION_LESS
     MERLIN_MATERIALX_MIN_VERSION)
    message(FATAL_ERROR
      "MaterialX ${_merlin_materialx_source_version} is older than the required ${MERLIN_MATERIALX_MIN_VERSION}")
  endif()
  set(MERLIN_MATERIALX_DETECTED_VERSION
      "${_merlin_materialx_source_version}")
endif()

if(NOT TARGET MaterialXGenSlang)
  message(FATAL_ERROR
    "The selected MaterialX dependency does not provide MaterialXGenSlang")
endif()
if(DEFINED MATERIALX_STDLIB_DIR)
  set(_merlin_materialx_data_root "${MATERIALX_BASE_DIR}")
endif()
