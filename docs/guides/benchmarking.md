# Benchmarking

hdMerlin keeps two complementary performance records:

- `merlin-benchmark/v3` measures the renderer-owned RenderWorld → extraction →
  Vulkan → selected CPU readback path.
- `merlin-hydra-performance/v1` combines delegate telemetry with an OpenUSD
  Chrome trace for scene-index, CPU-to-Hgi upload, composite, and presentation
  scopes in the install-tree usdview regression.
- `merlin.viewport-benchmark/v1` records native viewport CPU/GPU frame time,
  presented frames, swapchain recreation, validation, GPU presentation-copy
  bytes, and zero-CPU-readback frames; the Hydra variant records CPU/GPU frame
  time and cumulative readback bytes for the USD scene path.
- The planned host-bridge report extends the Hydra record with transfer mode,
  device-relationship capability, selected AOVs, source/destination metadata,
  bridge copy bytes/time, completion wait, target recreations, CPU fallback
  count, and structured rejection/fallback reasons. Tier 0, GPU-copy, and any
  direct-share path are compared as separate presentation modes.

## Native viewport benchmark

```powershell
./build/adapters/merlin-viewport/Release/merlin-viewport.exe `
  --frames 240 --vsync off --benchmark viewport.json
```

Use `--hidden --reference-check --resize-test --validate` for the automated
evidence path. The reference check deliberately requests color/depth readback
for one presented and one offscreen submission and requires exact equality;
other presented frames must record zero readback. `presentation_copy_bytes`
records the GPU blit into the swapchain rather than a CPU transfer. A Hydra
build accepts `--usd scene.usda` and proves normal USD viewport frames retain
zero cumulative readback.

Use Release builds, a fixed driver and resolution, and fixed GPU clock/power
policy for timing comparisons. Ordinary CI gates structural work; timing gates
are opt-in for controlled hardware.

The viewport's **Save benchmark** button also establishes an in-session
baseline from the interval since launch or the previous save. It then starts a
fresh comparison interval, so older baseline frames cannot dilute a subsequent
regression. The diagnostics panel reports live CPU/GPU percentage change,
colors regressions beyond the adjustable threshold, and marks samples over the
hitch threshold in the rolling timing plots. The saved JSON retains its
existing cumulative session semantics. This interactive comparison is
exploratory; retain the JSON and use the comparison script below for
reproducible evidence.

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
| `gpu-driven-small-objects` | The same shared indexed triangle at 1,000, 10,000, and 100,000 instances, measured through conventional and required GPU-driven indexed submission |
| `gpu-driven-diverse-objects` | 16 differently sized triangle/quad meshes and eight untextured basic materials, at the same 1k/10k/100k instance tiers, with conventional/GPU-driven and camera-motion comparisons |
| `gpu-driven-textured-objects` | The diverse fixture with four 2x2 RGBA textures, nearest/linear samplers, independent Forward reference, and the same draw tiers |
| `gpu-driven-arena-objects` | The textured fixture with `--arena-blocks 2/4/8/16` (default 4) production vertex blocks and one index block; unused vertex padding changes placement without increasing triangle work |
| `generated-material-objects` | Eight parameter states of one hand-authored generated-material ABI artifact, conventional execution, explicit required rejection/preferred fallback, camera motion, and parameter/module recovery |
| `one-million-gaussians` | One deterministic degree-0 Gaussian resource with 1,000,000 particles |
| `five-million-gaussians` | The same deterministic distribution scaled to 5,000,000 particles |
| `ten-million-gaussians` | The same deterministic distribution scaled to 10,000,000 particles |
| `aov-combinations` | The reference sequence with explicit color-only, color+depth, and all-AOV gates |
| `4k` | The reference sequence at 3840×2160 unless width/height are overridden |

Large fixtures are explicit so the normal CTest remains fast. Capture them on
the same controlled machine when comparing scale or 4K behavior.

`gpu-driven-small-objects` is an opt-in capability fixture. It requires
bindless GPU Scene tables, `drawIndirectFirstInstance`, indirect count, and
shader draw parameters; it fails explicitly instead of silently comparing a
fallback on unsupported devices. For each draw-count tier it emits an `update`
baseline followed by warmed `conventional` and `gpu-driven` steady-state
baselines and a `camera-motion-gpu-driven` phase. Candidate lists are warmed
in every reusable frame context, so the
steady GPU-driven baselines require zero candidate upload, descriptor rewrite,
first-use allocation, and `mesh_cpu_draw_visit_count`. Camera motion checks
those same invariants on every sample and compares its final color/depth/ID
images with conventional Forward at the same camera. A
representative capture command is:

```powershell
./build/adapters/merlin-benchmark/Release/merlin-benchmark.exe `
  --fixture gpu-driven-small-objects --width 64 --height 64 `
  --steady-frames 10 --output gpu-driven-small-objects.json
