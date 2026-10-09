include_guard(GLOBAL)

# Some shared SDK exports retain the producer's Python headers on every pxr
# target, even when pxrConfig honors the consumer's Python3_* cache entries.
# Rebind only directories exported by OpenUSD's Python library. The SDK's
# find_dependency call still owns Python version/ABI validation, and the
# verified SDK files themselves remain untouched.
function(merlin_rebind_openusd_python_includes)
  if(NOT TARGET python OR NOT TARGET Python3::Python OR
     NOT "python" IN_LIST PXR_LIBRARIES)
    return()
  endif()
  get_target_property(_producer_dirs python INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
  get_target_property(_consumer_dirs Python3::Python INTERFACE_INCLUDE_DIRECTORIES)
  if(NOT _producer_dirs OR NOT _consumer_dirs)
    return()
  endif()
  foreach(_target IN LISTS PXR_LIBRARIES)
    if(NOT TARGET "${_target}")
      continue()
    endif()
    foreach(_property INTERFACE_INCLUDE_DIRECTORIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
      get_target_property(_original_dirs "${_target}" "${_property}")
      if(NOT _original_dirs)
        continue()
      endif()
      set(_updated_dirs)
      foreach(_directory IN LISTS _original_dirs)
        if(_directory IN_LIST _producer_dirs)
          list(APPEND _updated_dirs ${_consumer_dirs})
        else()
          list(APPEND _updated_dirs "${_directory}")
        endif()
      endforeach()
      list(REMOVE_DUPLICATES _updated_dirs)
      set_property(TARGET "${_target}" PROPERTY "${_property}" "${_updated_dirs}")
    endforeach()
  endforeach()
endfunction()
