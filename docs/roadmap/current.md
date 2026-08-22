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

`RendererSettings` v2 now defines that vocabulary in the backend-neutral
contract, validates exact schema and field values, and rejects backend,
presentation, experimental-path, validation, and not-yet-connected execution
choices with stable codes. The development viewport exposes the applied
contract and routes rejections through its existing revisioned feedback and
diagnostic history. Version 2 adds GPU-driven indexed Forward selection as an
independent submission policy with disabled, preferred-with-fallback, and
required modes, forwards its visibility mask and culling switches through the
backend-neutral request, and capability-gates required execution.
The complete field and validation contract is recorded in
[versioned renderer settings](../design/renderer-settings.md).

### ✅ v0.15.0 — Persistent GPU Scene foundation

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

The common GPU Scene ABI v1 now defines 16-byte-aligned `GpuGeometry`,
`GpuInstance`, `GpuMaterial`, and `GpuDraw` records with fixed C++ size and
offset checks. One shared Slang definition is compiled to both SPIR-V and Metal,
and compiler reflection is checked field-by-field against the C++ layout. The
ABI keeps table references and geometry arena offsets 32-bit in v1; native
packing must reject unrepresentable values rather than truncate them. `GpuDraw`
stores its stable source-local 64-bit logical identity as two 32-bit words;
completion-safe physical slots remain replaceable addresses rather than stable
scene identity.

Versioned geometry, instance, and material residency is now in place over the
same finite-slot boundary. Exact snapshot resource deltas avoid full table
scans when source and base revision match; malformed indices, revision gaps,
and source changes reconcile complete identity maps. Changed records receive
fresh completion-safe slots, dense-table movement without a record-version
change preserves the resident slot, and accepted upserts are coalesced into
physical dirty ranges for bounded native copies. Geometry tracks vertex and
index revisions independently while instance and material records use their
unified revision. Resource-record changes reissue only dependency-indexed draw
records, preserving logical draw IDs while moving physical draw slots so new
frames never retain references to retired resource generations.

The first common record-packing layer is now in place over those plans.
Geometry, instance, material, and draw upserts become ABI-v1 records grouped
into ordered contiguous physical-slot ranges, so a native backend can issue one
bounded table copy per dirty range without scanning the resident table. Packing
validates source/revision continuity, record versions, resource-table kind and
residency, indexed-triangle payloads, finite transforms and bounds, explicit
32-bit object/instance identities, and texture/sampler slots. Geometry arena
offsets, ranges, counts, and primitive counts that do not fit ABI v1 are
rejected instead of truncated. Static accepted snapshots bypass candidate
cloning and input traversal, producing no packed records and zero copy bytes.
Changed geometry, instance, material, and draw mappings are evaluated as cloned
candidates and published together only after every record packs successfully.
Rejected updates leave revisions, slot generations,
retirements, telemetry, and free-slot order unchanged, so the same snapshot can
be corrected and retried. Plan packing is internal and verifies each upsert
against the candidate's current owner/generation before discarding generation
metadata for the shader-visible copy range.

The first native upload slice is now in place on Vulkan and Metal. Optional
fixed-capacity geometry, instance, material, and draw tables consume
caller-packed updates, validate snapshot and capacity continuity before
recording copies, issue one copy per contiguous dirty range, and expose exact
payload, range, capacity, reservation, and growth telemetry. Vulkan uses a
dedicated persistent staging ring for device-local tables; Metal uses private
tables with completion-safe per-frame shared staging buffers. Static accepted
updates reserve and copy nothing. Conventional Forward remains the fallback on
Metal when bindless tables are unavailable. Packed updates carry the immutable
dense draw-to-physical-slot dispatch map needed by that consumption path;
unchanged frames share it without
an identity scan, and Vulkan/Metal validate its size, capacity, and uniqueness.