```

`gpu-driven-diverse-objects` uses the same phases and exact four-AOV comparison.
It alternates triangle and quad geometry, varies base color and roughness, and
alternates single/double-sided materials. Instance creation assigns a material
every 256 instances; four materials are used at 1k and all eight at later tiers.
The extractor's material/mesh ordering determines native batches, so this
fixture requires multiple indirect calls bounded by the 128 resource pairs,
independent of instance count. Static and moving phases still require zero CPU
draw visits and uploads. The final fixture totals are 16 meshes, 100,000 instances,
and 150,000 triangles. Geometry fits in one vertex/index arena block.

The textured and arena fixtures retain these phases and compare all four AOVs
against a separate renderer with GPU Scene disabled, using Forward's mesh and
instance identity policy. The production arenas use 256-KiB blocks. Padding
unreferenced vertices creates the requested 2/4/8/16 vertex blocks; the index
arena retains one block. Image comparisons require rasterized foreground, so
an empty image cannot satisfy parity. Preparation cost is expected to remain
independent of draw count at fixed resources; command recording can grow with
arena/pipeline batch count. Vertex-block sweeps do not claim arbitrary index-
block packing, material-count scaling, host performance, or pinned-clock timing.

`generated-material-objects` deliberately uses conventional descriptors because
registered generated artifacts are not supported by bindless GPU submission.
Its phases are `update`, `conventional`, `prefer-fallback`, and
`camera-motion-prefer-fallback` at each draw tier. Required submission must reject
with the recorded unsupported reason. Preferred submission must execute every
generated draw, with one submission fallback and zero material fallback. Warmed
static/motion phases have zero preparation draw visits and descriptor writes;
conventional draw recording is still linear and excluded from that visit counter.
Parameter edits and missing-module/restoration verify cache invalidation and
image recovery. The small ABI fixture is hand-authored and does not measure
MaterialX graph generation; `merlin-vulkan-generated-material` separately checks
actual MaterialX prototype and textured Standard Surface execution and reuse.

Reports include actual comparison/rejection counts and recovery flags in
`mesh_verification`, selected descriptor backend, arena residency, validation
enablement and renderer diagnostic counts. `--validate` enables Vulkan checking;
omit it during timing captures. CPU preparation is `gpu_scene_update`, not
conventional draw recording or total frame time.

Run all eight configurations with structural and JSON schema checks explicitly
on a capable GPU (they are not universal CTest or timing gates):

```powershell
cmake -DMERLIN_BENCHMARK=C:/path/to/merlin-benchmark.exe `
  -DMERLIN_BENCHMARK_OUTPUT_DIR=build/gpu-driven-check `
  -DMERLIN_BENCHMARK_VALIDATE=ON `
  -P tests/run_gpu_driven_benchmark_test.cmake
```

This uses three samples at 256x256 for correctness. For timing captures, run the
selected fixture directly with `--steady-frames 30` or more at a fixed extent.

### Gaussian comparisons

The million-Gaussian fixtures emit `first-frame`, the legacy CPU `steady-state`,
then static and camera-motion phases for `cpu`, `gpu-sorted-stream`, and
`gpu-tiled`. Each path starts from the same camera and replays the same sine
translation sequence. All reusable contexts are warmed before static sampling.
GPU samples individually require zero source/prepared uploads, allocations,
CPU preparation and stage/overflow fallback. Gaussian compute descriptor writes
still occur and are reported; they are not a zero-work gate.

