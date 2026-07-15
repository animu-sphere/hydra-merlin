# Backlog

Ordered work after the active v0.7.0 milestone in [current.md](current.md).
Shipped scope moves to the [changelog](../../CHANGELOG.md).

Legend: ⬜ not started

## Milestone ladder

### ⬜ v0.8.0 — Slang foundation and Vulkan parity

Audit Core, `FrameSnapshot`, viewport, presentation, and telemetry boundaries
for leaked Vulkan execution concepts. Pin Slang, add reproducible build/install
shader artifacts and dependency tracking, migrate the existing Vulkan Forward
path from GLSL pass by pass, and validate SPIR-V plus reflection metadata. Add
C++/Slang ABI contract tests, deterministic shader/permutation keys, capability
declarations, and Metal-target generation/compiler gates for the common shader
set before Gaussian, Visibility, or meshlet shader families expand.

Exit requires the Slang Vulkan path to preserve color, depth, `primId`, and
`instanceId` reference output without a material performance regression; clean
and incremental builds to produce installable versioned artifacts; reflection
to detect layout and resource-binding mismatches; common shaders to compile for
Vulkan and Metal with explicit diagnostics/fallbacks for unsupported features;
and the superseded GLSL path to be removable. This milestone does not add a
Metal renderer or a general-purpose RHI.

### ⬜ v0.9.0 — `merlin-viewport` and Vulkan presentation

Extract the minimum backend-neutral operations needed by the working Vulkan
path and an upcoming Metal bootstrap: backend factory and selection,
renderer-meaning capabilities and limits, frame submit/resolve, completion
lifetime, presentation target, common telemetry, and errors. Keep command
encoding, transitions, descriptors/argument buffers, memory, synchronization,
and native surface objects backend-owned.

Build the dedicated `merlin-viewport` application as a backend-neutral native
host for window/input, camera, resize, renderer settings, selection/picking
foundations, overlays, benchmark mode, screenshots, and USD loading through
Hydra. Its first production presentation adapter is Vulkan with a direct
swapchain path; rendering and scene behavior remain shared with headless
execution. The application is a permanent product and performance reference,
not temporary Metal bootstrap scaffolding.

Exit requires Core and Hydra public paths to expose no Vulkan/Metal types;
Vulkan to preserve existing behavior through the new contract; direct Vulkan
swapchain rendering without CPU readback; vsync-off measurements and matching
headless/viewport output; and a dedicated executable whose host and interaction
layers are reused unchanged by Metal, Mesh, and Gaussian. usdview and DCC
presentation remain separate.

### ⬜ v0.10.0 — MaterialXGenSlang prototype

Use the official MaterialX Slang Shader Generator for a deliberate prototype
covering constants, colors, images, texcoords, normals, multiply/add/mix, and a
small Standard Surface subset. Generate a cached Slang material-evaluation
module rather than a complete render pass, connect it to Vulkan Forward through
the host-neutral `MaterialIR` boundary, diagnose unsupported nodes, and keep
geometry, lights, alpha policy, resources, and AOV writes renderer-owned.

Exit requires a MaterialX document to produce a deterministic
`evaluateMaterial`-style module and cache key; the Vulkan Forward path to call
it with reference images in tolerance; and both SPIR-V and Metal-target
artifacts plus reflection metadata to be generated from the same module. Raw
MaterialX graphs do not enter Core, and this prototype is not production-wide
node coverage.

### ⬜ v0.11.0 — Metal backend bootstrap and residency

Implement the independent Metal backend with device/queue setup, buffers,
textures, samplers, pipelines, depth, offscreen rendering, CPU readback, Mesh,
camera, the basic material contract, and color/depth/ID AOVs. Add argument-
buffer resource tables, stable generation-checked slots, dirty-only table
updates, heap/residency policy, frames-in-flight retirement, capacity
diagnostics, and Metal-specific telemetry without weakening Vulkan fast paths.

Exit requires the same `FrameSnapshot` to render through Vulkan and Metal;
basic AOV differential tests to meet declared exact/tolerance rules; unchanged
resource identity to stay stable with zero needless steady-state allocation or
table update; replacement and exhaustion to remain safe and actionable; and no
resource-lifetime violation on supported Apple Silicon hardware.

### ⬜ v0.12.0 — Native Metal presentation

Add the Metal presentation adapter to the same `merlin-viewport` application
using `CAMetalLayer` and drawables, including resize, format/color-space policy,
frame pacing, viewport UI integration, presentation telemetry, and explicit
future HDR extension points. Renderer work remains in `merlin-metal`; the
shared viewport host never owns Metal command encoding or native resources.

Exit requires the macOS native viewport to render without CPU readback, share
camera/input/settings/AOV behavior with the Vulkan viewport, survive resize and
frames in flight, and preserve matching headless/offscreen reference output.

