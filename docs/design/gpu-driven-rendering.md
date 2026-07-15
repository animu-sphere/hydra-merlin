# GPU-driven rendering policy

**Status:** approved direction, not an implementation claim · **Last reviewed:**
2026-07-14

This document is the design source of truth for bindless resources, the GPU
Scene, GPU-driven indexed drawing, the opaque Visibility Buffer path, and
meshlet rendering. The [renderer architecture](renderer-architecture.md)
defines the repository-wide dependency boundary; this document defines how the
Mesh pipeline evolves inside that boundary.

The implementation order is deliberately incremental:

```text
measurement and incremental sync
    → bindless resource foundation
    → persistent GPU Scene and stable draw identity
    → GPU-driven indexed Forward
    → experimental opaque Visibility Buffer
    → Visibility quality and MaterialX integration
    → static meshlet build and indexed-indirect rendering
    → optional Mesh Shader, Hi-Z, and discrete LOD
    → separately approved hierarchy and virtualized geometry work
```

Each step must be independently selectable, measurable, and comparable with the
existing Forward reference. A later path does not remove an earlier fallback.

## Decisions and boundaries

- Hydra continues to expose standard mesh topology, points, primvars,
  `GeomSubset` material bindings, and instancing. The adapter normalizes those
  values into `RenderWorld`; it does not allocate descriptors, pack visibility
  IDs, generate Vulkan commands, or build meshlets.
- Bindless is the resource ABI for future paths, not merely a descriptor-bind
  optimization. Texture, sampler, material, geometry, instance, and draw
  identity must be usable by Forward, Visibility resolve, and meshlet paths.
- GPU-driven indexed Forward is established before Visibility or meshlets. It
  proves stable draw IDs, culling, compaction, and indirect submission without
  changing shading at the same time.
- Visibility is an additional path for opaque indexed triangle meshes. Forward
  remains the correctness reference and handles unsupported materials,
  transparent geometry, small/dynamic geometry when appropriate, and debug
  fallback.
- Meshlets are an internal derived representation, not a USD schema. Meshlet
  data and Mesh Shader execution are separate decisions: indexed indirect is
  the first meshlet backend and `VK_EXT_mesh_shader` is optional.
- Mesh and Gaussian pipelines share resources and frame conventions, not
  primitive encoding or raster algorithms. Gaussian rendering never writes a
  triangle visibility encoding.
- Hierarchical meshlets and virtualized geometry require separate milestones
  for simplification, continuity, asset preprocessing, paging, residency, and
  failure recovery. They are not implied by static meshlet support.

## Ownership and data flow

```text
Hydra Scene Index / headless input
    ↓ thin adapter: normalize host data and dirty state
mutable RenderWorld
    ↓ Commit()
immutable FrameSnapshot
    ↓ revision comparison and changed-range compilation
persistent GPU Scene
    ├─ geometry table and arena ranges
    ├─ instance table
    ├─ draw table
    ├─ material table
    ├─ texture table
    ├─ sampler table
    └─ optional meshlet tables
    ↓ culling, compaction, command generation
render paths
    ├─ Forward opaque/reference and fallback
    ├─ Visibility raster + material resolve for supported opaque Mesh
    ├─ Forward transparency / future OIT
    ├─ Gaussian-specific rendering
    └─ overlay, selection, and presentation
```

`FrameSnapshot` is the host/GPU ownership boundary. GPU indices are finite,
reusable slots, not public scene handles or persistent pointers. CPU-side
handles carry an index and generation; shaders normally receive only the index.
Any slot or arena range is reusable only after the last completion value that
can reference it has retired.

## Common GPU Scene ABI

The exact C++ and shader layout is versioned and reflection-tested. The logical
records are:

