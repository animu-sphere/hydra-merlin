option(MERLIN_BUILD_TESTS "Build Merlin tests" ON)
set(MERLIN_GAUSSIAN_BUNDLED_SAMPLE
  "${CMAKE_CURRENT_SOURCE_DIR}/adapters/merlin-hydra2/tests/fixtures/leica-sofort-top8192.usdc")
set(MERLIN_GAUSSIAN_BUNDLED_REFERENCE_IMAGE
  "${CMAKE_CURRENT_SOURCE_DIR}/adapters/merlin-hydra2/tests/fixtures/leica-sofort-top8192-reference.png")
set(MERLIN_GAUSSIAN_SAMPLE "${MERLIN_GAUSSIAN_BUNDLED_SAMPLE}"
  CACHE FILEPATH
  "Optional standard OpenUSD Gaussian stage used by integration tests")
set(MERLIN_GAUSSIAN_REFERENCE_IMAGE "" CACHE FILEPATH
  "Optional reference image paired with MERLIN_GAUSSIAN_SAMPLE")
option(MERLIN_ENABLE_VULKAN "Build the Vulkan backend" ON)
option(MERLIN_ENABLE_METAL "Build the native Metal backend" ${APPLE})
option(MERLIN_ENABLE_HYDRA2 "Build the OpenUSD Hydra 2 adapter" OFF)
# Hydra 2 is also satisfied by Metal alone, so this cannot be a plain option
# defaulting to MERLIN_ENABLE_HYDRA2: a Metal-only Hydra build would then turn
# the bridge on implicitly and fail its own dependency rule. A dependent option
# also keeps the cached value from outliving a later MERLIN_ENABLE_HYDRA2=OFF.
include(CMakeDependentOption)
cmake_dependent_option(MERLIN_ENABLE_HGI_VULKAN_BRIDGE
  "Build the Hydra-owned HgiVulkan presentation bridge" ON
  "MERLIN_ENABLE_HYDRA2;MERLIN_ENABLE_VULKAN" OFF)
cmake_dependent_option(MERLIN_ENABLE_HGI_METAL_BRIDGE
  "Build the Hydra-owned HgiMetal presentation bridge" ON
  "MERLIN_ENABLE_HYDRA2;MERLIN_ENABLE_METAL" OFF)
option(MERLIN_ENABLE_MATERIALX
  "Build the MaterialXGenSlang material-function compiler" OFF)
option(MERLIN_FETCH_MATERIALX
  "Fetch the pinned MaterialX source when no compatible package is available" OFF)
set(MERLIN_MATERIALX_SOURCE_DIR "" CACHE PATH
  "Optional compatible MaterialX source tree used instead of fetching")
option(MERLIN_BUILD_VIEWPORT "Build the native merlin-viewport application" ON)
option(MERLIN_GENERATE_RENDERER_REPORT
  "Generate OpenStrata renderer evidence after linking merlin-headless" ON)
