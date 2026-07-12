# Benchmarking

`merlin-benchmark` measures the flattened RenderWorld → extraction → Vulkan →
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

## Baselines

The `merlin-benchmark/v1` schema always emits these baselines in this order:

1. `first-frame`: initial scene creation/extraction, GPU upload, render-target
   allocation, pipeline creation, submission, and CPU readback.
2. `steady-state`: the median of `--steady-frames` unchanged-scene frames.
   Scene update and extraction are zero by definition.
3. `scene-edit`: an instance transform edit, extraction, revision-triggered
   scene upload, rendering, and CPU readback against the warm render target.

Each baseline has integer nanosecond CPU scopes for `scene_update`,
`extraction`, `upload`, `command_recording`, `readback`, and `total_frame`.
`readback` includes GPU submission/completion wait and copying both AOVs into
CPU-owned arrays. `total_frame` covers all work in the named baseline after the
renderer and Vulkan device have been initialized. The named scopes are not
exhaustive: render-target allocation, pipeline creation, and frame-pacing waits
fall inside `total_frame` but outside `upload`/`command_recording`/`readback`,
so the sub-scopes are not expected to sum to `total_frame` (most visibly in
`first-frame`).

The structural counters are `draw_count`, `triangle_count`, `upload_bytes`,
`readback_bytes`, total/buffer/image allocation counts, pipeline creation count,
and scene/pipeline cache hits and misses. Static steady-state samples must have
zero upload bytes, allocations, and pipeline creations; the executable rejects
a run if those samples do not have identical structural counters.

JSON field order, names, units, baseline order, and integer formatting are
deterministic. Timing values intentionally are not deterministic. Prefer
structural-counter assertions in normal CI and compare timing results only in a
controlled capability environment.