| Record | Required identity and payload |
| --- | --- |
| `GpuGeometry` | Vertex/index arena ranges or addresses, counts, index type, attribute mask, bounds, and optional meshlet range |
| `GpuInstance` | Current transform, normal/world inverse data, object/instance ID, visibility mask, and flags; previous transform is added when motion vectors require it |
| `GpuDraw` | Geometry, material, and instance indices; indexed range; primitive base; flags; and optional meshlet dispatch range |
| `GpuMaterial` | Material class/flags, scalar factors, and bindless texture/sampler indices |
| `GpuMeshlet` | Local vertex/primitive ranges, material-homogeneous geometry identity, bounds, normal cone, and reserved LOD/hierarchy fields |

The first geometry implementation may use arena offsets because they are easy
to validate. Buffer device address remains an evidence-driven backend option,
not an ABI requirement. C++ and shader definitions must have static layout
checks plus SPIR-V reflection or generated-layout tests.

Updates are revision and changed-range based:

```text
RenderWorld mutation
    → resource revision change
    → FrameSnapshot comparison
    → dirty table and arena ranges
    → persistently mapped upload ring
    → completion-safe GPU Scene update
```

Snapshots produced by `SceneExtractor` carry a non-zero source identity and an
optional delta whose base revision names the exact prior snapshot. A persistent
consumer may use the dirty queues only when both source and base revision match
its resident state. A different source, a skipped revision, or a manually
constructed snapshot triggers full reconciliation. This keeps the incremental
path an optimization rather than a correctness precondition.

Snapshot resource and transient draw tables use immutable, balanced,
structurally shared storage. A localized upsert allocates one replacement
record and copies only its logarithmic tree path; unchanged record identities
and subtrees remain shared by older snapshots. Transform-only edits do not
touch draws, while visibility and material-binding edits replace only the
dependent transient draw. Snapshot build evidence reports resource records
visited/copied, draw decisions rebuilt, and any structural full-table fallback.
Dense record indices still require that fallback for additions and removals;
eliminating it remains part of v0.7.0 rather than prematurely assigning the
persistent draw identity reserved for v0.10.0.

A static frame performs no upload, descriptor allocation/update, shader
compilation, or pipeline creation. Transform, visibility, material parameter,
and texture changes do not rebuild unrelated geometry. Topology changes may
invalidate triangulation, material partitions, meshlets, local indices, bounds,
and cones; points and normal changes invalidate only the derived data they
affect.

## Bindless resource contract

The initial logical global resource set contains frame/scene constants, draw,
instance, material, and geometry tables, plus sampled-image and sampler arrays.
Storage images or acceleration structures are added only for a demonstrated
consumer. Physical descriptor-set splitting is allowed when update frequency or
device limits justify it, but it must not leak into scene ingestion.

The backend probes and reports every feature it actually uses, including
descriptor indexing, partially-bound arrays, update-after-bind, variable
descriptor counts, and non-uniform indexing. Table sizes are configurable and
exhaustion is an actionable error. Devices missing the required combination use
the conventional Forward descriptor path.

Resource-table requirements:

- separate texture and deduplicated sampler tables;
- reserved white, black, flat-normal, and error texture slots;
- generation-checked free-list allocation and completion-value retirement;
- incremental descriptor writes and dirty-range table uploads;
- peak/current slot use, allocation, update, retirement, and exhaustion
  telemetry;
- safe replacement while older frames remain in flight.

Pipeline keys exclude texture and material instance identity. They contain only
state or shader classes that genuinely create a pipeline variant, such as
topology, culling, depth/alpha mode, vertex-layout class, render path, and
material shader class.

## GPU-driven indexed Forward

The first GPU-driven path retains ordinary indexed geometry:

```text
candidate draw records
    → compute frustum and visibility-mask culling
    → compact visible draw list
    → generate indexed indirect commands and count
    → indexed indirect Forward rasterization
```

Occlusion, meshlet expansion, and LOD are not required for this first step. Draw
identity must remain stable through compaction: the shader-visible draw index
maps unambiguously to `GpuDraw`, and thus to geometry, material, instance, and
selection identity. Both candidate and visible counts are recorded.

Completion requires CPU command-recording cost to stop scaling linearly with
draw count, Forward output to match the conventional path, and culling on/off
to be selectable for validation.

## Opaque Visibility Buffer

