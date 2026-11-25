set(_opensubdiv "${MERLIN_OPENSUBDIV_SOURCE_DIR}/opensubdiv")
set(_sources
  far/bilinearPatchBuilder.cpp
  far/catmarkPatchBuilder.cpp
  far/error.cpp
  far/loopPatchBuilder.cpp
  far/patchBasis.cpp
  far/patchBuilder.cpp
  far/patchDescriptor.cpp
  far/patchMap.cpp
  far/patchTable.cpp
  far/patchTableFactory.cpp
  far/ptexIndices.cpp
  far/stencilBuilder.cpp
  far/stencilTable.cpp
  far/stencilTableFactory.cpp
  far/topologyDescriptor.cpp
  far/topologyRefiner.cpp
  far/topologyRefinerFactory.cpp
  sdc/crease.cpp
  sdc/typeTraits.cpp
  vtr/fvarLevel.cpp
  vtr/fvarRefinement.cpp
  vtr/level.cpp
  vtr/quadRefinement.cpp
  vtr/refinement.cpp
  vtr/sparseSelector.cpp
  vtr/triRefinement.cpp
)
list(TRANSFORM _sources PREPEND "${_opensubdiv}/")

add_library(merlin-opensubdiv OBJECT ${_sources})
target_compile_features(merlin-opensubdiv PRIVATE cxx_std_14)
set_target_properties(merlin-opensubdiv PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)
if(MSVC)
  target_compile_definitions(merlin-opensubdiv PRIVATE _USE_MATH_DEFINES)
endif()
