if(MERLIN_ENABLE_VULKAN)
  set(_merlin_metadata_vulkan_enabled true)
  set(_merlin_metadata_vulkan_version "null")
  if(DEFINED MERLIN_VULKAN_DETECTED_VERSION AND
     MERLIN_VULKAN_DETECTED_VERSION MATCHES "^[0-9A-Za-z.+_-]+$")
    set(_merlin_metadata_vulkan_version
        "\"${MERLIN_VULKAN_DETECTED_VERSION}\"")
  endif()
  set(_merlin_metadata_vulkan_sdk_version "null")
  if(DEFINED MERLIN_VULKAN_SDK_DETECTED_VERSION AND
     MERLIN_VULKAN_SDK_DETECTED_VERSION MATCHES "^[0-9A-Za-z.+_-]+$")
    set(_merlin_metadata_vulkan_sdk_version
        "\"${MERLIN_VULKAN_SDK_DETECTED_VERSION}\"")
  endif()
  set(_merlin_metadata_slang_version "null")
  if(DEFINED MERLIN_SLANG_DETECTED_VERSION AND
     MERLIN_SLANG_DETECTED_VERSION MATCHES "^[0-9]+[.][0-9]+([.][0-9]+)?$")
    set(_merlin_metadata_slang_version
        "\"${MERLIN_SLANG_DETECTED_VERSION}\"")
  endif()
  set(_merlin_metadata_shader_artifact_schema
      "${MERLIN_SHADER_ARTIFACT_SCHEMA_VERSION}")
  set(_merlin_metadata_exported_vulkan
      ",\n      \"Merlin::Vulkan\"")
  set(_merlin_metadata_runtime_products
      "      \"merlin-headless\",\n      \"merlin-benchmark\"")
else()
  set(_merlin_metadata_vulkan_enabled false)
  set(_merlin_metadata_vulkan_version "null")
  set(_merlin_metadata_vulkan_sdk_version "null")
  set(_merlin_metadata_slang_version "null")
  set(_merlin_metadata_shader_artifact_schema "null")
  set(_merlin_metadata_exported_vulkan "")
  set(_merlin_metadata_runtime_products "")
endif()

if(MERLIN_ENABLE_METAL)
  set(_merlin_metadata_metal_enabled true)
  set(_merlin_metadata_exported_metal
      ",\n      \"Merlin::Metal\"")
else()
  set(_merlin_metadata_metal_enabled false)
  set(_merlin_metadata_exported_metal "")
endif()

if(MERLIN_ENABLE_MATERIALX)
  set(_merlin_metadata_materialx_enabled true)
  set(_merlin_metadata_materialx_version
      "\"${MERLIN_MATERIALX_DETECTED_VERSION}\"")
  set(_merlin_metadata_exported_materialx
      ",\n      \"Merlin::MaterialX\"")
else()
  set(_merlin_metadata_materialx_enabled false)
  set(_merlin_metadata_materialx_version "null")
  set(_merlin_metadata_exported_materialx "")
endif()

if(TARGET merlin-viewport)
  set(_merlin_metadata_viewport_enabled true)
  if(_merlin_metadata_runtime_products)
    string(APPEND _merlin_metadata_runtime_products ",\n")
  endif()
  string(APPEND _merlin_metadata_runtime_products "      \"merlin-viewport\"")
else()
  set(_merlin_metadata_viewport_enabled false)
endif()

if(MERLIN_ENABLE_HYDRA2)
  set(_merlin_metadata_hydra2_enabled true)
  set(_merlin_metadata_openusd_version "null")
  if(DEFINED MERLIN_OPENUSD_DETECTED_VERSION AND
     MERLIN_OPENUSD_DETECTED_VERSION MATCHES "^[0-9A-Za-z.+_-]+$")
    set(_merlin_metadata_openusd_version
        "\"${MERLIN_OPENUSD_DETECTED_VERSION}\"")
  endif()
  if(_merlin_metadata_runtime_products)
    string(APPEND _merlin_metadata_runtime_products ",\n")
  endif()
  string(APPEND _merlin_metadata_runtime_products "      \"hdMerlin\"")
else()
  set(_merlin_metadata_hydra2_enabled false)
  set(_merlin_metadata_openusd_version "null")
endif()
if(MERLIN_ENABLE_HGI_VULKAN_BRIDGE)
  set(_merlin_metadata_hgi_vulkan_bridge_enabled true)
else()
  set(_merlin_metadata_hgi_vulkan_bridge_enabled false)
endif()
if(MERLIN_ENABLE_HGI_METAL_BRIDGE)
  set(_merlin_metadata_hgi_metal_bridge_enabled true)
else()
  set(_merlin_metadata_hgi_metal_bridge_enabled false)
endif()

if(_merlin_metadata_runtime_products)
  set(_merlin_metadata_runtime_products
      "[\n${_merlin_metadata_runtime_products}\n    ]")
else()
  set(_merlin_metadata_runtime_products "[]")
endif()

configure_file(
  cmake/merlin-release-metadata.json.in
  "${CMAKE_CURRENT_BINARY_DIR}/merlin-release-metadata.json"
  @ONLY
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/merlin-release-metadata.json"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/merlin"
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/merlin"
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/openstrata.renderer.yaml"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/merlin"
)
if(MERLIN_ENABLE_VULKAN OR MERLIN_ENABLE_METAL OR MERLIN_ENABLE_MATERIALX)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/merlin/licenses"
  )
endif()
if(MERLIN_ENABLE_MATERIALX AND
   EXISTS "${_merlin_materialx_data_root}/LICENSE")
  install(FILES "${_merlin_materialx_data_root}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/merlin/licenses/materialx"
  )
endif()
if(MERLIN_ENABLE_VULKAN OR MERLIN_ENABLE_HYDRA2)
  install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openusd/LICENSE.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openusd/NOTICE.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/merlin/licenses/openusd"
  )
endif()

if(MERLIN_BUILD_TESTS)
  add_test(
    NAME merlin-release-metadata
    COMMAND ${CMAKE_COMMAND}
      "-DMERLIN_METADATA:FILEPATH=${CMAKE_CURRENT_BINARY_DIR}/merlin-release-metadata.json"
      "-DMERLIN_EXPECTED_VERSION:STRING=${PROJECT_VERSION}"
      "-DMERLIN_EXPECTED_VULKAN:BOOL=${MERLIN_ENABLE_VULKAN}"
      "-DMERLIN_EXPECTED_METAL:BOOL=${MERLIN_ENABLE_METAL}"
      "-DMERLIN_EXPECTED_HYDRA2:BOOL=${MERLIN_ENABLE_HYDRA2}"
      "-DMERLIN_EXPECTED_HGI_VULKAN_BRIDGE:BOOL=${MERLIN_ENABLE_HGI_VULKAN_BRIDGE}"
      "-DMERLIN_EXPECTED_HGI_METAL_BRIDGE:BOOL=${MERLIN_ENABLE_HGI_METAL_BRIDGE}"
      "-DMERLIN_EXPECTED_MATERIALX:BOOL=${MERLIN_ENABLE_MATERIALX}"
      "-DMERLIN_EXPECTED_VIEWPORT:BOOL=${_merlin_metadata_viewport_enabled}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify-release-metadata.cmake"
  )
  set_tests_properties(merlin-release-metadata PROPERTIES LABELS packaging)
endif()
