if(NOT DEFINED MERLIN_METADATA OR
   NOT DEFINED MERLIN_EXPECTED_VERSION OR
   NOT DEFINED MERLIN_EXPECTED_VULKAN OR
   NOT DEFINED MERLIN_EXPECTED_HYDRA2 OR
   NOT DEFINED MERLIN_EXPECTED_MATERIALX OR
   NOT DEFINED MERLIN_EXPECTED_VIEWPORT)
  message(FATAL_ERROR "release metadata verification arguments are incomplete")
endif()

file(READ "${MERLIN_METADATA}" _merlin_metadata)

string(JSON _schema GET "${_merlin_metadata}" schema)
string(JSON _schema_version GET "${_merlin_metadata}" schema_version)
string(JSON _project_version GET "${_merlin_metadata}" project version)
string(JSON _vulkan_enabled GET "${_merlin_metadata}" configuration vulkan)
string(JSON _hydra2_enabled GET "${_merlin_metadata}" configuration hydra2)
string(JSON _materialx_enabled GET "${_merlin_metadata}" configuration materialx)
string(JSON _viewport_enabled GET "${_merlin_metadata}" configuration viewport)
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
string(JSON _slang_required GET "${_merlin_metadata}"
  requirements slang required_series)
string(JSON _shader_abi_version GET "${_merlin_metadata}"
  requirements slang shader_abi_version)
string(JSON _materialx_minimum GET "${_merlin_metadata}"
  requirements materialx minimum)
string(JSON _materialx_detected GET "${_merlin_metadata}"
  requirements materialx detected)
string(JSON _materialx_revision GET "${_merlin_metadata}"
  requirements materialx genslang_revision)
string(JSON _glfw_minimum GET "${_merlin_metadata}"
  requirements glfw minimum)
string(JSON _glfw_fallback_commit GET "${_merlin_metadata}"
  requirements glfw fallback_commit)

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
if((_materialx_enabled AND NOT MERLIN_EXPECTED_MATERIALX) OR
   (NOT _materialx_enabled AND MERLIN_EXPECTED_MATERIALX))
  message(FATAL_ERROR
    "metadata MaterialX flag ${_materialx_enabled} != ${MERLIN_EXPECTED_MATERIALX}")
endif()
if((_viewport_enabled AND NOT MERLIN_EXPECTED_VIEWPORT) OR
   (NOT _viewport_enabled AND MERLIN_EXPECTED_VIEWPORT))
  message(FATAL_ERROR
    "metadata viewport flag ${_viewport_enabled} != ${MERLIN_EXPECTED_VIEWPORT}")
endif()
if(MERLIN_EXPECTED_VIEWPORT AND NOT MERLIN_EXPECTED_VULKAN)
  message(FATAL_ERROR "viewport metadata requires the Vulkan backend")
endif()
if(NOT _packaging_contract STREQUAL "runtime-only")
  message(FATAL_ERROR "unexpected runtime product packaging contract")
endif()
if(NOT _slang_required STREQUAL "2026.8" OR
   NOT _shader_abi_version EQUAL 2)
  message(FATAL_ERROR "unexpected Slang/shader ABI metadata contract")
endif()
string(LENGTH "${_materialx_revision}" _materialx_revision_length)
if(NOT _materialx_minimum STREQUAL "1.39.6" OR
   NOT _materialx_revision MATCHES "^[0-9a-f]+$" OR
   NOT _materialx_revision_length EQUAL 40)
  message(FATAL_ERROR "unexpected MaterialXGenSlang dependency metadata")
endif()
string(LENGTH "${_glfw_fallback_commit}" _glfw_commit_length)
if(NOT _glfw_minimum STREQUAL "3.4" OR
   NOT _glfw_fallback_commit MATCHES "^[0-9a-f]+$" OR
   NOT _glfw_commit_length EQUAL 40)
  message(FATAL_ERROR "unexpected GLFW dependency metadata")
endif()

