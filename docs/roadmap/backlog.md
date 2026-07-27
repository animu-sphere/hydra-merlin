# Backlog

Ordered work after the released v0.12.0 native Metal viewport presentation,
alongside active foundation work in
[current.md](current.md).
Shipped scope moves to the [changelog](../../CHANGELOG.md).

Legend: ⬜ not started

The backlog follows the product phases in the [roadmap strategy](README.md),
but a version label does not override dependency, capability, integration, or
benchmark evidence. Scope may move when that evidence changes the justified
order.

## Phase B — Cross-platform backend and presentation parity

### ⬜ v0.13.0 — HgiVulkan GPU-copy bridge

Implement `HgiVulkanBridge` in the Hydra adapter, not the renderer RHI. Create
Hgi-owned destination targets, retain Tier 0 CPU RenderBuffers as the reference
path, then copy selected color/depth/`primId`/`instanceId` AOVs from
`merlin-vulkan` images with explicit layout/ownership transitions and a distinct
bridge completion token. Validate public HgiVulkan device, texture, format,
usage, resize, target-lifetime, and host-composite contracts for each supported
OpenUSD/host version.

Exit requires CPU fallback and GPU copy to work; camera-only GPU-copy frames to
avoid CPU readback/upload; resize and frames-in-flight retirement to be safe;
unsupported configurations to report structured fallback reasons; independent
presentation telemetry; no coarse device/queue idle wait; and Tier 0 comparison
evidence. Direct native sharing and external interop are not v0.13.0 claims.

### ⬜ v0.13.1 — HgiVulkan direct-path hardening

Consider same-logical-device sharing only after physical/logical device identity,
queue ownership, format/usage, public host consumption, completion retention,
resize/destruction, and vendor/driver evidence are all verified. GPU copy remains
the fallback. External memory/semaphores require a demonstrated supported-host
need and a measured advantage over GPU copy.

### ⬜ v0.14.0 — HgiMetal bridge

Implement the equivalent HgiMetal adapter with Tier 0 CPU fallback, Metal-local
copy, and optional same-`MTLDevice` sharing. Preserve independent Metal resource
ownership and synchronization, validate SDR sRGB/Display P3 target contracts and
HDR rejection, and compare native Metal viewport and Hydra output from the same
renderer AOVs. See the [Hgi host presentation policy](../design/hgi-host-presentation.md).

## Phase C — Scene breadth and lighting quality

### Lighting ladder

Lighting evolves with the architectural rule that material evaluation produces
surface properties while the renderer owns illumination, visibility,
integration, and presentation.

1. **L0 — Diagnostic light.** Retain one or a bounded small number of
   deterministic directional lights, an explicit no-authored-light fallback,
   and no hidden dependence on host lighting.
2. **L1 — Minimal environment lighting.** Add dome/environment ingestion,
   diffuse irradiance, prefiltered specular sampling, a BRDF integration lookup,
   exposure/rotation, a deterministic fallback environment, and color-space
   diagnostics for both handwritten and generated material functions.
3. **L2 — Production viewport lighting.** After the material ABI and backend
   parity are stable, add fixture-justified distant, point, spot, and dome
   lights, transforms/visibility, documented intensity/unit interpretation,
   shadow maps, alpha-tested casters, filtering/bias policy, and debug views.
   Light linking waits for a clear Hydra contract and fixture set.
4. **L3 — Scalable evaluation.** Add tiled/clustered light assignment and GPU
   influence culling only when active local-light counts make bounded Forward
   loops inadequate and benchmarks demonstrate the need.

A lighting tier exits only when no-light behavior and supported Hydra inputs
are documented, unsupported units/features produce diagnostics, basic and
MaterialX paths agree within tolerance, lighting cost is separately observable,
representative dielectric/metal/textured/normal-mapped/mixed-light fixtures
exist, and Vulkan/Metal behavior is compared where both backends are available.

### ⬜ v0.14.1 — Gaussian MVP

Consume the existing OpenUSD Gaussian representation through Hydra and create
host-neutral Gaussian resources. Implement GPU upload, covariance evaluation,
screen-space projection, procedural splats, elliptical falloff, opacity and
spherical-harmonic appearance, CPU depth sorting, transform/visibility changes,
partial attribute upload, diagnostics, and mixed Mesh/Gaussian rendering.

The MVP displays authored Gaussian appearance, including spherical harmonics,
without forcing it into the mesh MaterialX BSDF. Future Gaussian relighting is
separate research and does not block faithful standard-data display.

