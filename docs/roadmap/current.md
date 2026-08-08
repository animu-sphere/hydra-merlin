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
v0.13.0 shipped the HgiVulkan GPU-copy bridge; its release boundary is recorded
in [docs/releases/v0.13.0.md](../releases/v0.13.0.md). v0.13.1 shipped the
direct-path hardening boundary; its capability decision and evidence are in
[docs/releases/v0.13.1.md](../releases/v0.13.1.md).
v0.14.0 shipped HgiMetal GPU-copy host presentation; its objective,
compatibility, limitations, and evidence are recorded in
[docs/releases/v0.14.0.md](../releases/v0.14.0.md).
v0.14.1 shipped the Gaussian correctness MVP; its objective, compatibility,
limitations, and evidence are recorded in
[docs/releases/v0.14.1.md](../releases/v0.14.1.md).

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

Current UI follow-up:

- ✅ Native platform USD selection with stage switching.
- ✅ Rolling host/GPU timing history and expanded resource activity.
- ✅ Gaussian resource, particle, SH, projection, sorting, culling, cache,
  upload, and fallback diagnostics.
- ✅ Structured material diagnostic severity, fallback, and context display.
- ✅ Camera state and navigation values.
- ✅ Interactive renderer settings with explicit applied/rejected feedback.
- ✅ Interactive AOV selection and diagnostic image inspection.
- ✅ General host/backend diagnostic history beyond material diagnostics.
- ✅ Saved benchmark comparison, thresholds, and hitch markers in the UI.

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

### ✅ Versioned renderer settings

Define a host-neutral settings schema before DCC integration expands. The first
version covers backend, presentation mode, Forward/experimental path, AOV,
lighting mode, exposure/tone mapping, alpha policy, debug views, validation,
and telemetry controls.

`RendererSettings` v1 now defines that vocabulary in the backend-neutral
contract, validates exact schema and field values, and rejects backend,
presentation, experimental-path, validation, and not-yet-connected execution
choices with stable codes. The development viewport exposes the applied
contract and routes rejections through its existing revisioned feedback and
diagnostic history.
The complete field and validation contract is recorded in
[versioned renderer settings](../design/renderer-settings.md).

### ⬜ v0.15.0 — Persistent GPU Scene foundation

The first identity slice is in place: immutable snapshots assign non-zero,
source-local draw IDs that do not depend on sorted table position, retain them
across transform, visibility, material-binding, and dense resource-index
changes, and never reuse them after removal. Revisioned draw upsert/removal
deltas let a persistent consumer distinguish unchanged, replaced, retired, and
new logical draws without rebuilding identity from submission order.

The first backend-neutral residency slice is also in place. Finite GPU Scene
slots carry allocator ownership and generation on the CPU, remain unavailable
from retirement through their last referencing completion value, and advance
generation only when collected for reuse. Persistent draw residency consumes an
exact source/base-revision delta when possible, falls back to full identity
reconciliation for gaps or malformed delta indices, allocates a fresh slot for
an in-flight record replacement, and rejects stale generations, stale snapshot
revisions, foreign handles, and completion-unsafe exhaustion. CPU unit coverage
exercises allocation, retirement, collection, reuse, source changes, exact
deltas, fallback reconciliation, and failure atomicity.

The milestone remains incomplete. Next are versioned/reflected geometry,
instance, material, and draw records over these slots, dirty-range table upload,
native backend integration, and persistent Gaussian attribute residency
described in the
[GPU-driven rendering policy](../design/gpu-driven-rendering.md) and
[Gaussian rendering roadmap](../design/gaussian-rendering-roadmap.md).

## Near-term execution order

1. Keep the released v0.10.0 boundary narrow; do not broaden node coverage
   merely to claim general MaterialX. Production quality belongs to v0.18.0.
2. Build reflected GPU Scene table layouts and native uploads over the
   generation-checked draw slots and stable snapshot identity.
3. Add persistent Gaussian attribute residency and retain range-only updates.
4. Strengthen non-GPU and Linux gates before implementation breadth grows.
5. Preserve the completed native Metal presentation path while extending the
   shared viewport diagnostics and platform evidence.