Vulkan basic bindless Forward now consumes the four persistent tables. CPU
indexed submission retains geometry-buffer binding and raster state, but its
per-draw shader input is reduced to frame view state plus the packed physical
`GpuDraw` slot. Vertex and fragment stages resolve geometry attributes,
transform/normal data, stable object and instance IDs, material factors, and
bindless texture/sampler slots from the resident ABI-v1 records. Generated
materials and devices selected onto conventional descriptors keep the existing
Forward path. Reflection validates the storage-buffer/push-constant ABI, and
runtime evidence checks packed ID AOV output and continued table use on a
zero-upload static frame.

Metal bindless Forward now consumes the same four persistent tables through a
dedicated Metal pipeline. The per-draw input is reduced to view state plus the
physical draw slot; Metal vertex and fragment stages resolve the shared ABI-v1
geometry, instance, material, and draw records, including bindless texture and
sampler indices. Conventional devices and generated-material fallback retain
the existing CPU-constant path. The Metal runtime gate checks color parity,
packed ID AOV output, draw consumption, and zero-upload static-frame reuse when
a Metal device is available.

Persistent Gaussian residency now completes the milestone. Gaussian resources
participate in the common generation-checked finite-slot identity contract,
while Vulkan retains device-local position, covariance, opacity, and
spherical-harmonic ranges independently from the camera-dependent prepared
stream. Exact snapshot deltas avoid static traversal; exact particle ranges
upload only changed aspects when completion safety permits reuse. Transform and
visibility changes advance the resident record generation without re-uploading
source attributes. Runtime coverage proves zero static allocation/upload,
12-byte single-particle SH and 4-byte opacity updates, deterministic ID output,
and completion-safe range retirement. CPU preparation, attribute sync,
prepared-stream sync, and GPU raster timing are independently observable in the
backend contract, Hydra log, benchmark JSON, and viewport. Deterministic opt-in
1M, 5M, and 10M Gaussian fixtures provide the scale measurement inputs.

The implementation exit criteria and v0.15.0 release metadata are complete.
Tag publication follows main CI and is intentionally outside this worktree.

### 🚧 v0.16.0 — GPU-driven rendering

The first backend-neutral indexed-draw reference is in place. It consumes
persistent physical `GpuDraw` slots, validates geometry, material, and instance
residency, applies independently selectable visibility-mask and conservative
zero-to-one clip frustum culling, preserves the physical draw slot through
compaction, and generates native-neutral indexed-indirect command batches.
Vertex and index arena block identity remains outside the shader-visible scene
record and partitions commands into batches with one bindable buffer pair, so
block-local byte offsets never alias after an arena grows. Exact candidate,
visible, per-stage rejection, command, and batch counts make the result suitable
as the CPU correctness oracle for the native compute path. Malformed identities,
ranges, bounds, transforms, duplicate candidates, missing block residency, and
unrepresentable arena offsets are rejected before a partial plan is returned.

The first Vulkan compute contract is also in place. A packaged Slang compute
artifact consumes persistent geometry, instance, and draw tables plus a native
arena-batch candidate list, applies the same visibility-mask and conservative
zero-to-one clip-frustum rules as the CPU oracle, and emits a compact
`VkDrawIndexedIndirectCommand`-compatible stream, per-candidate results, and
bounded dispatch counters. Reflected push-constant, storage-binding, counter,
and command layouts are fixed in shader ABI v5. The current kernel classifies
candidates in 64-thread workgroups and atomically compacts visible commands and
rejection counters; command order within a batch is not an identity boundary
because the command stream encodes the persistent physical draw slot in
`firstInstance`. Owned Vulkan devices explicitly enable and report
`drawIndirectFirstInstance`; borrowed contexts must declare that the host
enabled it before the runtime path may be selected.

The first opt-in Vulkan renderer execution slice is now in place for one native
arena/pipeline batch. It uploads persistent draw-slot candidates, dispatches
the reference compute kernel into device-local result, indirect-command, and
count buffers, synchronizes compute writes into
`vkCmdDrawIndexedIndirectCount`, and carries the physical draw slot through
`firstInstance` into a dedicated table-backed Forward shader. Owned devices
enable and report `drawIndirectCount` and `shaderDrawParameters` alongside
`drawIndirectFirstInstance`; borrowed hosts must explicitly declare all three
before the path can be required. Runtime telemetry reports candidate, visible,
per-culling-reason, indirect-draw, candidate-upload, and fallback counts, and a
validation-enabled Vulkan test covers visible color/ID output plus a zero-count
visibility-mask-culling submission on the asynchronous-transfer path.

