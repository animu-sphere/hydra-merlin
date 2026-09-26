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
sort during camera movement. A frame drawn from the GPU-sorted stream prepares
the CPU reference only when validation is requested: the GPU stages verify
their own order, keys, pairs, and ranges on the device, the CPU stream adds
only the divergence comparison, and the particle counters come from the GPU
preparation's partition. Frames that draw the CPU-sorted stream always
prepare it, and a stream left stale by skipped frames is prepared again
rather than reused.

Projection/compaction reads the tightly packed attribute ranges, evaluates
the selected projection and radiance, and retains a sort key with stable
resource/particle identity. A kernel is clipped by its center against the
camera's near plane, as the 3DGS reference and Mesh geometry are. Its
footprint is evaluated at the center, so a kernel between the eye and the near
plane would otherwise cover the view. The far plane keeps a conservative
three-sigma bound. The perspective Jacobian clamps the center to the reference
1.3x guard band in normalized device coordinates, which bounds the footprint
of close off-axis kernels. Reusable frame contexts own the output and
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
the verified depth order within every tile without widening the key. Tiles
are 16x16 pixels. A record covers the tiles of every pixel whose center lies
inside its conservative square, clamped to the viewport; the CPU and GPU
evaluate the same float expressions. Counts become pair offsets through the
sort's block scan, and emission writes record-major pairs whose value is the
record's sorted position, so the unsorted list is already back to front. The
sort's histogram, scan, and stable scatter kernels then run only the passes
the tile grid's index bits need, bounding their input by a device-written
element count instead of sentinel padding. A ranges pass writes each tile's
`[begin, end)` span, cleared to empty beforehand. Pairs live in a fixed
capacity; emission stops at it, so overflow drops the record-major tail and
is reported, never written out of bounds. A per-record limit, the tile count
or less, keeps every pair offset within uint32. Verification checks strict
(tile, record) order, each pair against its record's bounds and limit, and
range membership, and an order-sensitive checksum is compared with a CPU
replay of the same binning and truncation. The CPU-sorted path remains the
image reference, and sorted-stream raster the GPU fallback, when tile stages
are unavailable.

Tile raster runs in compute after the render pass, one 16x16 workgroup per
tile and one thread per pixel, compositing the tile's range front to back:
`C + T * destination` equals the back-to-front "over" blend, and the first
contributing record is the nearest one, which the ID AOVs keep. Batches of
up to 256 records are staged in workgroup memory, within the guaranteed
16 KiB, from the front of the range, and a tile stops once every pixel's
transmittance falls below 1/1024, so what remains changes color by less
than a quarter step. Each record is evaluated with the procedural fragment
stage's cutoffs and the fixed-function state around it: the conservative
square, the three-sigma ellipse, the one-UNorm-step alpha floor, clamping
to [0, 1] before blending, and the LESS_OR_EQUAL Mesh depth test with
clipping to [0, 1]. The color and ID targets become storage images and
depth a sampled image only for requests that may select tile raster.

A lost pair would drop a splat from the image, so a single-thread selection
kernel decides on the device after binning: when every requested pair was
stored and no record was clamped, it zeroes the sorted-stream draw's
instance count and publishes the frame for tile raster; otherwise the draws
rasterize the whole sorted stream and the raster dispatch exits. Resolve
fails the frame if that choice disagrees with the binning counts, and
counts overflow fallbacks. Reference parity is a tolerance, not bit
equality: blending once in float instead of once per UNorm draw moves color
by a few rounding steps, and the procedural quads interpolate offsets from
subpixel-snapped corners while compute evaluates the exact offset, so rare
pixels on a splat's cutoff rim may keep a depth-tied neighbor's particle
index. Depth and primId stay exact. Delivery and support status live in the
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
