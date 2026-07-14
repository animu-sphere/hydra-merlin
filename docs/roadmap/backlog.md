# Backlog

Ordered work after the active v0.7.0 milestone in [current.md](current.md).
Shipped scope moves to the [changelog](../../CHANGELOG.md).

Legend: ⬜ not started

## Milestone ladder

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

Complete the common GPU Scene ABI with persistent geometry, instance, material,
and draw records; stable draw/object/material IDs; reflected C++/shader layouts;
pipeline/material sorting; descriptor-bind reduction; secondary-command reuse;
parallel recording; instance aggregation; nested-instancing improvements;
per-instance visibility; and revision-based packet invalidation.

Exit requires static scenes to generate no new draw packets and Mesh submission
cost to be ready for indirect execution without weakening deterministic IDs,
changed-range upload, picking identity, or completion-safe resource lifetime.

### ⬜ v0.11.0 — GPU-driven rendering

For Mesh, establish GPU-driven indexed Forward with candidate draw records,
frustum/visibility-mask culling, visible-draw compaction, indexed multi-draw
indirect, indirect count, and debug-selectable CPU/GPU culling. Occlusion,
meshlet expansion, and LOD are deliberately later gates. For Gaussian, add
compute projection, frustum/screen-size/opacity culling, visible compaction, GPU
depth-key generation and sorting, tile binning, indirect dispatch, and early
termination without sharing the Mesh primitive algorithm.

Exit requires camera movement to avoid CPU traversal of all instances or
Gaussians, Mesh CPU submission not to scale linearly with draw count, and both
render preparations to complete in a small number of GPU dispatches/draws.
Conventional Forward output remains the image reference, and candidate/visible
counts plus culling cost are retained as benchmark evidence.

### ⬜ v0.12.0 — Experimental opaque Visibility Buffer

Add an opt-in Visibility path for supported opaque, static, indexed triangle
meshes. Rasterize stable draw and primitive IDs to `R32G32_UINT` plus depth,
reconstruct geometry/instance/material data through the common GPU Scene, and
resolve a bounded basic-PBR subset to HDR color in compute. Begin with explicit
LOD or mip 0, retain Forward for transparent and unsupported content, and keep
Gaussian and overlay passes specialized.

Exit requires correct draw/primitive and picking identity, transforms, normals,
UVs, and multiple materials/instances; deterministic Forward differential
images; explicit pass barriers/timestamps; and separate visibility-raster and
material-resolve evidence. Visibility remains a selectable path, not the new
universal renderer.

### ⬜ v0.13.0 — GPU presentation interop

Retain Tier 0 CPU RenderBuffer readback as the universal reference. If measured
v0.5.1 evidence shows readback and host upload are limiting, evaluate same-device
HgiVulkan image sharing first, external-memory/semaphore interop second, and a
GPU-to-GPU copy fallback third. Specify device ownership, image layouts, queue
ownership, synchronization, capability negotiation, and safe fallback in a
presentation adapter outside Core.

Exit requires zero color/depth CPU readback and host CPU upload on the supported
path, no coarse device wait, and a measured improvement over Tier 0.

### ⬜ v0.14.0 — MaterialX and Visibility quality

Validate and canonicalize a deliberate Standard Surface subset including base
color, metallic, roughness, normal, opacity, image, texcoord, UV transform,
normal map, constant, multiply, and add. Translate supported graphs into
MaterialIR-facing shader inputs, generate deterministic SPIR-V/reflection/cache
metadata, support parameter-only updates, and add asynchronous compilation,
prewarming, texture/sampler residency, and version-aware graph/shader keys.
Share the bindless resource ABI between Forward and Visibility resolve; add
analytic or validated conservative texture gradients, normal mapping, emissive,
environment lighting, and alpha-mask quality before considering material-class
pixel lists or specialized resolve dispatches.

Exit requires deterministic shader output, explicit structured fallback for
unsupported nodes, shader sharing for identical graphs, zero steady-state
compile/pipeline creation, stable Forward/Visibility image comparisons, and
acceptable mip and primitive-boundary behavior. Raw MaterialX graphs never enter
the Core scene model, and Gaussian appearance is not forced into a MaterialX
BSDF.

### ⬜ v0.15.0 — Static meshlet rendering

Build material-homogeneous meshlets internally from standard Hydra mesh data
after triangulation and `GeomSubset` partitioning. Add deterministic local
indices, configurable vertex/primitive limits, bounds and normal cones,
in-process build caching and dirty-state invalidation, instance-then-meshlet
frustum/cone culling, visible meshlet compaction, and an indexed-indirect
meshlet backend compatible with Forward and Visibility. Keep small, dynamic,
deformed, skinned, unstable-topology, and unsupported geometry on conventional
paths when appropriate.

Exit requires deterministic builder/cache behavior; correct topology, points,
primvar, and material-partition invalidation; conventional/Visibility image
parity; visible and rejected meshlet statistics; and a measured reduction in
processed triangles or frame cost on the large-static-mesh fixture without a
regression on small-mesh fallback fixtures.

### ⬜ v0.16.0 — Optional Mesh Shader, Hi-Z, and discrete LOD

Add a capability- and benchmark-selected `VK_EXT_mesh_shader` backend while
retaining indexed-indirect meshlets. Add previous-frame hierarchical-Z
occlusion, conservative history invalidation for camera/projection/resolution
changes, screen-space size selection, discrete meshlet LOD, and debug views for
culling reason and LOD.

Exit requires indexed and Mesh Shader backends to render the same supported
scenes, a repeatable win on at least one declared hardware profile before
automatic Mesh Shader selection, safe fallback everywhere else, no material
visibility errors during motion, and acceptable versioned LOD transitions.

### ⬜ v0.17.0 — Large scene, streaming, and parallel ingestion

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

## Post-v1 research directions

Hierarchical meshlets and virtualized geometry are not aliases for the v0.15
static meshlet path. They require separately approved designs and evidence for
parent-cluster simplification, geometric error, mixed-level traversal, crack and
transition handling, offline compressed pages, GPU residency feedback,
asynchronous disk/cache streaming, eviction, memory budgeting, and recovery from
missing pages. They do not enter a release merely because static meshlets or a
Mesh Shader backend exist.

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
  add Linux and NVIDIA/AMD coverage, with Intel when practical. Extend the
  versioned report with descriptor indexing/table limits, indirect draw/count,
  fragment barycentric, subgroup, and Mesh Shader capabilities before the paths
  that consume them. A missing runner must remain distinguishable from a product
  failure.
- ⬜ **Capabilities.** Extend versioned host-neutral capability reporting with
  backend feature/limit extensions and explicit unsupported/fallback states.
- ⬜ **OpenUSD compatibility.** Extend the validated 26.05 shared-SDK and MSVC
  configuration checks with runtime plugin ABI diagnostics where supported by
  the host.
- ⬜ **Shader artifacts.** Define generated shader manifests, versioning, and
  cache-compatibility contracts.
- ⬜ **Build and exported products.** Add independently justified options and
  resolve `Merlin::Hydra2`/`Merlin::Headless` packaging without making OpenUSD a
  Core/Vulkan transitive dependency.
- ⬜ **OST template extraction.** Extract only boundaries proven by Merlin and a
  second consumer; keep renderer-specific extraction, material, upload,
  Gaussian, and presentation policies project-owned until separately proven.
