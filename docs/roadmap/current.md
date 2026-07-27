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

Implementation progress:

- the adapter now discovers the application-owned Hgi render driver through
  the public Hydra driver contract on OpenUSD 26.05 and 26.08;
- when a Vulkan Hgi driver is supplied, the color RenderBuffer creates an
  Hgi-owned target and submits Tier 0 CPU-to-Hgi upload; depth and id buffers
  stay on `Map` readback, target destruction is handed back to the Hgi that
  created it, and the adapter introduces no coarse idle wait;
- missing, disabled, non-Vulkan, driver-swap, and operationally rejected paths
  retain the original CPU RenderBuffer and report structured selection,
  fallback, target, byte, and encode-time telemetry alongside a coarse-wait
  counter that stays at zero because no idle wait is introduced;
- selected-AOV renderer image export, Vulkan GPU copy, distinct bridge
  completion, host-consumption retirement, and smoke evidence against an
  OpenUSD package that ships a Vulkan Hgi driver remain before the milestone
  can exit.

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

`ost 0.19.0` requires a renderer report to record a producer session that ran
to completion and succeeded before it will accept a PASS. The generated
`openstrata.renderer-report/v1alpha1` document emits bare `{id, status}` checks
with no such field, so `ost validate` reports `renderer-evidence: fail` even
after a managed build that OpenStrata performed itself.

The report generator must bind each assertion to the outcome of the session
that produced it, so a failed or partial run cannot leave a stale PASS behind.
This depends on the upstream schema being published; the gap and the
corresponding upstream ask are recorded in
[OST report 9](../reports/ost/09-2026-07-23-v0.20.0-asks.md).

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