### Initial scope and encoding

The prototype accepts opaque, static, indexed, single-sample triangle meshes
with a limited basic-PBR material subset. Alpha blending, alpha mask, skinning,
high-frequency deformation, displacement, Gaussian primitives, and overlays
remain on other paths until explicitly added.

The canonical first encoding is:

```text
Visibility: R32G32_UINT
    x = stable GpuDraw index
    y = primitive index relative to the draw (plus primitiveBase when needed)
Depth: D32_SFLOAT
```

The visibility attachment clears to a reserved invalid pair such as
`0xFFFFFFFF, 0xFFFFFFFF`; valid table indices never use that sentinel.
Geometry, instance, material, object, and meshlet identity are recovered through
`GpuDraw` and the GPU Scene tables rather than widening the attachment. Picking
uses the same chain: pixel → draw → instance → object ID → adapter-owned Hydra
path map. A meshlet backend keeps the same external pair and resolves any
meshlet-local primitive through draw/meshlet metadata.

Barycentrics are obtained from a supported fragment-shader barycentric feature
or reconstructed from the triangle. An extra barycentric attachment is allowed
only as a validation fallback because it weakens the bandwidth objective.

### Passes and resolve quality

```text
1. visibility/depth raster
2. compute material resolve
   visibility → draw/geometry/instance/material → triangle attributes
   → barycentric interpolation → texture/material evaluation → HDR color
3. Forward transparent or unsupported-material pass
4. Gaussian and overlay passes as applicable
5. tone mapping and presentation
```

The first textured resolve may use explicit LOD or mip 0 to prove identity,
fetch, transforms, and interpolation. Production quality then progresses
through quad-neighbor gradients with conservative primitive-boundary behavior
to analytic perspective-correct UV gradients. Normal mapping, emissive,
metallic/roughness, environment lighting, alpha mask, shadows, and motion
vectors are added only with Forward comparison images.

Material resolve starts with a bounded material-class or uber-shader subset.
When MaterialX diversity makes divergence or shader size unacceptable, pixels
may be classified into material-specific resolve lists. Generated material
functions remain separate from the common resource ABI.

The implementation needs explicit pass dependencies, image layouts, buffer
barriers, transient lifetimes, debug labels, and timestamps. A small transparent
frame-plan implementation is sufficient; a general-purpose render graph is not
a prerequisite.

## Meshlet policy

### Build and invalidation

The adapter supplies a canonical mesh. The renderer triangulates, partitions by
`GeomSubset`/material binding, and builds material-homogeneous meshlets. Each
meshlet contains local vertex and primitive indices, configurable vertex and
triangle limits, bounds, a normal cone, geometry/material identity, and reserved
LOD metadata. Limits are tuning parameters selected with vendor/device evidence,
not fixed public format constants.

Topology or face-material partition changes rebuild the affected meshlets.
Primvar-only edits update the corresponding buffer. Point or normal edits update
bounds/cones on CPU or compute when profitable; high-frequency deformation may
use conservative bounds or fall back to conventional indexed rendering. Small
meshes may remain conventional when build, culling, or dispatch overhead exceeds
the saved work.

Meshlet builds start with an in-process cache keyed by topology, material
partition, vertex layout, build parameters, implementation version, and cache
format. A disk cache is considered only after deterministic invalidation and
measured build cost are established.

### Culling and raster backends

Instance culling runs before meshlet expansion. Meshlet culling is introduced in
this order: frustum, backface cone, previous-frame Hi-Z occlusion, then
screen-space size/discrete LOD. Camera cuts, projection or resolution changes,
and invalid depth history disable temporal occlusion conservatively.

The first raster backend generates a visible meshlet list and indexed indirect
commands. It validates builder output, local indices, culling, Visibility IDs,
and fallbacks on broad hardware. A Mesh Shader backend is enabled only after
`VK_EXT_mesh_shader`, limits, subgroup behavior, driver stability, and a measured
win are confirmed. Supporting the extension does not automatically select it.

