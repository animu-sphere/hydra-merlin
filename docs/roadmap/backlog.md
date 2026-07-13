# Backlog

Ordered work after the active v0.5.1 milestone in [current.md](current.md).
Shipped scope moves to the [changelog](../../CHANGELOG.md).

Legend: ⬜ not started

## Milestone ladder

### ⬜ v0.6.0 — Incremental Hydra sync

Separate dirty locator/bit processing and retain persistent USD-path-to-Merlin
state. Add transform-only, visibility-only, and material-parameter-only fast
paths; primvar descriptor, triangulation, and indexed-primvar caches; changed
range tracking; actionable unsupported-data diagnostics; and an ingestion study
for the existing OpenUSD Gaussian representation.

Exit requires a camera-only update to perform zero geometry, topology, primvar,
and Gaussian-attribute fetch or upload and zero pipeline creation. The Gaussian
study may add an adapter or Scene Index bridge for an existing schema, but must
not introduce a renderer-specific USD schema, prim type, or attribute contract.

### ⬜ v0.7.0 — Persistent RenderWorld and GPU residency

Extend the existing handle/revision and resource-granular residency foundations
to the full Hydra-scale Mesh/Gaussian model: persistent resource tables,
generation-safe stable handles, dirty queues, changed ranges, incremental
snapshots, structural sharing, delayed retirement, geometry and Gaussian arenas,
a persistently mapped upload ring, asynchronous transfer, and explicit VRAM
budget evidence.

Exit requires static snapshot cost to stop scaling with total prim count, 100
changes in a one-million-prim scene to scale approximately with those changes,
and unchanged GPU resource addresses to remain stable. Steady state performs
zero upload, allocation, descriptor allocation, shader compilation, pipeline
creation, and CPU wait for GPU.

### ⬜ v0.8.0 — Native Vulkan viewport

Add a native window and swapchain performance reference with resize, camera
control, USD scene loading through Hydra, renderer settings, selection/picking
foundation, debug and timing overlays, benchmark mode, screenshots, and a render
path shared with headless execution.

Exit requires direct swapchain rendering without CPU readback, vsync-off
measurement, matching headless/viewport reference output, and a structure that
can host both Mesh and Gaussian pipelines. usdview and DCC presentation costs
remain separate from this renderer baseline.

### ⬜ v0.9.0 — Gaussian MVP

Consume the existing OpenUSD Gaussian representation through Hydra and create
host-neutral Gaussian resources. Implement GPU upload, covariance evaluation,
screen-space projection, procedural splats, elliptical falloff, opacity and
spherical-harmonic appearance, CPU depth sorting, transform/visibility changes,
partial attribute upload, diagnostics, and mixed Mesh/Gaussian rendering.

Exit requires useful mixed-scene output without a custom USD schema or direct
PLY/SPLAT parser, correct CPU-sorted appearance, incremental transform and
visibility updates, and native-viewport performance evidence. GPU radix sort,
tiling, streaming, LOD, compression, and out-of-core rendering are not MVP
requirements.

### ⬜ v0.10.0 — Persistent draw and instancing

Add persistent draw packets and stable draw IDs, pipeline/material sorting,
descriptor-bind reduction, secondary-command reuse, parallel recording,
instance aggregation, nested-instancing improvements, per-instance visibility,
and revision-based packet invalidation.

Exit requires static scenes to generate no new draw packets and Mesh submission
cost to be ready for indirect execution without weakening deterministic IDs or
resource lifetime.

### ⬜ v0.11.0 — GPU-driven rendering

For Mesh, add indexed multi-draw indirect, indirect count, GPU command buffers,
frustum and occlusion culling, visible-draw compaction, LOD, hierarchical Z, and
temporal visibility. For Gaussian, add compute projection, frustum/screen-size/
opacity culling, visible compaction, GPU depth-key generation and sorting, tile
binning, indirect dispatch, and early termination.

Exit requires camera movement to avoid CPU traversal of all instances or
Gaussians, Mesh CPU submission not to scale linearly with draw count, and both
render preparations to complete in a small number of GPU dispatches/draws.

