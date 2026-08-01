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

## 🟨 v0.14.1 — Gaussian MVP

The ingestion and host-neutral resource slice is implemented. The render
delegate advertises Hydra's standard `particleField` Rprim, prefers float over
half attributes, applies the OpenUSD length/fallback policy, normalizes
quaternions, evaluates `R·diag(scale²)·Rᵀ` covariance, clamps linear opacity,
and retains particle-major degree-0–3 spherical-harmonic coefficients without
applying importer-specific log, sigmoid, or coordinate conversions.

Core now tracks Gaussian positions, covariance, opacity, radiance, policy,
transform, and visibility revisions. Extraction shares unchanged immutable
arrays and carries normalized changed-particle ranges for later partial GPU
upload. Rejection and fallback paths use `merlin-diagnostic/v1`.

The Vulkan path now has a deterministic CPU reference preparation stage. It
transforms positions and covariance into camera space, implements perspective
and tangential covariance projection, rejects hidden, transparent, invalid,
and off-screen particles, evaluates degree-0–3 real spherical harmonics, and
produces a stable back-to-front stream using the authored Z-depth or camera-
distance policy. Shader-ready screen centers, inverse conics, three-sigma
bounds, radiance, opacity, depth, and sort keys are retained together with
candidate, visible, rejection, fallback, cache, and sort counters. Mixed
sorting policies use one diagnosed Z-depth fallback so incomparable key domains
never enter a global blend order. Static Gaussian tables reuse their prepared
stream while camera, projection, and viewport remain unchanged.

Vulkan now uploads that prepared stream into completion-safe frame-owned
instance buffers and renders it as one instanced procedural draw. A dedicated
color subpass expands six fixed corner vertices to each conservative
three-sigma bound, evaluates the inverse-conic ellipse and Gaussian falloff per
fragment, and composites authored opacity and SH radiance back-to-front without
writing depth. Opaque Mesh depth and ID AOVs remain intact while Gaussian color
composes against the shared target. Static frames reuse both CPU preparation
and the frame-local upload; draw and upload telemetry are exposed through the
backend, benchmark report, and development viewport. The shader artifacts,
reflection, manifest identities, packaging list, and Vulkan validation/image
tests cover this raster boundary.

The external fixture selected with `MERLIN_GAUSSIAN_SAMPLE` validates both the
OpenUSD schema payload and the usdview/Hydra path. Current local evidence uses
`/Asset/Splat` from the 8192-particle SOG sample: degree 3, 131072 RGB SH
coefficients, no ingestion diagnostics, and one Gaussian snapshot resource.
The Tier 0 and forced-HgiVulkan usdview smokes retain and sort all 8192
particles, upload 425984 bytes, emit one procedural draw, produce Gaussian
color in the captured frame, report zero validation diagnostics, and reach
zero Gaussian upload bytes on a static cached frame. HgiVulkan keeps its
asynchronous GPU-copy path without a coarse wait and holds the RenderBuffer
unconverged until a completed copy makes the retained target displayable.

Remaining work is changed-range upload from normalized particle edits,
Gaussian-specific ID/picking semantics, external-fixture reference-image
evidence through usdview, and native-viewport performance evidence. The local
GPU image fixture covers ellipse rejection, falloff, alpha/SH appearance,
opaque Mesh depth composition, static upload reuse, and zero renderer-owned
Vulkan validation diagnostics; it does not replace evidence from the external
8192-particle stage.

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