`gaussian_verification` compares static and final moving images outside the
timed samples. Sorted-stream permits two RGBA8 steps and requires exact
depth/IDs. Tiled permits six steps, exact depth/primId, and up to 1% depth-tied
particle-ID differences, matching the existing raster test contract. Every
comparison records its differences and `passed` result. A tolerance failure
preserves the complete report and returns failure. The v0.16.0 local 10M tiled
capture exceeded the color bound and remains diagnostic evidence. Current
Vulkan Gaussian paths use a floating-point working target and pixel-center
ellipse evaluation. The image gate checks `gaussian_float_color_frame_count=1`
without widening the six-step bound. Final RGBA8 conversion is included in
Gaussian raster and total GPU/frame cost; reference images can differ from
the old per-splat UNorm blend. Retain both failed historical and current reports.
`compare-benchmarks.py` also rejects a failed current image gate, including
self-comparison, or removal of the baseline's image verification evidence.

```powershell
./build/adapters/merlin-benchmark/merlin-benchmark.exe `
  --fixture one-million-gaussians --width 597 --height 540 `
  --steady-frames 40 --output gaussian.json

cmake -DMERLIN_BENCHMARK=C:/path/to/merlin-benchmark.exe `
  -DMERLIN_BENCHMARK_OUTPUT_DIR=build/gaussian-check `
  -DMERLIN_BENCHMARK_VALIDATE=ON -P tests/run_gaussian_benchmark_test.cmake
```

The hardware gate defaults to 1M at 597x540 with four samples. Use the quoted
`-DMERLIN_GAUSSIAN_FIXTURES=one-million-gaussians;five-million-gaussians` argument
to include 5M. These are capability/image gates, without timing thresholds.
Add `ten-million-gaussians` to the same list for the dense-stack regression.

For the host comparison, use a viewer-capable OpenUSD SDK built with Vulkan,
and build hdMerlin against that same SDK. A GL-only lookdev artifact cannot
select HgiVulkan. Keep its immutable artifact/OCI digest with the captures.
`tests/run_gaussian_host_benchmark.cmake` stages the current install tree,
selects HgiGL Tier 0 or HgiVulkan GPU copy, and records six warmed static/motion
phases, screenshots, per-sample counter ranges, a delegate log and host trace.
It verifies the selected transfer mode and four versus three CPU AOV Maps.
Validation enables Hgi synchronization checking; leave it off for timing.

```powershell
$sdk = 'C:/path/to/vulkan-lookdev-sdk'
$rendererBuild = 'C:/path/to/merlin-build'
$python = 'C:/path/to/python.exe'
$scene = 'C:/dev/hydra-merlin/adapters/merlin-hydra2/tests/fixtures/leica-sofort-top8192.usdc'
foreach ($copy in 0,1) {
  cmake "-DMERLIN_BUILD_DIR=$rendererBuild" "-DMERLIN_PXR_ROOT=$sdk" `
    "-DMERLIN_PYTHON=$python" "-DMERLIN_TESTUSDVIEW=$sdk/bin/testusdview" `
    "-DMERLIN_GAUSSIAN_SAMPLE=$scene" "-DMERLIN_FORCE_HGI_VULKAN=$copy" `
    "-DMERLIN_STAGE_DIR=C:/dev/hydra-merlin/build/host-$copy" `
    -P tests/run_gaussian_host_benchmark.cmake
}
$env:PYTHONPATH = "$sdk/lib/python"
$env:PATH = "$sdk/bin;$sdk/lib;" + $env:PATH
& $python tests/compare_gaussian_host_benchmarks.py `
  build/host-0 build/host-1 build/host-comparison.json