if(MERLIN_EXPECTED_VULKAN)
  string(JSON _slang_detected GET "${_merlin_metadata}"
    requirements slang detected)
  string(JSON _shader_artifact_schema GET "${_merlin_metadata}"
    requirements slang artifact_schema_version)
  if(NOT _slang_detected MATCHES "^2026[.]8([.][0-9]+)?$" OR
     NOT _shader_artifact_schema EQUAL 1)
    message(FATAL_ERROR
      "Vulkan metadata requires Slang 2026.8.x and shader artifacts v1")
  endif()
  set(_expected_exported_target_count 4)
  if(MERLIN_EXPECTED_MATERIALX)
    math(EXPR _expected_exported_target_count
         "${_expected_exported_target_count} + 1")
  endif()
  if(NOT _exported_target_count EQUAL _expected_exported_target_count)
    message(FATAL_ERROR
      "Vulkan metadata exported target count is incorrect")
  endif()
  string(JSON _vulkan_target GET "${_merlin_metadata}"
         packaging exported_targets 3)
  if(NOT _vulkan_target STREQUAL "Merlin::Vulkan")
    message(FATAL_ERROR "Vulkan metadata target is missing")
  endif()
else()
  set(_expected_exported_target_count 3)
  if(MERLIN_EXPECTED_MATERIALX)
    math(EXPR _expected_exported_target_count
         "${_expected_exported_target_count} + 1")
  endif()
  if(NOT _exported_target_count EQUAL _expected_exported_target_count)
    message(FATAL_ERROR "Core metadata exported target count is incorrect")
  endif()
endif()

if(MERLIN_EXPECTED_MATERIALX)
  if("${_materialx_detected}" VERSION_LESS "${_materialx_minimum}")
    message(FATAL_ERROR
      "MaterialX metadata ${_materialx_detected} is older than ${_materialx_minimum}")
  endif()
  set(_materialx_target_index 3)
  if(MERLIN_EXPECTED_VULKAN)
    set(_materialx_target_index 4)
  endif()
  string(JSON _materialx_target GET "${_merlin_metadata}"
         packaging exported_targets ${_materialx_target_index})
  if(NOT _materialx_target STREQUAL "Merlin::MaterialX")
    message(FATAL_ERROR "MaterialX metadata target is missing")
  endif()
endif()

set(_expected_runtime_product_count 0)
if(MERLIN_EXPECTED_VULKAN)
  math(EXPR _expected_runtime_product_count
       "${_expected_runtime_product_count} + 2")
endif()
if(MERLIN_EXPECTED_VIEWPORT)
  math(EXPR _expected_runtime_product_count
       "${_expected_runtime_product_count} + 1")
endif()
if(MERLIN_EXPECTED_HYDRA2)
  math(EXPR _expected_runtime_product_count
       "${_expected_runtime_product_count} + 1")
endif()
if(NOT _runtime_product_count EQUAL _expected_runtime_product_count)
  message(FATAL_ERROR
    "metadata runtime product count ${_runtime_product_count} != ${_expected_runtime_product_count}")
endif()

if(MERLIN_EXPECTED_VIEWPORT)
  string(JSON _viewport_product GET "${_merlin_metadata}"
         packaging runtime_products 2)
  if(NOT _viewport_product STREQUAL "merlin-viewport")
    message(FATAL_ERROR "viewport metadata runtime product is missing")
  endif()
endif()

if(MERLIN_EXPECTED_HYDRA2)
  if(NOT _openusd_detected STREQUAL _openusd_validated)
    message(FATAL_ERROR
      "Hydra metadata OpenUSD ${_openusd_detected} != validated ${_openusd_validated}")
  endif()
  set(_hydra_product_index 2)
  if(MERLIN_EXPECTED_VIEWPORT)
    set(_hydra_product_index 3)
  endif()
  string(JSON _hydra_product GET "${_merlin_metadata}"
         packaging runtime_products ${_hydra_product_index})
  if(NOT _hydra_product STREQUAL "hdMerlin")
    message(FATAL_ERROR "Hydra metadata runtime product is missing")
  endif()
endif()
