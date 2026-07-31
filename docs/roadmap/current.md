# Current

Incomplete work for the next release milestone and active carry-over is listed
here. A completed milestone may remain until its release metadata is prepared;
its detailed pre-release history is retained in the
[delivery history](../reports/delivery-history.md); shipped versions are
recorded in the [changelog](../../CHANGELOG.md).

v0.10.0 shipped the MaterialX shader-generation boundary; its ownership rules
remain authoritative in the
[MaterialXGenSlang material boundary](../design/materialxgenslang-boundary.md).
v0.11.0 shipped the native Metal offscreen/residency backend; its objective,
compatibility, limitations, and evidence are recorded in
[docs/releases/v0.11.0.md](../releases/v0.11.0.md). v0.12.0 shipped native
Metal viewport presentation; its objective, compatibility, limitations, and
evidence are recorded in [docs/releases/v0.12.0.md](../releases/v0.12.0.md).

## v0.13.0 — HgiVulkan GPU-copy bridge

The next release starts host presentation work with HgiVulkan. The Hydra adapter
will establish Hgi-owned destination targets and retain the existing Tier 0 CPU
RenderBuffer path as the universal reference/fallback. It will then add
selected-AOV Vulkan GPU copy with explicit image state, renderer-to-bridge and
bridge-to-host completion, resize/frames-in-flight-safe retirement, structured
capability and rejection telemetry, and Tier 0 image/performance comparison.

This release does not claim direct native resource sharing or external
memory/semaphore interop. Same-logical-device direct sharing is a separately
gated v0.13.1 hardening path, and HgiMetal follows in v0.14.0. The full
contract, forbidden synchronization, validation matrix, and dependency rules
are in the [Hgi host presentation policy](../design/hgi-host-presentation.md).

Completed implementation:

- the adapter now discovers the application-owned Hgi render driver through
  the public Hydra driver contract on OpenUSD 26.05 and 26.08;
- when a Vulkan Hgi driver is supplied, the color RenderBuffer creates an
  Hgi-owned target. Packages exposing the validated native `hgiVulkan` target
  lend their Vulkan 1.3 device and graphics queue to Merlin's conventional
  renderer path, then copy the color AOV directly between same-device images;
  depth and id buffers stay on `Map` readback, target destruction is handed
  back to the Hgi that created it, and the adapter introduces no coarse idle
  wait. Merlin-owned Vulkan contexts retain the Vulkan 1.4 baseline;
- missing, disabled, non-Vulkan, driver-swap, and operationally rejected paths
  retain the original CPU RenderBuffer and report structured selection,
  fallback, target, byte, and encode-time telemetry alongside a coarse-wait
  counter that stays at zero because no idle wait is introduced;
- the Vulkan backend now exports selected color, depth, `primId`, and
  `instanceId` images with explicit native format, layout, stage/access,
  aspect, queue-family, extent, and renderer-completion metadata; a move-only
  lease pins each frame target across Resolve until the host bridge returns it,
  and export/active-lease telemetry makes that lifetime observable;
- native HgiVulkan copy records explicit source/destination layouts and a
  same-queue `vkCmdCopyImage`, restores the host target layout, and returns the
  source lease only from Hgi command-buffer completion. OpenUSD 26.05 and 26.08
  runtime smoke both prove one exported color AOV, three retained CPU-readback
  AOVs, no color `Map`/host upload, non-zero host-consumption completion, and a
  zero coarse-wait counter across the regression and resize sequence;
- the comparison fixture runs Tier 0 and HgiVulkan through the same 13-phase
  regression, bounds raster-edge differences to 0.25% of pixels and a 0.25
  mean channel error, and writes versioned image/performance evidence. On the
  OpenUSD 26.08 Windows baseline the maximum observed values are 0.1862% and
  0.1819; color GPU copy removes 1,289,520 CPU-readback bytes per frame and the
  representative median readback-plus-transfer time falls from about 19.1 ms
  to 14.2 ms;