Exit requires useful mixed-scene output without a custom USD schema or direct
PLY/SPLAT parser, correct CPU-sorted appearance, incremental transform and
visibility updates, and native-viewport performance evidence. GPU radix sort,
tiling, streaming, LOD, compression, and out-of-core rendering are not MVP
requirements.

### ⬜ v0.15.0 — Persistent GPU Scene and Gaussian resources

Complete the common GPU Scene ABI with persistent geometry, instance, material,
and draw records; stable object/draw/material/primitive/instance IDs; reflected
C++/shader layouts; pipeline/material sorting; descriptor-bind reduction;
secondary-command reuse; parallel recording; instance aggregation;
nested-instancing improvements; per-instance visibility; and revision-based
packet invalidation. Add generation-checked persistent Gaussian records,
changed-range updates, immutable snapshot integration, and separate Gaussian
ingestion/projection/sort/pair/raster timing.

Exit requires static Mesh scenes to generate no new draw packets, static Gaussian
scenes to allocate or upload nothing, partial Gaussian edits to upload only
changed ranges, and both paths to retain deterministic IDs, picking identity,
and completion-safe lifetime. The full Gaussian sequence is in the
[Gaussian rendering roadmap](../design/gaussian-rendering-roadmap.md).

## Phase D — GPU-driven rendering and shading scalability

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

### ⬜ v0.17.0 — Contribution-aware Gaussian culling and adaptive bounds

Add conservative Gaussian and Gaussian–tile contribution bounds, tighter
projected bounds, threshold quality modes, and diagnostics for rejection,
pair counts, saturation, and early termination. `Exact` remains the validation
path; `Balanced` requires image and temporal-tolerance evidence with no
unexplained view-dependent popping. Experimental opaque Mesh Visibility remains
a separately gated track after GPU-driven indexed Forward is stable.

### ⬜ Experimental opaque Visibility Buffer — independently gated Mesh track

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

### ⬜ v0.18.0 — Hierarchical Gaussian tiles; production MaterialX and lighting

Classify coarse Gaussian tiles, render light tiles directly, and subdivide only
heavy tiles for local pair generation and sorting. Start with a bounded two-level
path, overflow fallback, debug visualization, and resolution-specific profiles;
select it only from measured 4K/8K p95/p99 benefit. MaterialX and lighting work
continues as an independent scope:

Extend the accepted v0.10.0
[MaterialX material-function boundary](../design/materialxgenslang-boundary.md)
rather than redefining it. Broaden Standard Surface coverage with UV transforms,
tangent-space normal maps, opacity/alpha mask, emissive, and the additional
material inputs justified by fixtures. Add production environment lighting,
asynchronous compilation, prewarming, texture/sampler residency, runtime
parameter-only updates, shader sharing, and cache persistence/recovery while
preserving the v0.10.0 module/artifact/instance identity split.

Advance lighting to the justified L1/L2 scope: production environment lighting,
fixture-backed Hydra light types, shadow mapping, and consistent handwritten and
generated-material response. Do not claim a MaterialX input whose renderer
illumination behavior is undefined. Share the logical material, lighting, and
bindless resource ABI between Forward and Visibility resolve; add analytic or
validated conservative texture gradients, mip and primitive-boundary quality,
and Forward/Visibility material parity before considering material-class pixel
lists or specialized resolve dispatches.

Exit requires broader supported fixtures to remain deterministic, every
unsupported input to retain an explicit structured fallback, identical graphs
to share modules, parameter-only edits and static scenes to perform zero
steady-state compile/pipeline creation, and Forward/Visibility comparisons to
meet declared tolerances. Lighting behavior and costs must be documented,
diagnosed, and compared across Vulkan and Metal. Raw MaterialX graphs never
enter the Core scene model, and Gaussian appearance is not forced into a
MaterialX BSDF.

### ⬜ v0.19.0 — Temporal Gaussian reuse; static meshlet rendering

Reuse only compatibility-classified Gaussian preprocessing with conservative
camera margins, periodic full validation, and explicit invalidation on scene,
projection, target, LOD, or residency changes. `PreprocessOnly` must be exact;
selective color-tile reuse remains experimental. Static meshlet work continues
as an independent scope:

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

### ⬜ v0.20.0 — Gaussian LOD/compression; optional Mesh Shader, Hi-Z, and mesh LOD

Add deterministic Gaussian chunk bounds, discrete LOD, versioned compression,
full-precision fallback, and residency preparation with image/temporal evidence.
Mesh optional-path work continues as an independent scope:

Add a capability- and benchmark-selected `VK_EXT_mesh_shader` backend while
retaining indexed-indirect meshlets. Add previous-frame hierarchical-Z
occlusion, conservative history invalidation for camera/projection/resolution
changes, screen-space size selection, discrete meshlet LOD, and debug views for
culling reason and LOD.

