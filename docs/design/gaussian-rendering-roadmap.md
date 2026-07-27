# Gaussian rendering roadmap

**Status:** accepted post-MVP direction
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

## Delivery sequence

### v0.14.1 — Gaussian MVP

The MVP follows HgiMetal presentation work. It adds standard Hydra ingestion,
host-neutral resources, covariance evaluation, screen-space projection,
procedural elliptical splats, opacity and spherical-harmonic appearance, CPU
depth sorting, partial attribute upload, transform/visibility updates, mixed
Mesh/Gaussian output, deterministic fixtures, and native-viewport performance
evidence. It is a correctness baseline; GPU sorting, tiling, streaming, LOD,
compression, and out-of-core residency are explicitly out of scope.

### v0.15.0 — Persistent Gaussian resources and measurement

Add generation-checked persistent GPU records for position, covariance or
scale/rotation, opacity, spherical harmonics, primitive/instance identity,
transform/visibility/attribute revisions, and residency generation. Support
transform-, visibility-, opacity-, SH-, and changed-range-only updates; avoid
rebuilding unchanged buffers; and integrate Gaussian state into immutable
`FrameSnapshot` revisions.

Exit requires zero steady-state Gaussian allocation/upload in static scenes,
range-only partial uploads, stable IDs, deterministic CPU reference output,
separate CPU/GPU stage timing, and 1M/5M/10M representative fixtures. The
common persistent GPU Scene and draw identity work shares this foundation.

### v0.16.0 — GPU-driven Gaussian baseline

Replace camera-motion CPU traversal and CPU sorting with the canonical path:

```text
candidates -> projection -> conservative culling -> compaction -> depth key and bounds
           -> Gaussian-tile pairs -> GPU radix sort -> tile ranges -> indirect raster
           -> front-to-back accumulation and early termination
```

The baseline uses deterministic 32- or 64-bit keys as required, fixed
benchmark-selected tile sizes, bounded GPU submissions, and independently
switchable stages. CPU sorting, GPU projection plus CPU sort, and conservative
flat tiling remain diagnostic fallbacks. It exits with reference-tolerance
parity, timestamp ranges and observable candidate/visible/rejected/sorted/pair
counts, and no CPU full traversal or sort during camera movement.

### v0.17.0 — Contribution-aware culling and adaptive bounds

Use conservative bounds for Gaussian and Gaussian-tile contribution to reduce
sorting, pair generation, memory traffic, and blending. `Exact` remains the
validation path. Record rejection ratios, processed pairs per pixel,
early-termination depth, saturated tiles, and threshold hits. Versioned
thresholds must meet declared image, alpha/transmittance, PSNR, SSIM, maximum
error, and temporal-flicker tolerances without view-dependent popping.

### v0.18.0 — High-resolution hierarchical tiles

Classify a coarse grid into empty, light, and heavy tiles; render light tiles
directly and subdivide heavy tiles for local pair generation and segmented or
local sorting. Begin with a fixed two-level design, bounded workgroup storage,
overflow fallback, dynamic heavy-tile queues, and 1080p/1440p/4K/8K profiles.
Choose flat versus hierarchical scheduling from measured p95/p99 benefit; do
not impose high-resolution hierarchy on small viewports where it regresses.

### v0.19.0 — Temporal Gaussian reuse

Classify camera, projection, scene, transform, attribute, visibility, and
target changes before reusing visibility, compaction, projected bounds, sort
keys, tile classification, residency, or other preprocessing. Previous
visibility is a conservative predictor, not truth: use expanded bounds,
camera-delta margins, periodic full validation, and automatic invalidation.
`PreprocessOnly` must be exact; selective color-tile reuse remains experimental
until it meets explicit temporal-error limits and is cheaper than the work it
saves.

### v0.20.0 — LOD, compression, and residency preparation

Introduce chunk bounds, deterministic discrete LOD, versioned compressed
attribute formats with full-precision fallback, and stable residency records.
LOD decisions use projected error, distance, motion, quality budget, VRAM, and
frame budget. Report compression ratios/decode cost and ensure temporal caches
invalidate LOD and residency changes safely.

### v0.21.0 — Streaming and out-of-core Gaussian rendering

Add prioritized chunk requests, decoding/upload queues, completion-safe
residency, budgeted eviction/prefetch, and temporal residency hysteresis. Only
resident chunks enter individual Gaussian work; missing data retains a coarser
resident LOD or bounded proxy and never exposes uninitialized memory. The goal
is bounded VRAM, hitches, and degradation rather than a universal FPS number.

### v0.22.0 — Cross-backend optimization and hardening

Bring Vulkan and Metal to shared Gaussian resource ABI, quality modes, sort-key
meaning, AOV/picking semantics, telemetry names, and reference tolerances while
allowing native kernels. Add capability-gated backend optimizations,
overflow/allocation/device-loss handling, cache persistence, reproducible
benchmarks, DCC-host presentation validation, and conservative fallback modes.

## Required evidence and targets

Fixtures cover 100k–100M Gaussians, 1080p through 8K and nonstandard Hydra
viewports, sparse/dense/overdraw-heavy/imbalanced/mixed/instanced scenes,
animation and partial edits, camera navigation, and over-VRAM cases. Metrics
separate Hydra sync, dirty processing, uploads, projection, culling,
compaction, sorting, pair generation, classification, raster, temporal work,
streaming, CPU/GPU frame distributions, memory, quality, and temporal flicker.

The roadmap targets are evidence goals, not guarantees: against the CPU-sorted
MVP, 1M at 1080p targets 1.5–2.5x, 5M at 4K targets 3–5x, and 10M at 4K targets
3–6x where CPU sorting dominates. Gains overlap and are never multiplied.
Every advanced feature passes correctness, capability, performance, and product
gates: deterministic reference comparison, safe fallback and diagnostics,
repeatable hardware evidence including p95/p99, and versioned settings and
benchmark artifacts.
