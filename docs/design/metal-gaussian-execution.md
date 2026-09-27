# Metal Gaussian execution plan

**Status:** Phase 2 sorted-stream renderer integration available; host validation, broader measurements and Phase 3 tile raster remain open.
**Last reviewed:** 2026-09-28

Move Gaussian projection, culling, sorting and rasterization onto the Metal GPU
without making CPU work proportional to particle count during camera motion.
Use Slang as the shared shader source while keeping allocation, synchronization,
command encoding and presentation native to each backend.

This document records the Metal implementation strategy and acceptance gates.
The [Gaussian rendering design](gaussian-rendering.md) owns the common math,
sort, tile, image and fallback contracts. The
[multi-backend strategy](multibackend-slang-materialx.md) owns shader/backend
boundaries; [execution lifetime](execution-lifetime.md) owns completion safety.
Actual support belongs in the [support matrix](../reference/support-matrix.md),
active work in the [current roadmap](../roadmap/current.md), and inactive work
in the [backlog](../roadmap/backlog.md). Phase numbers below are dependency
order, not release commitments or a decision to start implementation.

## Starting point

[PR #107](https://github.com/animu-sphere/hydra-merlin/pull/107) adds the Metal
CPU-sorted reference path: shared CPU covariance projection, SH evaluation and
sorting, followed by Metal ellipse rasterization. It blends against opaque
mesh depth, writes resource/particle IDs, and reuses an immutable prepared
stream on unchanged frames. Camera or particle changes rebuild that stream.
The initial implementation used embedded MSL and did not provide GPU preparation,
sorting or tile rasterization. Preserve this path as the correctness reference
and capability fallback throughout the work below.

Vulkan already has Slang implementations of GPU preparation, deterministic
radix sorting, sorted-stream indirect raster, tile binning and compute tile
raster. Reuse their algorithms and contracts instead of creating a separate
Metal Gaussian model. Their GPU scheduling and resource APIs remain Vulkan
implementation details, not an RHI to reproduce in Core.

A local first-frame capture of a 5,834,784-particle USD stage at 1920x1200
reported approximately 818 ms CPU preparation, 37 ms prepared-stream writing
(303,408,768 bytes), and 216 ms total GPU execution. These are one-frame
observations, not steady-state or camera-motion benchmarks, and do not isolate
GPU raster cost. On Apple unified memory, prepared-stream writing is CPU
memory work; the byte counter is not evidence of a discrete-GPU bus transfer.
These observations motivate measuring both preprocessing and rasterization.
The private scene is not a distributable regression fixture.

## Phase 1: Measurement and shared Slang shaders

The reference raster now shares position expansion and ellipse/alpha evaluation
with Vulkan through Slang. Metal-specific entry points preserve the scalar
52-byte stream, Y convention and combined color/ID attachments. Slang 2026.8.x
emits MSL/reflection, Xcode produces an embedded metallib, and the versioned
install package retains those artifacts and their identity/checksum evidence.
Projection, covariance, SH evaluation, culling/compaction and portable radix-sort
kernels now also live in shared Slang sources, with backend-owned bindings and
record access. Metal uses explicit buffer slots and byte-addressed 64-byte
prepared records because native MSL `float3` storage has different padding.
The embedded/installed library includes these compute entry points. Preparation
preserves non-finite classification with native Metal fast math disabled.

Host layout assertions, generated binding checks and local Apple GPU tests
compare preparation and sorting with the CPU reference, including SH degrees
0–3, projection/sorting policies, nonidentity transforms, culling boundaries,
multiple resources, hierarchical scans, device-written counts and empty input.
The kernel harness allows absolute error of `2e-4 * max(1, abs(reference))`
per floating-point field; classifications, counters, identities and sentinel
placement must match exactly. Intentional distance ties use binary-exact input
coordinates so rounding of nearly equal CPU/GPU keys is not mistaken for a
radix-sort error. These checks do not establish renderer image parity or a
general exact ordering guarantee for numerically near-equal keys.
The compute test now submits preparation, sorting, gather and indirect ellipse
raster in one command buffer without an intermediate CPU readback. Metal gather
converts the 64-byte prepared records into the existing scalar 52-byte raster
stream and writes all four `MTLDrawPrimitivesIndirectArguments` words, including
zero instances for empty/all-rejected frames. Padded streams use the last valid
sorted element to publish the count; dynamic streams use a GPU-written count
bounded by capacity. Each invocation requires at least one workgroup, even for
zero elements. The output capacity must cover the sorted element capacity, and
sorted record indices must refer to the preparation buffer from that frame.
The existing sentinel-boundary algorithm is retained from Vulkan; Metal owns
its packing and argument bindings.

Gather checks require bit-exact field copies, draw arguments and untouched
sentinel/guard tails. Offscreen comparisons against CPU-prepared direct draws
allow at most 2/255 absolute error per RGBA8 channel and require exact depth and
resource/particle IDs. These fixture-specific image checks cover SH degrees,
projection/sort policies, transforms, multiple resources, hidden/all-rejected
input, workgroup boundaries, asymmetric alpha composition and a prefilled opaque
depth attachment. They do not establish general scene or host-presentation
parity. The harness isolates kernel correctness; Phase 2 below now also connects this
path to renderer frame contexts, persistent attributes and completion telemetry. Controlled performance captures
and updated native viewport/HgiMetal comparisons remain unfinished; this is
not completion of the phase gate below.

Establish repeatable static, camera-motion and particle-edit captures before
changing execution. Use the existing public Gaussian corpus and deterministic
scale fixtures; keep private scenes as optional local checks. Record the GPU,
OS, compiler/SDK versions, commit, physical render extent, particle count,
selected path and requested/read-back AOVs with each result.

Move Gaussian shader ownership out of the Vulkan backend as those shaders
become shared. Generate SPIR-V for Vulkan and MSL for Metal using the pinned
Slang toolchain. Compile Metal artifacts with Apple's compiler and install
them through the existing shader packaging contract. Do not add a Slang or
Metal SDK dependency to Core-only builds.

Share the projection, conic, SH, opacity and sorting calculations. Keep
backend-specific entry points or wrappers where input layout, coordinate
conventions, resource binding or attachment outputs differ. Match semantic
results rather than requiring identical GPU command sequences.

Validate generated resource bindings, offsets, strides, matrix layout and
entry points against the host records. Metal buffer bindings must not assume
that Vulkan descriptor sets or separate resource register classes map directly.
Use the [Slang Metal target documentation](https://shader-slang.org/slang/user-guide/metal-target-specific)
as a reference, but prove compatibility with the repository's pinned compiler.

**Gate:** Both targets compile, installed artifacts are discoverable, and the
Slang-generated Metal reference path passes color/depth/ID, orientation,
anisotropy, composition and host-presentation comparisons. Source consolidation
must not change the image or introduce per-frame shader compilation.

## Phase 2: Persistent attributes and GPU execution through raster

Retain position, covariance, opacity and SH attributes in renderer-owned GPU
buffers. Reuse generation-checked resource identity and immutable snapshot
revisions. Apply only changed attribute ranges; transform and visibility edits
update metadata. Replaced buffers and slots remain alive until their last GPU
consumer completes. Bound resident and scratch allocation and report failures
through the existing diagnostic/fallback contract.

The private Metal attribute store now supplies the compute/image harness with
immutable device-local position, covariance, opacity and SH buffers. It keys
reuse by source identity, the complete resource handle, attribute revisions and
shared payload identity. Camera, transform and visibility changes reuse those
buffers. Matching particle-base revisions and unchanged layouts permit staged
range updates; a GPU copy creates the new attribute version before patching it,
so earlier submissions can continue reading the old version. Revision gaps,
source changes, count changes and SH layout changes upload complete affected
attributes. Unchanged attributes retain their original buffers.

Preparation is transactional, and a separate commit publishes the resident
scene only after submission. Command completion retains staging and old
versions, including their contribution to an explicit live-byte budget.
Allocation failure leaves the previous scene usable. The caller must invalidate
residency and report a failed upload command before scheduling dependent work.
The harness checks actual bytes, partial SH ranges, removal/generation reuse,
abandoned/invalid updates, budget exhaustion and multiple blocked submissions.
Continuous image comparisons check static, camera, transform, localized edits,
visibility, removal and reintroduction with the existing color/depth/ID tolerance.
This backend-private store is now consumed by renderer GPU submissions with
post-submit publication and upload-failure invalidation. It scans
resource metadata, and partial updates currently copy the full changed attribute
on the GPU; it does not yet implement a resource-delta fast path or an arena.

The private execution helper now owns preparation, key generation, hierarchical
radix passes and gather encoding. It consumes immutable residency metadata and
camera constants, and returns a lease on device-local prepared/sorted records,
classification/counter buffers, raster instances and indirect arguments. The
caller encodes attribute uploads first, then execution and raster in the same
retaining command buffer, and publishes residency only after submission. The
helper does not submit, wait, read counts back or traverse particle payloads.
Scratch allocations contribute to frame allocation telemetry; cumulative
compute dispatches and live resident/scratch bytes are exposed by Metal statistics.

Scratch reuse requires both GPU completion and release of the caller's frame
lease. Cached, externally retained and in-flight allocations share a bounded
live-byte budget, including after reset or helper destruction. Allocation
failure releases partial scratch, and an encoding failure requires discarding
the unsubmitted command. An abandoned command releases its completion lease
when destroyed. GPU clears reset counters before reuse; gather always writes
all indirect arguments, including empty frames. Diagnostic mode adds sorted
order checks and poisoned output guards without CPU-dependent scheduling.
The image harness now uses this helper and checks zero new GPU allocations on
static/camera frames, reuse across extent changes, concurrent output isolation,
retained completed outputs, abandoned commands, reset and budget exhaustion.
The renderer now retains output leases until resolve, while command completion
retains independent GPU leases. Small per-resource counters and indirect
arguments are copied to shared frame buffers and inspected only after completion;
no prepared records or particle attributes are read back. Normal execution does
not run the CPU preparation reference. Validation enables GPU order checks.

`Disabled` preserves the CPU reference and its static prepared-stream cache.
`Prefer` and `Require` select GPU sorted-stream execution on devices meeting the
portable workgroup limits. Tiled is still unavailable: Prefer falls back to
GPU sorted-stream and counts the stage fallback; Require returns Unsupported.
Attribute/scratch address limits or exhausted configured budgets select CPU
fallback in Prefer and raise the original error in Require. Failed partial
encoding discards the unsubmitted command; submission catches C++ exceptions
inside its autorelease pool so abandoned command leases are released.

Attribute and scratch pools each default to a separate 1 GiB live-byte budget,
configurable through Metal BackendOptions. These are independent of the
mesh/texture heap and include retained/in-flight allocations. Frame telemetry
reports attribute bytes/ranges/generations, allocation counts, candidate/cull/
sorted/indirect counts and fallback counts. Metal statistics additionally expose
live pool bytes, cumulative compute dispatches and GPU attribute-version copy
bytes. Telemetry readback is 32 bytes plus 32 bytes per resource, separate from
requested image readback, and is included in readback bytes. CPU preparation,
attribute synchronization, prepared-stream writing and command recording times
are separated; Gaussian raster time still measures CPU encoding.

A failed attribute-upload command invalidates residency and dependent queued
frames before the next use, using the existing serial-queue failure boundary.
Renderer image tests cover partial edits, camera motion, transforms, resize,
visibility, removal/reintroduction, multiple unresolved frames and AOV leases.
Budget failures test both policy modes and recovery on the same backend;
actual device command failure injection remains unverified.

Local Release captures on Apple M3 cover public deterministic 65,536- and
1,048,576-particle fixtures at 1024x768, with cold, static, camera and range-edit
frames and no image readback. GPU camera/edit frames avoid CPU particle
preparation and full uploads; warmed camera frames allocate no GPU buffers.
Static GPU frames still recompute, so the CPU cached stream may be faster.
Run `merlin-metal-gaussian-tests --benchmark` for CSV samples; this is optional
hardware evidence rather than a timing-sensitive CTest gate. A local
5,834,784-particle, SH-degree-3 Garden stage was also displayed through the native
Metal viewport and usdview/HgiMetal GPU copy, with zero CPU preparation and
steady attribute uploads. Its initial attribute upload needs a 3 GiB residency
budget including staging. Hydra environment variables and native viewport CLI
options expose the independent residency/scratch limits; see the build guide.
The usdview check also exercised Tiled Prefer fallback to GPU sorted-stream.
These interactive checks do not establish general host image parity or
controlled performance. Public corpus comparisons, stage GPU timestamps and
wider hardware evidence remain open.

Connect the complete frame path on the GPU:

```text
resident attributes + camera/resource metadata
  -> projection, conservative culling, SH evaluation and compaction
  -> deterministic depth/identity keys
  -> frame-wide radix sort
  -> sorted-stream gather + GPU-written draw arguments
  -> indirect ellipse raster + color/ID outputs
```

Start with the existing portable radix-sort algorithm, which avoids subgroup
operations and inter-workgroup forward-progress assumptions. Prove its Metal
result before introducing SIMD-group-specific optimizations. Keep deterministic
resource/particle tie-breaking and the shared mixed-sort-policy fallback.

Do not read visible counts or sorted records back to the CPU to schedule the
next stage. GPU projection followed by CPU sorting is useful as a diagnostic
step, but is not the deliverable. CPU reference preparation runs only when
explicitly requested for comparison or when a fallback selects it. Small
telemetry readbacks must not introduce a dependency between GPU stages.

**Gate:** On camera-only frames, CPU particle traversal/sorting and prepared
stream uploads are zero. Source attributes remain resident. Dispatch/draw
counts are bounded by the algorithm's passes and resource batches, not one
submission per particle. Images and IDs meet the reference contract through
motion, resize, resource edits/removal and frames in flight. Capability and
settings telemetry distinguish CPU fallback from actual GPU execution.

## Phase 3: Tile rasterization and measured Apple GPU tuning

Port the existing stable tile binning/ranges and front-to-back compute raster
path. Preserve mesh depth tests, ellipse/alpha cutoffs, color and picking
semantics, bounded pair storage and the declared image tolerance for early
termination. When tile capacity is insufficient, select the complete
GPU-sorted raster stream on the device; never silently drop particles.

Begin with the existing portable tile/workgroup configuration as a baseline.
Then measure tile size, threadgroup occupancy, shared-memory use, sorting
traffic and attribute/SH bandwidth on the target Apple GPUs. Change native
storage modes, layout or SIMD-group kernels only when the evidence identifies
the relevant bottleneck and correctness comparisons still pass. Sharing Slang
source does not require identical backend tuning parameters.

**Gate:** The tile path improves measured raster or end-to-end costs on the
intended scenes without unexplained image/temporal regressions. Overflow,
large projected splats, empty scenes and near-plane cases retain correct
fallbacks. Prefer the sorted-stream path when the tile path is unsupported or
measurably unsuitable; do not claim a universal FPS improvement.

## Validation and evidence

Retain the current Metal image tests and portable CPU preparation suite.
Extend them with CPU/GPU comparisons for sorting ties, SH degrees, projection
and sorting policies, mixed meshes, visibility, changed particle ranges,
resource removal/reuse, resize and unresolved earlier submissions. Include
near/far-plane boundaries and tile overflow. Use the existing shared tolerance
contract; document any additional tolerance before accepting a new path.

Measure first-frame setup separately from warmed static frames, continuous
camera motion, localized particle edits and resize. Record median and tail
latency, visible/rejected/sorted/pair counts, allocations, resident/scratch
bytes, attribute/prepared uploads, readbacks and completion waits. Separate CPU
preparation/encoding times from GPU projection/sort/binning/raster timestamps.
In particular, the current Metal `gaussian_raster_ns` measures CPU command
encoding, not device raster duration; total GPU execution cannot replace
per-stage timestamps for tuning.

Verify offscreen rendering first, then native viewport presentation, then
usdview/HgiMetal GPU-copy and Tier 0 fallback. Keep diagnostic readback modes
separate from normal interactive measurements. Continue Vulkan regression
coverage whenever shared Slang or CPU reference code changes.

Hosted compile/package checks and Windows self-hosted Vulkan runs do not
establish Metal GPU runtime coverage. Retain explicit Apple GPU runtime/image
evidence; add a suitable macOS runner or a reproducible local evidence process
before using Metal performance as a CI gate. Store dated raw measurements
under the repository's [report policy](../reports/README.md), with stable
summaries in reviewed PRs/release records as appropriate.

## Delivery and deferred work

Use three reviewable delivery groups matching the phases above. Phase 2 may
need smaller dependent PRs for residency, preparation and sorting, but is only
complete once it produces the image without the CPU particle loop. Each group
includes its correctness tests, capability/fallback behavior, packaging and
measurement evidence.

After the complete GPU path is measured, consider contribution-aware culling,
hierarchical tiles, temporal reuse, LOD, compression and streaming. Do not
start these to hide an unmeasured basic-path bottleneck, and do not turn an
approximation into the only supported path.

Keep this document as the rationale and dependency map. Track only unfinished
work in the roadmap, and record shipped behavior in the changelog/support
matrix. Update this plan when evidence changes the ordering or acceptance
criteria instead of copying competing plans into new files.
