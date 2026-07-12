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

if(NOT _schema STREQUAL "animu-sphere.hdmerlin.release-metadata" OR
   NOT _schema_version EQUAL 1)
  message(FATAL_ERROR "unexpected release metadata schema")
endif()
if(NOT "${_project_version}" VERSION_EQUAL "${MERLIN_EXPECTED_VERSION}")
  message(FATAL_ERROR
    "metadata version ${_project_version} != ${MERLIN_EXPECTED_VERSION}")
endif()
if(NOT "${_vulkan_enabled}" STREQUAL "${MERLIN_EXPECTED_VULKAN}")
  message(FATAL_ERROR
    "metadata Vulkan flag ${_vulkan_enabled} != ${MERLIN_EXPECTED_VULKAN}")
endif()
if(NOT "${_hydra2_enabled}" STREQUAL "${MERLIN_EXPECTED_HYDRA2}")
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
  if(NOT _runtime_product_count EQUAL 3)
    message(FATAL_ERROR "Hydra metadata must list three runtime products")
  endif()
elseif(MERLIN_EXPECTED_VULKAN)
  if(NOT _runtime_product_count EQUAL 2)
    message(FATAL_ERROR "Vulkan metadata must list two runtime products")
  endif()
elseif(NOT _runtime_product_count EQUAL 0)
  message(FATAL_ERROR "Core metadata must not list runtime products")
endif()
