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
pipeline creation.

Field names, units, fixture order, and integer formatting are deterministic.
Timing values are not.

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

- `merlin-regression.log`: per-render version-3 event rows;
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

The camera phase asserts zero mesh Sync, points/topology/primvar fetch, geometry
upload, geometry-cache miss, and pipeline creation. The static phase asserts
zero upload, allocation, shader-module miss, geometry-cache miss, and pipeline
creation. The capability workflow retains benchmark JSON/comparisons, Hydra
JSON/log/trace, images, validation logs, and dependency/runtime provenance.
