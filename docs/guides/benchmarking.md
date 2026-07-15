# Benchmarking

hdMerlin keeps two complementary performance records:

- `merlin-benchmark/v3` measures the renderer-owned RenderWorld → extraction →
  Vulkan → selected CPU readback path.
- `merlin-hydra-performance/v1` combines delegate telemetry with an OpenUSD
  Chrome trace for scene-index, CPU-to-Hgi upload, composite, and presentation
  scopes in the install-tree usdview regression.

Use Release builds, a fixed driver and resolution, and fixed GPU clock/power
policy for timing comparisons. Ordinary CI gates structural work; timing gates
are opt-in for controlled hardware.

## Renderer benchmark

```powershell
./build/adapters/merlin-benchmark/Release/merlin-benchmark.exe `
  --fixture reference --width 512 --height 512 --steady-frames 30 `
  --output benchmark.json
```

The report records commit, build type, compiler, OS/architecture, GPU/driver,
Vulkan API, timestamp-query availability, fixture identity, and resolution.
Each stage contains integer-nanosecond `median`, `p95`, `p99`, and `max`
summaries. Each baseline also reports hitches above the larger of twice its
median or median plus 2 ms.

### Fixed fixtures

Select a fixture with `--fixture`:

| Name | Contract |
| --- | --- |
| `reference` | Shared geometry plus first-frame, static, camera, transform, visibility, material, points, topology, AOV, and removal scenarios |
| `million-triangles` | One indexed mesh and one instance with exactly 1,000,000 triangles |
| `ten-thousand-meshes` | 10,000 independently handled one-triangle meshes and instances |
| `thousand-instances` | 1,000 instances sharing one mesh |
| `aov-combinations` | The reference sequence with explicit color-only, color+depth, and all-AOV gates |
| `4k` | The reference sequence at 3840×2160 unless width/height are overridden |

Large fixtures are explicit so the normal CTest remains fast. Capture them on
the same controlled machine when comparing scale or 4K behavior.

### Reference baselines

The reference fixture emits, in order:

1. `first-frame`
2. `steady-state`
3. `camera-only`
4. `edit-transform`
5. `edit-visibility`
6. `edit-material`
7. `edit-points`
8. `edit-topology`
9. `aov-color-only`
10. `aov-color-depth`
11. `aov-all`
12. `remove-mesh`

Static and camera-only frames must perform zero geometry upload, allocation,
shader-module miss, pipeline creation, and geometry-cache miss. The color-only
gate transfers and maps exactly the color product, proving that depth and ID
readback are not hidden inside a color request.

### Stages

`stages_ns` separates:

- `render_world_update`
- `snapshot_extraction`
- `gpu_scene_update`
- `command_recording`
- `queue_submission`
- `completion_wait`
- `readback`
- `gpu_execution`
- `total_frame`

GPU execution uses two timestamps on the selected graphics queue and is zero
only when timestamp queries are unavailable. Completion waiting and CPU mapping
are no longer folded into readback, so CPU and GPU timelines can be correlated
without interpreting one aggregate duration as both.

### Structural counters

The v3 counters include draw/visible-primitive/triangle counts; upload and
readback bytes; requested, rendered, and CPU-readback AOV masks/counts; waits,
resolves, and maps; buffer/image allocation counts and bytes; geometry arena
suballocation/release; shader, descriptor-layout, pipeline, geometry, texture,
sampler, and scene cache behavior; descriptor pool/allocation/update work; and
pipeline creation. Upload evidence splits vertex, index, and texture payload
bytes, aligned mapped-ring reservations, stable geometry-range reuse, and arena
or ring growth. Bindless-capable runs additionally split sampled-image and
sampler descriptor writes so steady-state and localized-edit scaling can be
checked independently of the conventional reference descriptors. The top-level
`residency` object retains vertex/index arena capacity, resident/peak/free/
retiring bytes, free-span fragmentation, range and block counts, mapped-ring
capacity/reservations/in-flight regions/growth/wrap/retired buffers, and the
texture/sampler capacity, current/peak/retiring use, allocation/reuse/
retirement, descriptor updates, exhaustion, generation mismatches, references,
and sampler deduplication evidence.

Field names, units, fixture order, and integer formatting are deterministic.
Timing values are not.

## GPU-driven roadmap evidence

The following are planned benchmark contracts, not currently selectable v3
fixtures or counters. Each is added with the milestone that implements the
corresponding path, while the existing Forward fixtures remain the baseline.
The detailed delivery gates are defined in the
[GPU-driven rendering policy](../design/gpu-driven-rendering.md).

