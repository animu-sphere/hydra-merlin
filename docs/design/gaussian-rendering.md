# Gaussian rendering design

**Primary backend:** Vulkan first; Metal parity follows
**Scene interface:** Hydra 2 and the standard OpenUSD Gaussian representation

## Principles

Gaussian rendering consumes Hydra primitives, attributes, transforms,
visibility, and dirty state into backend-neutral Gaussian resources. It never
parses PLY, SPLAT, or other external source formats; importers and FileFormat
plugins convert those into standard USD before Hydra.

Mesh and Gaussian paths share camera state, frame scheduling, resource-lifetime
rules, AOV/presentation infrastructure, stable GPU Scene identity, diagnostics,
and backend synchronization conventions. Projection/culling, sorting/binning,
blending/early termination, quality controls, and telemetry remain specialized.

Every advanced path retains a selectable conservative reference. `Reference`
uses CPU sorting or conservative GPU execution for validation; `Exact` permits
only mathematically safe rejection; `Balanced` is the interactive default;
`Aggressive` declares stronger quality trade-offs; and `Auto` selects only
capability- and benchmark-proven variants. No approximation becomes the only
path merely because an API exposes an optimization.

## Pipeline and dependencies

Generation-checked persistent position, covariance, opacity, and
spherical-harmonic ranges share the GPU Scene identity and completion rules.
Transform and visibility revisions update resident metadata without copying
unchanged attributes; exact particle edits copy only affected aspects and
ranges. CPU preparation and sorting remain the deterministic image reference.

The GPU path follows this dependency order:

```text
candidates -> projection -> conservative culling -> compaction -> depth key and bounds
           -> frame-wide GPU radix sort -> Gaussian-tile pairs -> stable tile grouping
           -> tile ranges -> indirect raster
           -> front-to-back accumulation and early termination
```

The path uses deterministic keys, benchmark-selected tile sizes, bounded GPU
submissions, and independently switchable stages. CPU sorting, GPU projection
plus CPU sort, GPU-sorted stream raster, and conservative flat tiling remain
diagnostic fallbacks.
Validation requires reference-tolerance parity, timestamp ranges and observable
candidate/visible/rejected/sorted/pair counts, with no CPU full traversal or
sort during camera movement.

Projection/compaction reads the tightly packed attribute ranges, evaluates
the selected projection and radiance, and retains a sort key with stable
resource/particle identity. Reusable frame contexts own the output and
counter-readback resources; completion validates candidate and rejection
partitions before publishing telemetry.

Sort every prepared record of the frame once, before tile pairing. The 64-bit
key is the order-inverted authored sort key in the high
word and a frame-global candidate index in the low word. Resources receive
their index ranges in ascending identity, so the result reproduces the CPU
reference's back-to-front, resource, particle order and never depends on
atomic compaction order. Padding sentinels sort last. The sort is a portable
8-bit LSD radix sort: a digit-major histogram whose single exclusive scan
yields every workgroup's scatter base, a multi-level block scan, and a stable
scatter that ranks equal digits within its workgroup. It uses no subgroup
operations, bindless resources, or forward-progress assumptions, and stays
within four storage-buffer bindings. Passes are skipped only for low-word
bytes the frame's candidate count cannot occupy. A verification kernel checks
strict key order and each key against its record, and accumulates an
order-sensitive identity checksum compared with the CPU reference.
Vulkan timestamps bracket the selected GPU sort, from key generation through
verification and counter readback. The separate sort duration is zero for
fallback or devices without timestamp queries.

Before tile stages exist, the verified order is rasterized directly. A gather
kernel copies the 64-byte prepared record each sorted element names into
raster order, and the thread holding the last real element writes the instance
count of one indirect draw; sentinels sort last, so no atomics are involved.
The procedural color and ID draws read those records with the same shaders and
blend state as the CPU-sorted stream, so the frame uploads no prepared stream
and the image matches the CPU reference whenever the orders match. Resolve
rejects a draw whose instance count differs from the verified sorted count.

Tile pairing sorts only by tile identity with a stable sort, which preserves
the verified depth order within every tile without widening the key. The
CPU-sorted path remains the image reference, and sorted-stream raster the GPU
fallback, when tile stages are unavailable. Delivery and support status live in the
[current milestone](../roadmap/current.md) and
[support matrix](../reference/support-matrix.md).

### Contribution-aware culling and adaptive bounds

Use conservative bounds for Gaussian and Gaussian-tile contribution to reduce
sorting, pair generation, memory traffic, and blending. `Exact` remains the
validation path. Record rejection ratios, processed pairs per pixel,
early-termination depth, saturated tiles, and threshold hits. Versioned
thresholds must meet declared image, alpha/transmittance, PSNR, SSIM, maximum
error, and temporal-flicker tolerances without view-dependent popping.

### High-resolution hierarchical tiles

Classify a coarse grid into empty, light, and heavy tiles; render light tiles
directly and subdivide heavy tiles for local pair generation and segmented or
local sorting. Begin with a fixed two-level design, bounded workgroup storage,
overflow fallback, dynamic heavy-tile queues, and 1080p/1440p/4K/8K profiles.
Choose flat versus hierarchical scheduling from measured p95/p99 benefit; do
not impose high-resolution hierarchy on small viewports where it regresses.

### Temporal Gaussian reuse

Classify camera, projection, scene, transform, attribute, visibility, and
target changes before reusing visibility, compaction, projected bounds, sort
keys, tile classification, residency, or other preprocessing. Previous
visibility is a conservative predictor, not truth: use expanded bounds,
camera-delta margins, periodic full validation, and automatic invalidation.
`PreprocessOnly` must be exact; selective color-tile reuse remains experimental
until it meets explicit temporal-error limits and is cheaper than the work it
saves.

### LOD, compression, and residency preparation

Introduce chunk bounds, deterministic discrete LOD, versioned compressed
attribute formats with full-precision fallback, and stable residency records.
LOD decisions use projected error, distance, motion, quality budget, VRAM, and
frame budget. Report compression ratios/decode cost and ensure temporal caches
invalidate LOD and residency changes safely.

### Streaming and out-of-core Gaussian rendering

Add prioritized chunk requests, decoding/upload queues, completion-safe
residency, budgeted eviction/prefetch, and temporal residency hysteresis. Only
resident chunks enter individual Gaussian work; missing data retains a coarser
resident LOD or bounded proxy and never exposes uninitialized memory. The goal
is bounded VRAM, hitches, and degradation rather than a universal FPS number.

### Cross-backend optimization and hardening

Bring Vulkan and Metal to shared Gaussian resource ABI, quality modes, sort-key
meaning, AOV/picking semantics, telemetry names, and reference tolerances while
allowing native kernels. Add capability-gated backend optimizations,
overflow/allocation/device-loss handling, cache persistence, reproducible
benchmarks, DCC-host presentation validation, and conservative fallback modes.

## Required evidence

Fixtures cover 100k–100M Gaussians, 1080p through 8K and nonstandard Hydra
viewports, sparse/dense/overdraw-heavy/imbalanced/mixed/instanced scenes,
animation and partial edits, camera navigation, and over-VRAM cases. Metrics
separate Hydra sync, dirty processing, uploads, projection, culling,
compaction, sorting, pair generation, classification, raster, temporal work,
streaming, CPU/GPU frame distributions, memory, quality, and temporal flicker.

Every advanced feature passes correctness, capability, performance, and product
gates: deterministic reference comparison, safe fallback and diagnostics,
repeatable hardware evidence including p95/p99, and versioned settings and
benchmark artifacts.
