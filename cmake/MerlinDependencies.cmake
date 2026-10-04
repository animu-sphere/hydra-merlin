# Keep imported targets and detected-version outputs in the owning directory.
# Macros preserve the existing directory scope, including PARENT_SCOPE exports.
include_guard(GLOBAL)

if(MERLIN_ENABLE_MATERIALX)
  include(MerlinMaterialX)
endif()

macro(merlin_find_slang_dependencies)
  find_program(MERLIN_SLANGC_EXECUTABLE
    NAMES slangc
    HINTS
      "$ENV{VULKAN_SDK}/Bin"
      "$ENV{VULKAN_SDK}/bin"
    DOC "Pinned Slang compiler used for Merlin shader artifacts"
    REQUIRED
  )
  execute_process(
    COMMAND "${MERLIN_SLANGC_EXECUTABLE}" -version
    RESULT_VARIABLE _merlin_slang_version_result
    OUTPUT_VARIABLE _merlin_slang_version
    ERROR_VARIABLE _merlin_slang_version_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
  )
  if(NOT _merlin_slang_version_result EQUAL 0)
    message(FATAL_ERROR
      "slangc -version failed: ${_merlin_slang_version_error}")
  endif()
  if(_merlin_slang_version STREQUAL "")
    # Vulkan SDK builds of slangc currently print the version to stderr.
    set(_merlin_slang_version "${_merlin_slang_version_error}")
  endif()
  # Extract rather than match the whole output: slangc builds differ in whether
  # they print a bare version, a "slangc version X" line, or a suffixed one.
  if(NOT _merlin_slang_version MATCHES "([0-9]+[.][0-9]+([.][0-9]+)?)")
    message(FATAL_ERROR
      "could not read a version from 'slangc -version' at ${MERLIN_SLANGC_EXECUTABLE}: '${_merlin_slang_version}'")
  endif()
  set(_merlin_slang_version "${CMAKE_MATCH_1}")
  string(REPLACE "." "[.]" _merlin_slang_series_regex
         "${MERLIN_SLANG_REQUIRED_SERIES}")
  if(NOT _merlin_slang_version MATCHES
     "^${_merlin_slang_series_regex}([.][0-9]+)?$")
    message(FATAL_ERROR
      "hdMerlin requires slangc ${MERLIN_SLANG_REQUIRED_SERIES}.x; found '${_merlin_slang_version}' at ${MERLIN_SLANGC_EXECUTABLE}")
  endif()
  set(MERLIN_SLANG_DETECTED_VERSION
      "${_merlin_slang_version}" PARENT_SCOPE)
endmacro()

macro(merlin_find_vulkan_dependencies)
  find_package(Vulkan ${MERLIN_VULKAN_MIN_VERSION} REQUIRED)
  set(MERLIN_VULKAN_DETECTED_VERSION "${Vulkan_VERSION}" PARENT_SCOPE)
  merlin_find_slang_dependencies()

  set(_merlin_vulkan_sdk_version "unknown")
  if(DEFINED ENV{VULKAN_SDK})
    set(_merlin_vulkan_sdk_path "$ENV{VULKAN_SDK}")
    cmake_path(GET _merlin_vulkan_sdk_path FILENAME
               _merlin_vulkan_sdk_version)
    # LunarG's Linux VULKAN_SDK ends in an architecture directory below
    # the version, unlike the Windows SDK prefix.
    if(_merlin_vulkan_sdk_version MATCHES "^(x86_64|aarch64)$")
      cmake_path(GET _merlin_vulkan_sdk_path PARENT_PATH
                 _merlin_vulkan_sdk_parent)
      cmake_path(GET _merlin_vulkan_sdk_parent FILENAME
                 _merlin_vulkan_sdk_version)
    endif()
  endif()
  set(MERLIN_VULKAN_SDK_DETECTED_VERSION
      "${_merlin_vulkan_sdk_version}" PARENT_SCOPE)
endmacro()

