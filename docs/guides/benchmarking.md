# Benchmarking

`merlin-benchmark` measures the RenderWorld → snapshot extraction → Vulkan →
CPU readback reference path. It emits one JSON document to stdout, or to the
path selected with `--output`.

```powershell
./build/adapters/merlin-benchmark/Release/merlin-benchmark.exe `
  --width 512 --height 512 --steady-frames 30 --output benchmark.json
```

Use a Release build, fixed resolution, fixed GPU clocks/power policy, and the
same driver when comparing timings. The report records commit, build type,
compiler, OS/architecture, selected GPU and driver, Vulkan device API, and
resolution so incompatible runs can be rejected before comparison.

## Fixture

The `merlin-benchmark/v2` fixture exercises the resource-granular GPU scene:
two meshes (a triangle and a quad), two materials, and three instances where
the two triangle instances share one mesh. Shared geometry is uploaded once
regardless of how many instances reference it.

## Baselines

The schema always emits these baselines in this order:

1. `first-frame`: initial scene creation/extraction, per-mesh geometry
   suballocation and staging-ring upload, render-target allocation, pipeline
   creation, submission, and CPU readback.
2. `steady-state`: the median of `--steady-frames` unchanged-scene frames.
   Scene update and extraction are zero by definition; upload bytes,
   allocations, and pipeline creations must be zero.
3. `edit-transform`: an instance transform edit. Zero geometry bytes staged and
   zero geometry-cache misses.
4. `edit-visibility`: hides the quad instance. The draw disappears without any
   geometry work. An unmeasured restore frame follows so later scenarios run
   against the full scene again.
5. `edit-material`: a material color edit. Zero geometry work.
6. `edit-points`: a points-only mesh edit with an unchanged vertex count. Only
   the edited mesh's vertex payload is re-staged, in place, without
   suballocation churn; topology is untouched.
7. `remove-mesh`: removes the quad instance and mesh. Nothing is staged and
   exactly the removed mesh's vertex and index ranges are released; retirement
   is deferred until the last frame that could reference them completes.

Each baseline has integer nanosecond CPU scopes for `scene_update`,
`extraction`, `upload`, `command_recording`, `readback`, and `total_frame`.
`readback` includes GPU submission/completion wait and copying both AOVs into
CPU-owned arrays. `total_frame` covers all work in the named baseline after the
renderer and Vulkan device have been initialized. The named scopes are not
exhaustive: render-target allocation, pipeline creation, and frame-pacing waits
fall inside `total_frame` but outside `upload`/`command_recording`/`readback`,
so the sub-scopes are not expected to sum to `total_frame` (most visibly in
`first-frame`).

## Structural counters

The structural counters are `draw_count`, `triangle_count`, `upload_bytes`
(bytes staged through the upload ring this frame), `readback_bytes`,
total/buffer/image allocation counts, `pipeline_creation_count`, snapshot-level
`scene_cache_hits`/`scene_cache_misses`, per-mesh `geometry_cache_hits`/
`geometry_cache_misses`, arena `buffer_suballocation_count` and
`buffer_range_release_count`, and pipeline cache hits and misses. Static
steady-state samples must have zero upload bytes, allocations, and pipeline
creations; the executable rejects a run if those samples do not have identical
structural counters or perform any upload/allocation/pipeline work.

JSON field order, names, units, baseline order, and integer formatting are
deterministic. Timing values intentionally are not deterministic. Prefer
structural-counter assertions in normal CI and compare timing results only in a
controlled capability environment. The `merlin-benchmark-json` CTest asserts
the per-baseline structural counters listed above on every GPU-capable run.