Subdivision initially consumes a correctly triangulated/refined result from the
existing Hydra path. Independent GPU micropolygon or cluster generation is not
part of static meshlet delivery.

## Capability selection and fallbacks

Runtime selection considers device capabilities, geometry size and topology
stability, deformation frequency, material support, and benchmark profiles.
Development and CI overrides must be able to force:

- conventional or bindless resources;
- CPU/conventional or GPU-driven indexed submission;
- Forward or Visibility opaque rendering;
- culling stages on/off;
- explicit-LOD, quad, or analytic Visibility derivatives;
- conventional, meshlet indexed-indirect, or Mesh Shader geometry.

Production exposes these choices through a versioned renderer configuration and
capability report. Environment variables or command-line switches are debug/CI
interfaces, not the public configuration contract.

The permanent fallback set is:

```text
Mesh geometry
    ├─ conventional indexed Forward
    ├─ GPU-driven indexed Forward or Visibility
    ├─ meshlet indexed-indirect Forward or Visibility
    └─ optional meshlet Mesh Shader Forward or Visibility

Other content
    ├─ transparent / unsupported material → Forward
    ├─ Gaussian → Gaussian-specific path
    └─ selection, wireframe, gizmo → overlay path
```

## Delivery mapping

The ordered release mapping is maintained in the
[roadmap backlog](../roadmap/backlog.md). The design dependency gates are:

| Gate | Required evidence before advancing |
| --- | --- |
| Bindless foundation | Capability report, safe replacement/retirement, no per-material descriptor bind, incremental writes, Forward image parity |
| GPU Scene | Stable resource/draw identity, dirty-range upload, object/material traceability, reflected C++/shader layouts |
| GPU-driven Forward | Compacted visible list, indirect count, bounded command recording, culling debug views, conventional Forward parity |
| Experimental Visibility | Correct draw/primitive IDs, transforms and interpolation, basic resolve, Forward differential images |
| Visibility quality | Acceptable mip/edge behavior and representative material parity; transparent and unsupported content compose correctly |
| Static meshlets | Deterministic builder/cache/invalidation, indexed-indirect path, culling statistics, conventional/Visibility image parity |
| Optional Mesh Shader and LOD | Measured device-specific win, safe indexed fallback, conservative Hi-Z history, acceptable LOD transitions |

## Validation and performance evidence

Unit coverage includes slot allocate/retire/reuse, generation mismatch,
fallback slots, table packing and layout, draw/primitive mapping, meshlet build
determinism, cache invalidation, and barycentric reconstruction.

GPU coverage includes non-uniform texture indexing, partially-bound/update-after-
bind behavior when selected, in-flight replacement, indirect count, primitive
identity, compute resolve, meshlet local indices, culling reason accounting, and
fallback selection.

Image comparisons retain Forward as the reference for a triangle, indexed mesh,
multiple materials and instances, UV seams, normal maps, primitive boundaries,
alpha mask when supported, and mixed opaque/transparent/Gaussian/overlay
composition. Tolerances are versioned by material feature.

Performance evidence separates descriptor and GPU Scene update, command
recording, culling, visibility raster, material resolve, transparent/Gaussian
passes, and total GPU time. Structural counters include candidate/visible draws,
descriptor slot use and writes, uploaded ranges/bytes, total/visible meshlets,
rejections by reason, emitted triangles, and selected fallback/backend. The
[benchmarking guide](../guides/benchmarking.md) defines the fixture contract and
comparison rules.

## Explicit non-goals

- Replacing Forward in one release or forcing every geometry type through
  Visibility.
- Adding a meshlet-specific USD schema or exposing renderer packing to Hydra.
- Treating Mesh Shader support as mandatory or universally faster.
- Forcing small, dynamic, skinned, procedural, or unstable-topology meshes into
  meshlets before evidence supports it.
- Reproducing Unreal Engine Nanite, hierarchical LOD, or out-of-core geometry as
  part of the static meshlet milestone.
- Combining bindless, Visibility, meshlets, Hi-Z, async compute, clustered
  lighting, and material classification in one unmeasurable change.