| Planned fixture | Content | Primary decision |
| --- | --- | --- |
| `gpu-driven-small-objects` | Many shared small meshes, hundreds of thousands of instances, materials, and textures | Bindless update scaling, CPU submission slope, instance/draw compaction |
| `visibility-bandwidth` | High resolution, material-heavy opaque Mesh, UV seams, primitive boundaries, and controlled overdraw | Visibility raster/resolve cost and bandwidth against Forward |
| `meshlet-large-static` | Tens of millions of static triangles with substantial off-screen area | Meshlet build/cache cost, fine-culling rejection, emitted-triangle reduction |
| `occlusion-heavy` | Urban/interior layers and repeatable camera cuts | Hi-Z rejection, conservative history reset, visibility stability |
| `dynamic-geometry` | Transform, material, texture replacement, points deformation, and topology update phases | Dirty upload/descriptor retirement, rebuild cost, and fallback behavior |
| `mixed-path` | Opaque, alpha-mask, transparent, Gaussian, selection, and overlay content | Specialized-path composition and depth/color/identity conventions |

Planned structural/timing evidence is introduced in the same order as the
implementation:

- Bindless Forward reports selected/fallback backend, current/peak/capacity
  slots, allocation/retirement, descriptor writes, exhaustion, and sampler
  deduplication. Static frames perform zero descriptor allocation/update and
  zero global table descriptor work. Dedicated bindless update timing and
  material-bind reduction remain later evidence additions.
- GPU-driven indexed rendering reports candidate/visible draws, rejection by
  enabled stage, generated indirect commands, command-generation/culling time,
  and CPU command-recording slope with increasing draw count.
- Visibility reports selected derivative mode, visibility-raster and material-
  resolve time separately, supported/fallback draw counts, and Forward
  differential-image metadata for each material feature.
- Meshlets report build/cache/invalidation time, total/visible meshlets,
  frustum/cone/occlusion/LOD rejection, emitted triangles, selected indexed or
  Mesh Shader backend, and conventional fallback counts.

Path-on/path-off comparisons use the same scene, camera, resolution, AOVs,
warm-up, and frame count. A faster result is not accepted when ID mapping,
Forward differential images, fallback composition, or resource-lifetime tests
fail. Mesh Shader automatic selection requires a repeatable win for the named
GPU/driver profile and never weakens the indexed-indirect fallback.

## Comparing reports

```powershell
python scripts/compare-benchmarks.py baseline.json current.json `
  --output comparison.json
```

The `merlin-benchmark-comparison/v1` report fails on stable structural drift and
identifies the largest measured stage for every baseline. Timing thresholds are
disabled by default. When enabled, build, OS, compiler, GPU/driver, Vulkan, and
timestamp-query metadata must also match. Enable them only on controlled
hardware:

```powershell
python scripts/compare-benchmarks.py baseline.json current.json `
  --timing-threshold-percent 10 --output comparison.json
```

Exit code 2 means a regression was found; exit code 1 means an input/reporting
error. `merlin-benchmark-json` and `merlin-benchmark-compare` exercise the
schema, exit criteria, and self-comparison in CTest.

## Hydra and host evidence

The install-tree usdview test retains three related artifacts:

- `merlin-regression.log`: per-render version-4 event rows;
- `merlin-hydra-performance.json`: phase summaries;
- `merlin-usdview-trace.json`: the raw OpenUSD Chrome trace.

The JSON report separates Hydra Sync, RenderWorld update, extraction, GPU scene
update, command recording, queue submission, GPU execution, completion wait,
readback, RenderBuffer resolve/map, CPU-to-Hgi upload, host composite, and
presentation. Renderer/delegate stages use per-frame samples. Host-owned scopes
are summed per presented host frame and marked with
`sample_kind: trace_scope` so they cannot be mistaken for renderer-frame
samples. Every stage also carries an `available` flag; an unavailable host
scope remains explicit rather than being reported as a zero-duration operation.

The phases are `baseline`, `points`, `topology`, `primvar`, `transform`,
`visibility`, `camera`, `material_parameter`, `diagnostic`, `recovery`,
`remove`, `readd`, and `resize`. Version 4 adds triangulation/packed-mesh
rebuild, changed-vertex, coarse-primvar-invalidation, and diagnostic counters.

Camera, transform, visibility, and material-parameter phases assert zero
unrelated mesh fetch, normalization, triangulation, and upload. Camera also
asserts zero pipeline creation. The primvar phase records OpenUSD 26.05's
coarse `primvars` locator, value-compares the fetched semantics, avoids
triangulation, and requires upload bytes to equal the changed packed-vertex
ranges. Static baseline still asserts zero upload, allocation, shader-module
miss, geometry-cache miss, and pipeline creation. Diagnostic/recovery verifies
the versioned unsupported-topology path, while remove/readd verifies that path
caches and generations do not survive Rprim lifetime. The capability workflow
retains benchmark JSON/comparisons, Hydra JSON/log/trace, images, validation
logs, and dependency/runtime provenance.