Candidate draw-slot sequences now remain resident in each reusable Vulkan frame
context. An unchanged sequence skips staging reservation and transfer even when
camera or culling inputs change, while a changed or capacity-growing sequence is
uploaded before compute dispatch. Candidate-count telemetry remains populated
on both paths and candidate-upload bytes distinguish reuse from replacement.

Vulkan GPU-driven execution now partitions the stable draw sequence into
consecutive arena/pipeline batches. Each frame context owns one aligned shared
candidate, result, indirect-command, counter, and counter-readback buffer plus
one descriptor pool; batches address bounded ranges rather than creating native
memory allocations per draw group. Each batch receives one compute dispatch,
binds its vertex/index arena blocks and raster pipeline, and submits one
indexed-indirect-count draw. Candidate uploads from all changed or relocated
batches share a single staging-ring reservation, unchanged per-frame-context
batch sequences remain zero-upload, and resolved culling counters are validated
per batch before being accumulated for frame telemetry. Validation-backed
coverage proves both mixed-pipeline output and a 32-batch alternating-state
scene with buffer and descriptor-pool allocations equal to a one-batch control.

Candidate classification and visible-command compaction now execute in
64-thread compute workgroups. Each invocation applies the reference culling
rules and atomically reserves its compact indirect-command slot while retaining
the persistent physical draw slot in `firstInstance`; per-batch counters are
cleared on the GPU before dispatch. Validation-backed coverage exercises 130
candidates across multiple workgroups with exact visible and visibility-mask
rejection counters. Batch selection rejects or falls back before dispatch when
the required group count exceeds the device's
`maxComputeWorkGroupCount[0]` limit.

The first opt-in large-scene command-recording fixture now measures the same
shared indexed geometry through conventional and required GPU-driven
submission at 1,000, 10,000, and 100,000 instances. It warms every reusable
frame context before capturing steady-state timing, requires zero static GPU
Scene/candidate upload and descriptor rewrite, and retains candidate, visible,
rejection, and native indirect-draw counters alongside `command_recording_ns`.
Unsupported devices fail the explicit capability request instead of recording
fallback as GPU-driven evidence. Exact color, depth, primitive-ID, and
instance-ID output must match the conventional baseline at every tier. This
establishes a reproducible command-buffer-recording slope contract, not a claim
that total CPU preparation is draw-count independent: native batch preparation
remains visible in `gpu_scene_update` and `total_frame`. Controlled hardware
captures, draw-count-independent preparation, and broader geometry/material
diversity remain evidence follow-up.

The first Gaussian compute preparation contract is now packaged. A 64-thread
Slang kernel reads the persistent position, covariance, opacity, and
spherical-harmonic arena payloads without a CPU-prepared candidate stream,
performs projection, conservative culling, radiance evaluation, and atomic
visible-record compaction, and retains resource/particle identity plus the
authored sorting key for later stages. Shader ABI v6 reflection-checks its
uniform-backed constants, descriptors, 64-byte prepared record, and rejection
counters without exceeding the Vulkan push-constant baseline. This is an
artifact and ABI boundary only: runtime dispatch, deterministic radix
sorting, Gaussian-tile pairing/ranges, indirect raster, and reference-image
evidence still retain the CPU-sorted fallback.

The Gaussian compute preparation path remains incomplete. This slice is
therefore not the complete v0.16.0 support claim.

## Near-term execution order

1. Keep the released v0.10.0 boundary narrow; do not broaden node coverage
   merely to claim general MaterialX. Production quality belongs to v0.18.0.
2. Strengthen non-GPU and Linux gates before implementation breadth grows.
3. Preserve the completed native Metal presentation path while extending the
   shared viewport diagnostics and platform evidence.