### ⬜ v0.13.0 — Hydra host presentation bridges

Implement `MetalHgiMetalBridge` as an adapter between `merlin-metal` AOVs and
the Hydra host composite, not as the renderer RHI. Validate the public HgiMetal
device, texture, format, usage, synchronization, resize, and composite contracts
for each supported OpenUSD/host version. Select direct same-device texture
sharing when supported, Metal-local texture copy otherwise, and CPU readback as
the universal fallback. Preserve independent presentation telemetry and color,
depth, `primId`, and `instanceId` semantics.

Retain Tier 0 CPU RenderBuffer readback for every backend. If measured host
costs justify it, add an independent HgiVulkan adapter using same-device image
sharing first, external memory/semaphore interop second, and GPU copy fallback
third; Vulkan and Metal synchronization and resource ownership stay backend-
specific.

Exit requires Metal AOVs to display in the reference Hydra host; GPU-copy and
CPU fallbacks to work; direct sharing to be selected where the public host
contract permits it; resize and frames-in-flight lifetime to remain safe; and
native and Hydra presentation to consume the same renderer output. Any claimed
low-copy path must avoid coarse device waits and show a measured benefit over
Tier 0. Immediate zero-copy coverage for every DCC is not required.

### ⬜ v0.14.0 — Gaussian MVP

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

### ⬜ v0.15.0 — Persistent draw and instancing

Complete the common GPU Scene ABI with persistent geometry, instance, material,
and draw records; stable draw/object/material IDs; reflected C++/shader layouts;
pipeline/material sorting; descriptor-bind reduction; secondary-command reuse;
parallel recording; instance aggregation; nested-instancing improvements;
per-instance visibility; and revision-based packet invalidation.

Exit requires static scenes to generate no new draw packets and Mesh submission
cost to be ready for indirect execution without weakening deterministic IDs,
changed-range upload, picking identity, or completion-safe resource lifetime.

### ⬜ v0.16.0 — GPU-driven rendering

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

### ⬜ v0.17.0 — Experimental opaque Visibility Buffer

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

### ⬜ v0.18.0 — MaterialX and Visibility quality

Validate and canonicalize a deliberate Standard Surface subset including base
color, metallic, roughness, normal, opacity, image, texcoord, UV transform,
normal map, constant, multiply, and add. Translate supported graphs through the
accepted MaterialXGenSlang material-function boundary, generate deterministic
SPIR-V/Metal-target reflection and cache metadata, support parameter-only
updates, and add asynchronous compilation,
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

### ⬜ v0.19.0 — Static meshlet rendering

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

### ⬜ v0.20.0 — Optional Mesh Shader, Hi-Z, and discrete LOD

Add a capability- and benchmark-selected `VK_EXT_mesh_shader` backend while
retaining indexed-indirect meshlets. Add previous-frame hierarchical-Z
occlusion, conservative history invalidation for camera/projection/resolution
changes, screen-space size selection, discrete meshlet LOD, and debug views for
culling reason and LOD.

Exit requires indexed and Mesh Shader backends to render the same supported
scenes, a repeatable win on at least one declared hardware profile before
automatic Mesh Shader selection, safe fallback everywhere else, no material
visibility errors during motion, and acceptable versioned LOD transitions.

### ⬜ v0.21.0 — Large scene, streaming, and parallel ingestion

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

Hierarchical meshlets and virtualized geometry are not aliases for the v0.19
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
  add Linux and NVIDIA/AMD coverage, with Intel when practical. Add a macOS
  Apple Silicon compile runner before the Metal compile gate and GPU evidence
  before Metal execution ships. Extend the versioned report with logical
  resource-table limits, indirect draw/count, fragment barycentric, subgroup,
  external-presentation, and Mesh Shader capabilities before the paths that
  consume them; keep API-specific facts as backend diagnostics. A missing
  runner must remain distinguishable from a product failure.
- ⬜ **Capabilities.** Extend versioned host-neutral capability reporting with
  backend feature/limit extensions and explicit unsupported/fallback states.
- ⬜ **OpenUSD compatibility.** Extend the validated 26.05 shared-SDK and MSVC
  configuration checks with runtime plugin ABI diagnostics where supported by
  the host.
- ⬜ **Shader artifacts.** Define Slang-generated SPIR-V/Metal-target manifests,
  reflection metadata, compiler/generator provenance, versioning, and cache-
  compatibility contracts.
- ⬜ **Build and exported products.** Add independently justified options and
  resolve `Merlin::Hydra2`/`Merlin::Headless` packaging without making OpenUSD a
  Core/Vulkan transitive dependency.
- ⬜ **OST template extraction.** Extract only boundaries proven by Merlin and a
  second consumer; keep renderer-specific extraction, material, upload,
  Gaussian, and presentation policies project-owned until separately proven.