### ⬜ v0.12.0 — GPU presentation interop

Retain Tier 0 CPU RenderBuffer readback as the universal reference. If measured
v0.5.1 evidence shows readback and host upload are limiting, evaluate same-device
HgiVulkan image sharing first, external-memory/semaphore interop second, and a
GPU-to-GPU copy fallback third. Specify device ownership, image layouts, queue
ownership, synchronization, capability negotiation, and safe fallback in a
presentation adapter outside Core.

Exit requires zero color/depth CPU readback and host CPU upload on the supported
path, no coarse device wait, and a measured improvement over Tier 0.

### ⬜ v0.13.0 — MaterialX

Validate and canonicalize a deliberate Standard Surface subset including base
color, metallic, roughness, normal, opacity, image, texcoord, UV transform,
normal map, constant, multiply, and add. Translate supported graphs into
MaterialIR-facing shader inputs, generate deterministic SPIR-V/reflection/cache
metadata, support parameter-only updates, and add asynchronous compilation,
prewarming, texture/sampler residency, and version-aware graph/shader keys.

Exit requires deterministic shader output, explicit structured fallback for
unsupported nodes, shader sharing for identical graphs, zero steady-state
compile/pipeline creation, and stable image comparisons. Raw MaterialX graphs
never enter the Core scene model, and Gaussian appearance is not forced into a
MaterialX BSDF.

### ⬜ v0.14.0 — Large scene, streaming, and parallel ingestion

Add chunked hierarchical Gaussian bounds, multi-resolution LOD, compressed and
quantized attributes, asynchronous streaming, eviction/prefetch, temporal
residency, mesh/texture residency, VRAM budgets, and out-of-core rendering.
Parallelize Hydra primitive processing, triangulation, primvar/Gaussian
conversion, batched RenderWorld commits, and incremental snapshot generation
with deterministic merge and low-contention queues.

Exit requires a scene larger than VRAM to stream only needed chunks with bounded
hitches and controllable quality, plus deterministic CPU scaling for large dirty
sets across available cores.

### ⬜ v1.0.0 — Production interactive renderer

Deliver stable Hydra 2 and usdview integration, production DCC packaging,
MaterialX, Gaussian rendering, selection/picking, shadows, transparency,
diagnostics, hardware tiers, compatibility and performance contracts, and a
cross-platform regression suite.

## DCC integration order

1. usdview and `testusdview` remain the reference host.
2. Houdini Solaris viewport integration.
3. Husk batch integration.
4. Hydra 1 compatibility when demanded by a supported host.
5. Maya Hydra integration.

Before Solaris work begins, the renderer must have structured diagnostics,
OpenUSD compatibility checks, a renderer-settings schema, versioned capability
reporting, and comparable usdview frame-stage evidence. Integration packages own
environment setup, discovery metadata, settings UI, package metadata, and host
smoke tests; Core remains independent of every DCC SDK.

## Cross-cutting open items

- ⬜ **GPU capability matrix.** Retain Windows Vulkan/Hydra evidence first, then
  add Linux and NVIDIA/AMD coverage, with Intel when practical. A missing runner
  must remain distinguishable from a product failure.
- ⬜ **Capabilities and diagnostics.** Define versioned host-neutral capability
  and diagnostic schemas with backend extensions and explicit unsupported/
  fallback reporting.
- ⬜ **OpenUSD compatibility.** Detect shared/static mode, Debug/Release and MSVC
  runtime mismatch, runtime bin/lib locations, and plugin ABI compatibility.
- ⬜ **Shader artifacts.** Define generated shader manifests, versioning, and
  cache-compatibility contracts.
- ⬜ **Build and exported products.** Add independently justified options and
  resolve `Merlin::Hydra2`/`Merlin::Headless` packaging without making OpenUSD a
  Core/Vulkan transitive dependency.
- ⬜ **OST template extraction.** Extract only boundaries proven by Merlin and a
  second consumer; keep renderer-specific extraction, material, upload,
  Gaussian, and presentation policies project-owned until separately proven.