Exit requires indexed and Mesh Shader backends to render the same supported
scenes, a repeatable win on at least one declared hardware profile before
automatic Mesh Shader selection, safe fallback everywhere else, no material
visibility errors during motion, and acceptable versioned LOD transitions.

## Phase E — Large scenes, hosts, and production readiness

### ⬜ v0.21.0 — Gaussian streaming/out-of-core rendering and parallel ingestion

Add asynchronous Gaussian chunk requests, decode/upload queues, prioritized
prefetch, completion-safe residency, eviction, temporal hysteresis, bounded
VRAM, and out-of-core rendering. Missing data retains a coarser resident LOD
or bounded proxy and never exposes uninitialized memory. Continue mesh/texture
residency as independently justified work.
Parallelize Hydra primitive processing, triangulation, primvar/Gaussian
conversion, batched RenderWorld commits, and incremental snapshot generation
with deterministic merge and low-contention queues.

Exit requires a scene larger than VRAM to stream only needed chunks with bounded
hitches and controllable quality, plus deterministic CPU scaling for large dirty
sets across available cores.

### ⬜ v0.22.0 — Gaussian cross-backend optimization and production hardening

Bring Vulkan and Metal to one Gaussian resource ABI, culling/sort semantics,
quality modes, AOV/picking contract, telemetry vocabulary, and reference
tolerance while retaining backend-specific kernels. Add capability-selected
optimization, overflow/device-loss/allocation diagnostics, cache persistence,
reproducible benchmarks, host-presentation validation, and conservative fallback.

### DCC integration order

1. usdview and `testusdview` remain the reference host.
2. Houdini Solaris viewport integration.
3. Husk batch integration.
4. Hydra 1 compatibility when demanded by a supported host.
5. Maya Hydra integration.

Before Solaris work begins, the renderer must have structured diagnostics,
OpenUSD SDK/runtime compatibility diagnostics, a versioned renderer-settings
schema, stable Hydra 2 integration, versioned capability reporting, comparable
usdview frame-stage evidence, package/discovery automation, supported AOV and
picking semantics, and documented material/lighting boundaries. Integration
packages own environment setup, discovery metadata, settings UI, package
metadata, and host smoke tests; Core and GPU backends remain independent of
every DCC SDK.

### OpenStrata composition

hdMerlin remains an OpenStrata renderer unit while runtime composition assembles
compatible OpenUSD and plugin environments. This repository owns renderer code
and evidence; FileFormat/import plugins remain independent; runtime profiles,
artifact metadata, and validated matrices express compatibility. The renderer
does not directly depend on every possible import plugin.

For Gaussian content, an importer or FileFormat plugin exposes external data as
standard USD, OpenUSD composes the stage, Hydra exposes its standard Gaussian
primitive, hdMerlin consumes that primitive, and OpenStrata assembles compatible
runtime artifacts for the session.

### ⬜ v1.0.0 — Production interactive renderer

v1.0 is gated by product contracts and evidence, not by implementing every
planned algorithm. It requires stable Hydra 2 integration; native Vulkan and
supported Apple Silicon Metal viewports; production basic and MaterialX
coverage; documented lighting and shadows; standard Gaussian rendering;
selection/picking; opaque, alpha-mask, and transparent presentation policies;
structured diagnostics/fallbacks; hardware tiers; versioned settings;
packaging/runtime composition; cross-platform correctness/performance evidence;
and at least one production DCC integration beyond usdview.

Visibility, Mesh Shaders, Hi-Z, meshlet LOD, direct zero-copy for every host,
out-of-core support for every resource, and complete MaterialX graph coverage
may remain optional at v1.0. Optional paths still require declared capability,
selection, fallback, correctness, and performance evidence.

## Post-v1 research directions

Hierarchical meshlets and virtualized geometry are not aliases for the v0.19
static meshlet path. They require separately approved designs and evidence for
parent-cluster simplification, geometric error, mixed-level traversal, crack and
transition handling, offline compressed pages, GPU residency feedback,
asynchronous disk/cache streaming, eviction, memory budgeting, and recovery from
missing pages. They do not enter a release merely because static meshlets or a
Mesh Shader backend exist.

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
- ⬜ **Build and exported products.** Add independently justified options and
  resolve `Merlin::Hydra2`/`Merlin::Headless` packaging without making OpenUSD a
  Core/Vulkan transitive dependency.
- ⬜ **OST template extraction.** Extract only boundaries proven by Merlin and a
  second consumer; keep renderer-specific extraction, material, upload,
  Gaussian, and presentation policies project-owned until separately proven.
