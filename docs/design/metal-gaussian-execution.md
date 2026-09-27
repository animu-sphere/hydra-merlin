# Metal Gaussian execution plan

**Status:** Proposed implementation sequence; not a claim of GPU-driven Metal support.
**Last reviewed:** 2026-09-27

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
The implementation uses embedded MSL and does not provide GPU preparation,
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
