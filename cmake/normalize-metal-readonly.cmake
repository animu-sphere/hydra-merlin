# Slang 2026.8 emits StructuredBuffer<T> as device T*, including temporary
# pointers into its elements. Metal diagnoses these as writable vertex resources.
# The native GPU Scene Forward wrapper only reads these four record types.
# Restore const throughout that wrapper without changing bindings or layouts.
# Remove this compatibility step when the pinned compiler preserves read access.
file(READ "${MERLIN_METAL_SOURCE}" _source)
string(REGEX REPLACE
  "((GpuGeometry|GpuInstance|GpuInstance_natural|GpuMaterial|GpuDraw)_[0-9]+) device\\*"
  "\\1 device const*" _source "${_source}")
file(WRITE "${MERLIN_METAL_SOURCE}" "${_source}")
