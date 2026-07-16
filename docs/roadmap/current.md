# Current

Only incomplete work for the next release milestone and active carry-over is
listed here. Completed pre-release detail is retained in the
[delivery history](../reports/delivery-history.md); shipped versions will be
recorded in the [changelog](../../CHANGELOG.md).

## Next milestone

### ⬜ v0.9.0 — `merlin-viewport` and Vulkan presentation

Extract the minimum backend-neutral operations needed by the working Vulkan
path and an upcoming Metal bootstrap: backend factory and selection,
renderer-meaning capabilities and limits, frame submit/resolve, completion
lifetime, presentation target, common telemetry, and errors. Keep command
encoding, transitions, descriptors/argument buffers, memory, synchronization,
and native surface objects backend-owned.

Build the dedicated `merlin-viewport` application as a backend-neutral native
host for window/input, camera, resize, renderer settings, selection/picking
foundations, overlays, benchmark mode, screenshots, and USD loading through
Hydra. Its first production presentation adapter is Vulkan with a direct
swapchain path; rendering and scene behavior remain shared with headless
execution. The application is a permanent product and performance reference,
not temporary Metal bootstrap scaffolding.

#### Exit criteria

- Core and Hydra public paths expose no Vulkan or Metal types.
- Vulkan preserves existing behavior through the new contract and supports
  direct swapchain rendering without CPU readback.
- Vsync-off measurements and matching headless/viewport output are retained as
  evidence.
- The executable's host and interaction layers are reused unchanged by Metal,
  Mesh, and Gaussian work. usdview and DCC presentation remain separate.