```

Use `scripts/create-gaussian-benchmark.py output.usdc --particles 1000000`
with this Python environment for the deterministic scale corpus. Its positions,
covariance and degree-0 coefficients match the renderer fixture. The generator
authors a conservative three-sigma extent so usdview frames the particle field;
older generated scenes without extent can produce misleadingly small coverage.
usdview frames its own perspective camera. Compare policies within each experiment;
do not interpret renderer versus host times as identical-camera measurements.
Host comparison checks image parity for every policy, fixed extent/sample count,
and the selected-AOV readback saving. Native HgiVulkan normal unselected frames
now keep all four images on the GPU and perform no renderer image readback or
bridge download. HgiMetal now follows the same four-AOV GPU copy contract.
The host benchmark sets `MERLIN_HYDRA2_REGRESSION_COVERAGE=0` so diagnostic
logging does not force a depth payload. Other regression captures retain eager
depth coverage by default; that one diagnostic AOV is separate from host
display requirements. Bridge `hgi_cpu_download_count/bytes/ns` counters are
cumulative and record actual Map demand, including selection, separately from
renderer `readback_bytes`. GPU-copy encode and presentation scopes are
CPU trace durations, not GPU copy timestamps or end-to-end display latency.

### Metal HgiMetal host comparison

On macOS, the same install-tree runner accepts
`MERLIN_GAUSSIAN_HOST_BACKEND=metal`. OpenUSD must include HgiMetal and usdview.
The capture compares the CPU reference, required GPU sorted stream and required
GPU tiled raster. Each path
has static and alternating-camera samples, a screenshot and per-sample
allocation/upload checks. The host report uses the HgiMetal copy counters and
checks zero renderer image readback and zero bridge downloads on unselected
GPU-copy frames. GPU Gaussian
frames add 32 bytes of control plus 32 bytes per resource to readback telemetry;
tiled frames add another 44 bytes of pair/selection counters.

For a deterministic Apple GPU offscreen image and timing comparison at 65K and
1M particles, run `merlin-metal-gaussian-tests --tile-scale`. It prints CSV for
cold and five warmed camera samples per path and size, after five unreported
camera warmups. It includes device preparation/sort/tile/raster times, readback
bytes and maximum RGBA8
channel difference. The command fails on a color difference above 2/255 or
any depth/ID mismatch. GPU timing is device-local; wall time includes the
requested four-AOV readback.

```sh
sdk=/path/to/openusd-2608-prefix
build=/path/to/hydra-merlin/build/metal-hydra
scene=/path/to/hydra-merlin/adapters/merlin-hydra2/tests/fixtures/leica-sofort-top8192.usdc
cmake "-DMERLIN_BUILD_DIR=$build" "-DMERLIN_PXR_ROOT=$sdk" \
  "-DMERLIN_PYTHON=$(command -v python3)" \
  "-DMERLIN_TESTUSDVIEW=$sdk/bin/testusdview" \
  "-DMERLIN_GAUSSIAN_SAMPLE=$scene" \
  -DMERLIN_GAUSSIAN_HOST_BACKEND=metal \
  -DMERLIN_GAUSSIAN_VALIDATE=ON \
  -DMERLIN_GAUSSIAN_SHADER_VALIDATE=OFF \
  -DMERLIN_GAUSSIAN_FRAMES=40 \
  "-DMERLIN_STAGE_DIR=$build/metal-host-8192" \
  -P tests/run_gaussian_host_benchmark.cmake