- the milestone exit audit is complete pending release metadata. Tier 0 remains
  the universal fallback, all named phases including camera and resize prove
  GPU copy without color Map/upload, unsupported paths retain structured
  fallback reasons, retirement reaches Hgi completion, and the coarse-wait
  counter remains zero.

## Active carry-over

### ⬜ v0.10.x — Development viewport and diagnostic surface

Turn `merlin-viewport` into the primary renderer-development and profiling
tool without turning it into a USD authoring application. Add a thin
immediate-mode UI integration, preferably Dear ImGui, behind a host abstraction;
the UI remains outside Core and renderer backends and consumes host-neutral
settings, diagnostics, capability, and telemetry contracts.

The initial surface covers:

- backend/device capabilities and the selected path or rejected fallback;
- CPU/GPU frame timings and AOV selection;
- geometry, texture, sampler, descriptor, and VRAM residency;
- material/module compilation state and structured diagnostics;
- primitive/instance counts, uploads, allocations, camera, and viewport state;
- screenshot and benchmark controls.

Exit requires the viewport to explain path selection and rejection, make
material/resource fallback visible without log inspection, and support
interactive performance-regression inspection. The Vulkan render loop remains
independent of UI frame rate and widget implementation. A complete stage tree,
property editor, and authoring workflow remain outside this milestone.

## Phase A foundation gates

These cross-cutting items should land alongside v0.10.x, before the
implementation becomes substantially more backend-specific.

### ⬜ Evidence-tier separation

- **Tier 1 — required hosted checks:** Core Debug/Release on Windows and Linux,
  shader compilation, SPIR-V validation/reflection, Metal-target compilation,
  MaterialX generation/ABI tests, install-tree consumers, and Hydra adapter
  compilation where a reproducible SDK is available.
- **Tier 2 — required or scheduled capability checks:** Vulkan runtime/image
  tests, Hydra discovery and first frame, native Vulkan viewport smoke tests,
  and stable-update/changed-range tests.
- **Tier 3 — hardware-profile evidence:** timing thresholds, bindless
  selection, transfer-queue behavior, VRAM pressure/exhaustion, and
  vendor-specific capability reports.

GPU timing does not become a universal pull-request gate until runner variance
is controlled; missing hardware evidence remains distinguishable from a product
failure.

### ⬜ Producer-session renderer evidence

`ost 0.21.0` retains the producer-session validation boundary and improves
managed configure recovery, but this repository has not yet produced a
successful managed renderer session on the current Windows host. CMake still
stops during the MSVC compiler ABI try-compile, so the complete renderer JSON
envelope and the binding between managed completion evidence and generated
renderer assertions remain unverified here.

Exit requires one successful managed `renderer view` or `renderer viewport`
session followed by `ost validate`, plus a negative check showing that stale
external renderer evidence cannot be promoted by a newer managed completion.
The current recheck and exact remaining evidence are recorded in
[OST report 11](../reports/ost/11-2026-07-29-v0.21.0-recheck-v0.22.0-asks.md).

### ⬜ Linux Vulkan validation

Add Linux Vulkan configuration and shader builds, useful headless execution
through Mesa lavapipe, optional real-GPU capability execution, and GLFW viewport
smoke coverage for supported window systems.

### ⬜ Versioned renderer settings

Define a host-neutral settings schema before DCC integration expands. The first
version covers backend, presentation mode, Forward/experimental path, AOV,
lighting mode, exposure/tone mapping, alpha policy, debug views, validation,
and telemetry controls.

## Near-term execution order

1. Keep the released v0.10.0 boundary narrow; do not broaden node coverage
   merely to claim general MaterialX. Production quality belongs to v0.18.0.
2. Add the development viewport surface over existing capability, timing,
   residency, upload, material, AOV, and fallback contracts.
3. Strengthen non-GPU and Linux gates before implementation breadth grows.
4. Preserve the completed native Metal presentation path while extending the
   shared viewport diagnostics and platform evidence.
