# Current

Only incomplete work for the next release milestone and active carry-over is
listed here. Completed pre-release detail is retained in the
[delivery history](../reports/delivery-history.md); shipped versions will be
recorded in the [changelog](../../CHANGELOG.md).

## Next milestone: v0.3.0 — practical Hydra mesh input

**Status:** implemented on `codex/v0.3.0-practical-hydra-mesh-input`; exit
criteria are enforced by the `merlin-hydra2-usdview-smoke`,
`merlin-mesh-scale-regression`, `merlin-vulkan-resource-update`, and
`merlin-benchmark-json` CTest gates. Release pending.

Normalize Hydra normals, display color/opacity, UVs, indexed primvars, and all
surface interpolation classes into packed vertices; robustly triangulate
concave polygonal faces with malformed-topology diagnostics; preserve authored
material binding identity; flatten native and nested Hydra instancing while
sharing geometry; and deliver color, depth, primId, and instanceId AOVs.

### Exit criteria

- Concave, indexed, and face-varying mesh input renders through the install-tree
  usdview path without Vulkan validation messages.
- PointInstancer prototypes produce one independently transformed draw per
  authored instance while retaining one geometry resource.
- `primId` and `instanceId` are valid for covered pixels and distinguish mesh
  and instance identity.
- Scale gates cover a one-million-triangle mesh, 10,000 small meshes, and 256
  repeated primvar edits.

## Carry-over follow-ups

- Define a host-neutral diagnostic sink instead of writing directly to stderr or
  a DCC-specific logger.
- Add configure-time OpenUSD version, build-configuration, and C++ runtime ABI
  compatibility checks.
- Define a common machine-readable result schema for discovery, delegate
  creation, RenderBuffer, GPU render, and host-presentation assertions.