```

The generated 1M corpus can be created with `scripts/create-gaussian-benchmark.py`
using the same OpenUSD Python environment. Re-run the command with its USDC
path and a separate stage directory. The runner writes installed-renderer,
scene and runtime-config hashes, six phase screenshots, host verification,
delegate log, Chrome trace and `gaussian-hydra-performance.json`. Keep those
raw files local per the [report policy](../reports/README.md). Apple devices
with stage-boundary counter sampling report GPU preparation, radix sort plus
gather, tile binning/sort, and Gaussian raster durations. Sorted-stream raster
includes render-pass load/store overhead; selected tile raster measures its
compute pass. The delegate's
`gaussian_gpu_timestamps_available` field and each report stage's availability
distinguish unsupported sampling from a zero active duration.
These one-device observations are not timing gates.

Metal API validation and shader validation are separate options. Shader
validation defaults to the API-validation setting; explicitly disabling it
records `metal_shader_validation_enabled: false` in capture provenance. The
2026-10-05 large usdview captures use API validation with shader validation off:
instrumenting the host OpenGL driver stalled before renderer submission. Metal
offscreen and AOV tests separately pass with both validators enabled. Gaussian
raster timestamps include the final float-to-RGBA8 conversion.

Native viewport benchmark JSON separates `image_readback_bytes` from total
`readback_bytes`, which includes GPU control/counter telemetry. It also reports
actual sorted/tiled, overflow, CPU fallback and float-color frame counts.
`merlin-viewport-metal-gaussian-no-readback` requires eight presented tiled
frames, no image readback/fallback/overflow, and 8 * 108 bytes of counters.

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
- `gaussian_preparation`
- `gaussian_attribute_upload`
- `gaussian_prepared_upload`
- `gaussian_gpu_preparation`
- `gaussian_gpu_sort`
- `gaussian_gpu_tile`
- `gaussian_raster`
- `command_recording`
- `queue_submission`
- `completion_wait`
- `readback`
- `gpu_execution`
- `total_frame`

GPU execution uses two timestamps on the selected graphics queue and is zero
only when timestamp queries are unavailable. It includes uploads on the
single-queue fallback; dedicated-transfer execution is represented by
structural upload/submission evidence and synchronized before graphics rather
than being folded into the graphics timestamp interval. Completion waiting and
CPU mapping are no longer folded into readback, so CPU and GPU timelines can be
correlated without interpreting one aggregate duration as both.

Gaussian GPU preparation, sorting, tile work and raster use separate device
timestamp pairs. `gaussian_preparation` is the CPU reference timer. Inactive
GPU stages are zero; `environment.timestamp_queries` distinguishes unsupported
timestamps. Hydra reports retain these stages, selected Gaussian policy,
Gaussian counters and host transfer mode. Older records without a GPU timing
availability flag remain explicitly unavailable. CPU waits overlap device
execution, so stage medians must not be added to estimate total frame time.

### Structural counters

The v3 counters include draw/visible-primitive/triangle counts; upload and
readback bytes; requested, rendered, and CPU-readback AOV masks/counts; waits,
resolves, and maps; buffer/image allocation counts and bytes; geometry arena
suballocation/release; shader, descriptor-layout, pipeline, geometry, texture,
sampler, and scene cache behavior; descriptor pool/allocation/update work; and
pipeline creation. Upload evidence splits vertex, index, texture, and optional
GPU Scene table payload bytes, aligned mapped-ring reservations, stable
geometry-range reuse, and arena or ring growth. GPU Scene-enabled runs also
report dirty-copy range count, fixed table capacity, and their dedicated upload
ring's reservation, growth, and in-flight state. Bindless-capable runs
additionally split sampled-image and sampler descriptor writes so steady-state
and localized-edit scaling can be checked independently of the conventional
reference descriptors. Transfer
submission and image ownership-transfer counters distinguish dedicated-queue
upload work from the single-queue fallback. The top-level
`residency` object retains vertex/index arena capacity, resident/peak/free/
retiring bytes, free-span fragmentation, range and block counts, mapped-ring
capacity/reservations/in-flight regions/growth/wrap/retired buffers, and the
texture/sampler capacity, current/peak/retiring use, allocation/reuse/
retirement, descriptor updates, exhaustion, generation mismatches, references,
and sampler deduplication evidence. Its `transfer_queue` object records selected
families, submissions, bytes, ownership transitions, and upload timeline value;
`memory_budget` records heap capacity/budget/usage/available bytes plus the
configured/effective limit and renderer current/peak allocations, releases,
queries, and exhaustion count.

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
  and command-buffer-recording slope with increasing draw count. The initial
  `gpu-driven-small-objects` fixture establishes that narrowly scoped slope and
  bounded native indirect submission for one shared geometry/material. It also
  requires exact color, depth, primitive-ID, and instance-ID parity with the
  conventional baseline at every tier and after camera motion. GPU-driven
  batch preparation remains in `gpu_scene_update`; inspect it and `total_frame`
  independently. `mesh_cpu_draw_visit_count` covers draw-summary, generated
  pipeline preflight, and batch-selection visits, excluding conventional draw
  command recording. It must be zero in warmed static and camera-motion GPU
  phases; GPU execution and readback costs can still grow with scene size.
  `gpu-driven-diverse-objects` extends this to 16 triangle/quad resources and
  eight basic materials with multiple batches. Texture and 2/4/8/16 vertex-arena
  block sweeps add independent Forward parity. Generated-material ABI evidence
  covers conventional preparation reuse and explicit submission fallback;
  generated bindless/indirect execution remains unsupported.
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
update, command recording, queue submission, GPU execution, renderer completion,
bridge encode/copy/wait, RenderBuffer resolve/map, CPU-to-Hgi upload, host
composite, host consumption, and presentation. Renderer/delegate stages use per-frame samples. Host-owned scopes
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
retains a full current Release renderer benchmark, the CTest schema/structural
self-comparison, explicit automatic and forced-conventional
descriptor-selection capability JSON, an expected undersized-bindless-table
failure log, Hydra JSON/log/trace, images, validation logs, and dependency/
runtime provenance. It does not treat an old release tag as a permanent timing
baseline. The Hydra job instead compares Tier 0 and HgiVulkan over the same
current 13-phase fixture, so image parity, transfer bytes, and
readback/upload/copy timing describe the paths being selected now.
`merlin-headless --descriptor-backend` and the bindless capacity options
reproduce those selection cases locally.