macro(merlin_find_hydra_dependencies)
  if(MERLIN_ENABLE_HGI_VULKAN_BRIDGE)
    # Vulkan-enabled OpenUSD packages expose hgiVulkan through pxrTargets.cmake,
    # whose imported link interface names Vulkan::Vulkan.
    find_package(Vulkan REQUIRED COMPONENTS shaderc_combined)
  endif()
  find_package(pxr CONFIG REQUIRED)
  include(GNUInstallDirs)

  get_target_property(_merlin_pxr_include_dirs hd INTERFACE_INCLUDE_DIRECTORIES)
  set(_merlin_pxr_header "")
  foreach(_merlin_pxr_include_dir IN LISTS _merlin_pxr_include_dirs)
    if(EXISTS "${_merlin_pxr_include_dir}/pxr/pxr.h")
      set(_merlin_pxr_header "${_merlin_pxr_include_dir}/pxr/pxr.h")
      set(_merlin_pxr_include_dir_selected "${_merlin_pxr_include_dir}")
      break()
    endif()
  endforeach()
  if(NOT _merlin_pxr_header)
    message(FATAL_ERROR
      "hdMerlin could not locate pxr/pxr.h through the imported hd target; "
      "use a complete OpenUSD SDK package")
  endif()

  file(STRINGS "${_merlin_pxr_header}" _merlin_pxr_version_line
    REGEX "^#define PXR_VERSION [0-9]+$")
  if(NOT _merlin_pxr_version_line)
    message(FATAL_ERROR
      "hdMerlin could not read PXR_VERSION from ${_merlin_pxr_header}")
  endif()
  string(REGEX REPLACE ".*PXR_VERSION ([0-9]+).*" "\\1"
    _merlin_pxr_version_number "${_merlin_pxr_version_line}")
  math(EXPR _merlin_pxr_version_major
    "${_merlin_pxr_version_number} / 100")
  math(EXPR _merlin_pxr_version_minor
    "${_merlin_pxr_version_number} % 100")
  if(_merlin_pxr_version_minor LESS 10)
    set(_merlin_pxr_version_minor "0${_merlin_pxr_version_minor}")
  endif()
  set(_merlin_openusd_detected_version
    "${_merlin_pxr_version_major}.${_merlin_pxr_version_minor}")
  set(MERLIN_OPENUSD_DETECTED_VERSION
    "${_merlin_openusd_detected_version}" PARENT_SCOPE)

  if(NOT _merlin_openusd_detected_version IN_LIST
     MERLIN_OPENUSD_VALIDATED_VERSIONS)
    string(JOIN ", " _merlin_openusd_supported_versions
      ${MERLIN_OPENUSD_VALIDATED_VERSIONS})
    message(FATAL_ERROR
      "hdMerlin validates OpenUSD ${_merlin_openusd_supported_versions}, but "
      "${_merlin_openusd_detected_version} was found at "
      "${_merlin_pxr_include_dir_selected}. Use the validated SDK or disable "
      "MERLIN_ENABLE_HYDRA2; Core and native GPU backends remain "
      "OpenUSD-independent.")
  endif()
  set(MERLIN_OPENUSD_VALIDATED_VERSION
    "${_merlin_openusd_detected_version}" PARENT_SCOPE)

  get_target_property(_merlin_hd_target_type hd TYPE)
  if(NOT _merlin_hd_target_type STREQUAL "SHARED_LIBRARY")
    message(FATAL_ERROR
      "hdMerlin requires the validated shared-library OpenUSD layout; imported "
      "target hd is ${_merlin_hd_target_type}. Static/monolithic plugin ABI "
      "support is not yet validated.")
  endif()

  set(_merlin_openusd_release_only FALSE)
  if(MSVC)
    get_target_property(_merlin_hd_imported_configurations hd
      IMPORTED_CONFIGURATIONS)
    if(_merlin_hd_imported_configurations)
      list(TRANSFORM _merlin_hd_imported_configurations TOUPPER)
      if(NOT "DEBUG" IN_LIST _merlin_hd_imported_configurations)
        set(_merlin_openusd_release_only TRUE)
        message(STATUS
          "OpenUSD ${_merlin_openusd_detected_version} provides no Debug "
          "libraries; hdMerlin Debug builds will be rejected to prevent an "
          "MSVC runtime/iterator ABI mismatch")
      endif()
    endif()
  endif()
endmacro()

macro(merlin_find_viewport_dependencies)
  enable_language(C)
  include(FetchContent)

  find_package(glfw3 ${MERLIN_GLFW_MIN_VERSION} CONFIG QUIET)
  if(NOT TARGET glfw)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glfw
      GIT_REPOSITORY https://github.com/glfw/glfw.git
      GIT_TAG ${MERLIN_GLFW_PINNED_COMMIT}
      GIT_SHALLOW FALSE
    )
    FetchContent_MakeAvailable(glfw)
  endif()

  FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG ${MERLIN_IMGUI_PINNED_COMMIT}
    GIT_SHALLOW FALSE
  )
  FetchContent_MakeAvailable(imgui)

  if(MERLIN_ENABLE_HYDRA2)
    if(APPLE)
      enable_language(OBJC)
    endif()
    set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NFD_BUILD_SDL2_TESTS OFF CACHE BOOL "" FORCE)
    set(NFD_BUILD_GLFW3_TESTS OFF CACHE BOOL "" FORCE)
    set(NFD_INSTALL OFF CACHE BOOL "" FORCE)
    if(UNIX AND NOT APPLE)
      # The portal delegates the chooser to the active desktop environment,
      # which keeps the viewport usable under both X11 and Wayland without
      # linking either display-server client into the dialog implementation.
      set(NFD_PORTAL ON CACHE BOOL "" FORCE)
      set(NFD_X11 OFF CACHE BOOL "" FORCE)
      set(NFD_WAYLAND OFF CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(nfd
      GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
      GIT_TAG ${MERLIN_NFD_PINNED_COMMIT}
      GIT_SHALLOW FALSE
    )
    FetchContent_MakeAvailable(nfd)
  endif()
endmacro()
