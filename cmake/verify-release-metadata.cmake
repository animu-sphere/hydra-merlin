if(NOT DEFINED MERLIN_METADATA OR
   NOT DEFINED MERLIN_EXPECTED_VERSION OR
   NOT DEFINED MERLIN_EXPECTED_VULKAN OR
   NOT DEFINED MERLIN_EXPECTED_HYDRA2)
  message(FATAL_ERROR "release metadata verification arguments are incomplete")
endif()

file(READ "${MERLIN_METADATA}" _merlin_metadata)

string(JSON _schema GET "${_merlin_metadata}" schema)
string(JSON _schema_version GET "${_merlin_metadata}" schema_version)
string(JSON _project_version GET "${_merlin_metadata}" project version)
string(JSON _vulkan_enabled GET "${_merlin_metadata}" configuration vulkan)
string(JSON _hydra2_enabled GET "${_merlin_metadata}" configuration hydra2)
string(JSON _packaging_contract GET "${_merlin_metadata}"
       packaging runtime_products_contract)
string(JSON _exported_target_count LENGTH "${_merlin_metadata}"
       packaging exported_targets)
string(JSON _runtime_product_count LENGTH "${_merlin_metadata}"
  packaging runtime_products)
string(JSON _openusd_validated GET "${_merlin_metadata}"
  requirements openusd validated)
string(JSON _openusd_detected GET "${_merlin_metadata}"
  requirements openusd detected)

if(NOT _schema STREQUAL "animu-sphere.hdmerlin.release-metadata" OR
   NOT _schema_version EQUAL 1)
  message(FATAL_ERROR "unexpected release metadata schema")
endif()
if(NOT "${_project_version}" VERSION_EQUAL "${MERLIN_EXPECTED_VERSION}")
  message(FATAL_ERROR
    "metadata version ${_project_version} != ${MERLIN_EXPECTED_VERSION}")
endif()
# string(JSON GET) yields ON/OFF for JSON booleans and MERLIN_EXPECTED_* arrive
# as CMake BOOL values, so compare them as booleans rather than by exact spelling.
if((_vulkan_enabled AND NOT MERLIN_EXPECTED_VULKAN) OR
   (NOT _vulkan_enabled AND MERLIN_EXPECTED_VULKAN))
  message(FATAL_ERROR
    "metadata Vulkan flag ${_vulkan_enabled} != ${MERLIN_EXPECTED_VULKAN}")
endif()
if((_hydra2_enabled AND NOT MERLIN_EXPECTED_HYDRA2) OR
   (NOT _hydra2_enabled AND MERLIN_EXPECTED_HYDRA2))
  message(FATAL_ERROR
    "metadata Hydra flag ${_hydra2_enabled} != ${MERLIN_EXPECTED_HYDRA2}")
endif()
if(NOT _packaging_contract STREQUAL "runtime-only")
  message(FATAL_ERROR "unexpected runtime product packaging contract")
endif()

if(MERLIN_EXPECTED_VULKAN)
  if(NOT _exported_target_count EQUAL 3)
    message(FATAL_ERROR "Vulkan metadata must export three targets")
  endif()
  string(JSON _vulkan_target GET "${_merlin_metadata}"
         packaging exported_targets 2)
  if(NOT _vulkan_target STREQUAL "Merlin::Vulkan")
    message(FATAL_ERROR "Vulkan metadata target is missing")
  endif()
else()
  if(NOT _exported_target_count EQUAL 2)
    message(FATAL_ERROR "Core metadata must export two targets")
  endif()
endif()

if(MERLIN_EXPECTED_HYDRA2)
  if(NOT _openusd_detected STREQUAL _openusd_validated)
    message(FATAL_ERROR
      "Hydra metadata OpenUSD ${_openusd_detected} != validated ${_openusd_validated}")
  endif()
  if(NOT _runtime_product_count EQUAL 3)
    message(FATAL_ERROR "Hydra metadata must list three runtime products")
  endif()
  string(JSON _hydra_product GET "${_merlin_metadata}"
         packaging runtime_products 2)
  if(NOT _hydra_product STREQUAL "hdMerlin")
    message(FATAL_ERROR "Hydra metadata runtime product is missing")
  endif()
elseif(MERLIN_EXPECTED_VULKAN)
  if(NOT _runtime_product_count EQUAL 2)
    message(FATAL_ERROR "Vulkan metadata must list two runtime products")
  endif()
elseif(NOT _runtime_product_count EQUAL 0)
  message(FATAL_ERROR "Core metadata must not list runtime products")
endif()
