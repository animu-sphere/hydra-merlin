# Delivery history (pre-release roadmap detail)

This is the granular delivery log for work completed toward hdMerlin foundation
releases. It
is **historical evidence**, not a description of current behavior (see
[design](../design/)) or planned work (see [roadmap](../roadmap/)). Once a
version ships, its stable summary belongs in the
[changelog](../../CHANGELOG.md).

Legend: ✅ done

---

## v0.16.x Metal quality, host AOVs and native viewport

Completed the locally testable Metal follow-up on 2026-10-05, based on
`589688b` plus the working-tree changes. Apple M3/macOS 15.7.9, OpenUSD 26.08,
Slang 2026.8 and AppleClang 16 were used. This closes the color-tail/larger
capture, Mesh movement and Metal demand-readback implementation items; broader
hardware, controlled timing, actual click picking/overlay coverage and CI
separation remain on the roadmap.

- Metal CPU/GPU sorted streams and tiles now accumulate in RGBA32F and convert
  once to the exported RGBA8 image, with pixel-center ellipse evaluation.
  A closed-form 512-splat low-opacity fixture independently checks the corrected
  reference. Offscreen 65K/1M tests pass the unchanged two-step color and exact
  depth/ID policies; the scale capture has zero color difference. Overflow,
  hidden/removal, mixed Mesh composition, leases and resize also pass.
- API-validated HgiMetal captures use forty warmed samples for each of six
  static/camera CPU/sorted/tiled phases at 597x540. The public 8,192-particle
  corpus has maximum sorted/tiled differences of 1/6 channel units, replacing
  the historical tiled 15-unit tail. The properly framed deterministic 1M scene
  matches exactly in all four final policy comparisons. Every measured GPU
  sample has zero prepared upload/allocation/fallback, zero image readback and
  zero bridge downloads; sorted/tiled control telemetry is 64/108 bytes.
- The generated scale scene lacked extent, so usdview originally displayed a
  small patch. The generator now authors conservative three-sigma bounds; the
  accepted 1M capture covers about 410x410 pixels. Old unframed captures remain
  local as limited evidence. Alternating camera phases end at their common
  reset camera; final images and per-sample counters provide distinct checks.
- Native viewport eight-frame tiled captures at 8,192 and 1M particles have
  maximum 1/255 CPU-reference differences at the physical 1194x1080 Retina
  extent. All eight frames select tile/float rendering, with no overflow or CPU
  fallback. A separate no-capture regression presents eight frames with zero
  image readback and exactly 864 counter bytes. Viewport reports now distinguish
  these bytes and actual path selection; Cocoa dialog parenting builds under ARC.
- HgiMetal copies depth and both ID targets alongside color. Exact current
  non-color Map tests cover motion, odd-sized resize, removal, cache reuse and
  Tier 0 recovery. ID Uint/Sint transfer stays on the GPU; Map owns aligned
  storage. Completion-handler lease release is serialized with backend state.
  Mesh projection reflection now flips front-face winding, and all usdview
  Mesh phases including Hdx selection pass. Regression logging still deliberately
  requests one depth image; normal benchmark frames request none.
- Full Metal/Hydra/native viewport CTest passes **39/39 without skips**, and
  Core CTest passes **17/17**. Offscreen and AOV tests run Metal API and shader
  validation. Large host captures run API validation with shader validation off;
  the shader-instrumented host stalled in Apple OpenGL-driver compilation before
  renderer submission. The failed attempt and stack sample remain local.
- Raster timestamps include final conversion; the retained float attachment
  costs 16 bytes/pixel per frame context. API/shader-instrumented offscreen
  warmed camera GPU medians are sorted/tiled 11.48/7.90 ms at 65K and
  64.77/51.60 ms at 1M; raster medians are 8.45/2.36 and 41.49/11.48 ms.
  These are local observations with unpinned power/clocks and some concurrent
  GPU work, not performance gates or additive stage estimates.

Raw reports/logs remain local in `build/metal-hydra/metal-host-8192-float-api/`,
`build/metal-viewport-followup/metal-host-1m-framed/`, native captures in the latter
build directory, and `docs/reports/2026-10-05-metal-v016-followup.md`.

## v0.16.x HgiVulkan demand-driven depth/ID readback

Completed the Vulkan slice on 2026-10-04, based on `7a85e96` plus the
working-tree change. Bound supported AOVs copy into retained Hgi targets with
completion-owned export leases. Host depth composition consumes its GPU
texture; non-color Map downloads the current target once. New GPU copies and
Tier 0 writes replace that CPU version. Mixed consumers and failed copies
retain eager Tier 0. HgiMetal remains color-only and is still on the roadmap.

- RTX A5000/OpenUSD 26.08/Windows Release/MSVC 19.51: the real HgiVulkan AOV
  test matches current depth/primId/instanceId exactly through motion, resize
  and removal, caches repeated Map, rejects mapped-buffer resize/mutation,
  and exercises Tier 0 writes. Synchronization validation exposed retained
  staging-write hazards; explicit reuse barriers fix both Hgi Map downloads
  and backend image readback. No device-wide waits were added.
- The 8,192-particle host CTest runs four samples per static/motion policy;
  a separate 1M host capture runs forty per policy. CPU sorted-stream, GPU
  sorted-stream and GPU tiled paths keep all four display images on the GPU:
  every measured sample has zero renderer image readback and zero bridge
  downloads. Final 1M policy images match exactly. Clocks/power remain unpinned;
  this is transfer/correctness evidence, not a universal performance gate.
- Mesh usdview still passes its existing image policy, and an added selection
  phase exercises demand-driven ID Map with validation. Its image comparison
  disables only usdview's subsequent OpenGL axis overlay; the original phases
  and thresholds are unchanged. Regression coverage deliberately reads back
  one depth AOV (1,289,520 bytes at 597x540), versus Tier 0's four AOVs
  (5,158,080 bytes). Normal host benchmarks disable that diagnostic coverage.
- A clean rebuild and all 54 CTest cases pass without skips, including
  source/install consumers, shared shader/ABI checks and Vulkan/Hydra fixtures.
  The first incremental run exposed an obsolete render-settings test object
  after the adapter telemetry layout changed; the clean rebuild resolved it.
  Metal runtime execution and broader actual host picking remain unverified.

Raw reports/logs remain local in `build/hydra-aov-*` and the dated
`docs/reports/2026-10-04-hydra-demand-readback.md` report.

## v0.16.x Vulkan Gaussian float composite and 10M quality closure

Completed the Vulkan color-discrepancy follow-up on 2026-10-04, based on
`b0db2b5` plus the working-tree change. CPU/GPU sorted streams and compute tiles
now use an RGBA32F working attachment and one final conversion to the public
RGBA8 AOV. Fragment ellipse evaluation uses the actual pixel center. Per-splat
UNorm quantization and interpolated quad offsets both contributed to the old
discrepancy; a closed-form 513-particle low-opacity RGBA fixture anchors the
corrected reference independently of GPU path comparisons.

- RTX A5000, driver 597.16, Vulkan SDK 1.4.350.0, Windows Release/MSVC 19.51:
  1M/5M/10M captures at 597x540 use forty warmed samples per static/motion path.
  All twelve final-frame comparisons have exact color, depth, primId and
  instanceId parity. The six-step tiled color bound is unchanged. Separate
  four-sample captures at every scale pass with validation enabled and no
  renderer validation messages.
- Gaussian raster timestamps include the final GPU conversion. At 10M,
  static GPU sorted-stream/tiled raster medians are 37.67/1.59 ms, total GPU
  medians 60.31/57.23 ms, and total frame medians 62.12/59.14 ms. The extra
  float target costs 16 bytes/pixel per frame context before allocation
  alignment. Clocks/power are unpinned; these are observations, not hardware
  timing gates, and stage medians are not additive frame estimates.
- Final CTest passes 53/53 with no skips, including shader/ABI, four-AOV
  Gaussian regression and overflow, completion/lease/resize, GPU-only RGBA8
  export, Mesh/Gaussian usdview, HgiVulkan synchronization/copy and installed
  SDK consumers. Tier 0 and HgiVulkan color-copy PNG bytes match. A separate
  float-composite Vulkan PNG records the intentional reference correction;
  the old shared PNG and host image tolerances are retained.
- Raw accepted, failed and rejected diagnostic reports remain local in
  `build/v016x-tile-quality/`, with the dated local
  `docs/reports/2026-10-04-vulkan-gaussian-tile-quality.md` report. The historical
  v0.16.0 10M 8/10-step failure remains untouched. Metal Gaussian execution and
  its host color-tail investigation remain separate incomplete work.

## v0.16.0 Metal Gaussian tile raster and Apple M3 scale check

Connected the tiled request to the native Metal renderer on 2026-09-30. The
same retaining command now performs sorted-record gather, tile pair count and
prefix scan, pair emit, stable tile-key radix sort, ranges and selection. A
successful selection skips the indirect sorted draw and composites into the
opaque Mesh color/ID targets; pair overflow or a clamped record retains the
complete sorted draw without a CPU-visible scheduling dependency. Tiled Prefer
uses GPU sorted-stream fallback if tile allocation is unsupported; Tiled Require
reports Unsupported for that condition. An explicit pair capacity supports
deterministic overflow checks.

- Release/AppleClang 17, Xcode 16.4, Slang 2026.8, macOS 15.7.9 and Apple M3
  were used at 512x512, 65,536 and 1,048,576 deterministic degree-0 particles.
  The source checkout was based on `3d5d015` with these changes uncommitted.
  `merlin-metal-gaussian-tests --tile-scale` captured cold plus five camera
  warmups and five measured camera frames per path with four AOVs requested.
  Every tiled frame selected compute raster. Color differed by at most 2/255
  per RGBA8 channel; depth and both ID
  AOVs matched GPU sorted-stream exactly. The image/overflow renderer test ran
  with Metal validation enabled. A separate 300-particle/256-pair test forced
  overflow and matched every sorted-stream AOV byte exactly.
  Metal API Validation renderer tests also cover tiled opaque Mesh depth,
  visibility/removal, empty and all-rejected frames, plus cold/warm Prefer
  fallback for an unsupported explicit capacity.
- Camera-frame medians (five measured samples, ms) at 65K: sorted GPU execution
  3.93, raster 2.15; tiled GPU execution 4.29, tile work 0.86, compute raster
  0.92. At 1M: sorted GPU execution 28.77, raster 12.56; tiled GPU execution
  35.10, tile work 11.34, compute raster 7.18. The tile raster stage is cheaper
  at both sizes, but binning and sorting outweigh the saving on these scenes.
  These are local samples with unpinned clocks/power, not a universal
  performance claim. Wall medians with
  four-AOV readback were 4.86/5.22 ms at 65K and 30.63/36.64 ms at 1M
  (sorted/tiled). Raw CSV remains local under the [report policy](README.md).
- A subsequent same-day HgiMetal GPU-copy/usdview run on the public
  8,192-particle corpus selected tiled raster
  in every measured frame, with 14,169–14,231 requested pairs below the 65,536
  capacity, zero overflow/fallback and zero warmed uploads/allocations. Four
  samples per static and alternating-camera phase passed the host image policy:
  tiled versus CPU had maximum RGBA8 channel difference 15, with only 0.0183%
  of pixels above the six-unit per-channel threshold and mean channel error
  0.0172. This measured tail exceeds the offscreen degree-0 two-unit bound;
  it remains a quality/tuning target. Camera-phase median GPU execution was
  3.45 ms tiled versus 2.79 ms sorted; tile work took 0.42 ms and compute raster
  1.43 ms. A separate two-sample HgiMetal tile run passed with Metal API and
  GPU order/pair validation enabled. The four timing samples are directional
  evidence only. HgiMetal tiled 1M,
  native viewport captures, other Apple GPU models and scene tuning remain
  open. Earlier HgiMetal comparisons below record the former sorted-stream
  fallback and remain valid historical evidence.

## v0.16.0 Metal Gaussian host validation and stage timestamps

Completed controlled HgiMetal sorted-stream comparisons and device stage
sampling on 2026-09-30. Phase 3 tile raster and broader hardware evidence
remain open; a tiled request in `Prefer` is explicitly counted as a sorted-stream
fallback.

- Release/AppleClang 17, Xcode 16.4, Slang 2026.8, OpenUSD 26.08,
  macOS 15.7.9 and Apple M3 were used with 597x540 HgiMetal GPU-copy output.
  The public 8,192-particle degree-3 corpus has 40 warmed samples per static
  and alternating-camera phase; the generated degree-0 1M scene has 20.
  Each capture records the scene, runtime config and installed renderer hashes,
  policy, image comparison and per-sample counters. Validation was off for
  timing; a separate four-sample HgiMetal host comparison and the Metal GPU
  image, residency and compute tests pass with validation enabled.
- Four GPU-versus-CPU host screenshots per corpus pass: maximum RGBA8 channel
  difference is 1 on 8,192 particles and 0 on 1M, with zero pixels above the
  sorted-stream two-step threshold. Every GPU camera sample has zero CPU
  Gaussian preparation, zero attribute/prepared upload and zero allocation.
  HgiMetal copies color on the GPU and reads back three other AOVs in one map:
  3,868,560 image bytes at this extent, plus 64 bytes of post-completion
  counters on the GPU path. Direct sharing is still unavailable.
- Apple stage-boundary counter samples now report preparation including scratch
  clear/key generation, radix sort including gather, and an isolated Gaussian
  render pass. On the 1M moving phase their medians are 2.30, 12.33 and
  99.86 ms, with 114.81 ms command-buffer GPU execution. The CPU reference
  moving phase spends 66.68 ms in CPU preparation, uploads a 52 MB prepared
  stream and has a 100.96 ms GPU execution median. On this corpus the GPU
  path removes CPU traversal/upload but does not improve device raster cost;
  static CPU cache remains cheaper. The render-pass sample includes its
  load/store overhead, and stage medians are not additive frame estimates.
- The Metal/Hydra CTest run passed 31/32 cases. The remaining Mesh usdview
  smoke fails an image movement assertion in the same way on an isolated
  build of the unmodified `2cabe45` commit; it does not exercise Gaussian
  rendering. This pre-existing host test gap is separate from the passing
  Gaussian host captures.
- The Phase 3 Metal tile shaders compile into the packaged metallib. On Apple
  M3, every tile pipeline is created and a two-tile fixture passes GPU
  count/emit/ranges/verification plus compute raster color/ID checks under
  Metal API validation. Its known prefix and ordered pairs are supplied by
  the test; the renderer's frame-wide scan/sort, GPU overflow choice and
  native tiled mode remain to be connected and compared at scale.

Raw captures remain local under the [report policy](README.md). Reproduce the
same-runtime HgiMetal captures with the [Metal host commands](../guides/benchmarking.md#metal-hgimetal-host-comparison).

## v0.16.0 Vulkan Gaussian and host comparisons

Completed the Vulkan Gaussian/host measurement work on 2026-09-30. The harness
records image failures as failures; the 10M tiled color discrepancy and the
overall v0.16.0 exit criteria remain open.

- Release/MSVC 19.51, RTX A5000 driver 597.16, 597x540 and 40 warmed samples per
  phase cover deterministic degree-0 1M/5M/10M particles. CPU, GPU sorted-stream
  and GPU tiled replay the same camera sequence. GPU preparation, sort, tile
  binning and raster use device timestamps; every measured GPU frame skips CPU
  preparation with zero source/prepared uploads, allocation and stage fallback.
  Compute descriptors still refresh and are reported. Validation is disabled
  for timing; separate 1M/5M validation captures pass.
- Sorted-stream matches all four CPU AOVs exactly at all three tiers. Tiled
  preserves depth/primId exactly and stays within the established 1% depth-tied
  particle-ID allowance. Maximum RGBA8 error is 3 at 1M and 5 at 5M; 10M reaches
  8 static/10 moving against the six-step bound and returns failure with its
  complete report. Its timings are diagnostic, not acceptance evidence.
- Moving CPU preparation medians are 113.93/598.50/1311.91 ms at 1M/5M/10M;
  GPU preparation is 0.267/1.275/2.671 ms on sorted-stream. Sorted-stream total
  frame medians are 5.86/26.17/45.82 ms, including four-AOV CPU readback.
  Static cached CPU can be cheaper than tiled recomputation at 1M. Clocks and
  power are unpinned, so these serialized local observations are not timing gates.
- Paired usdview captures use the same OpenUSD Vulkan runtime for HgiGL Tier 0
  and HgiVulkan GPU copy, the 8,192-particle degree-3 corpus and deterministic
  1M scene, six static/motion policy phases and 40 samples each. All twelve
  cross-host phase images match exactly. Every GPU-copy frame reads/maps three
  depth/ID AOVs instead of four: 3,868,560 versus 5,158,080 bytes, saving
  1,289,520 bytes (25%). Color copy has no coarse wait; at most one lease is
  pending. Direct share remains rejected as `public-texture-import-unavailable`.
  Host camera framing differs from the headless experiment. Host copy/present
  scopes are CPU trace durations, not GPU copy or end-to-end display timings.
- The public artifact is
  `oci://ghcr.io/animu-sphere/openstrata-runtime-cy2026-lookdev:26.08-vulkan-windows-x86_64`,
  OCI digest `sha256:21cb5fe4c725c9918f6b18fe79b957cfec9d6032e3a40e2e8290ed2dcb70331f`,
  archive digest `sha256:03c5a8edcf9f362bebf9d84afd53f4e8e84b24edfa333ad375b0871a1feae463`.
  OST verifies its SBOM/provenance and installs it into an isolated local SDK;
  hdMerlin is rebuilt against that SDK.
- Hgi synchronization validation exposed overlapping transfer writes to the
  tiled verification record count. An explicit transfer-to-transfer buffer
  dependency fixes it. The new install-tree host benchmark checks all six
  policies with synchronization validation; related Vulkan/Hgi/image/report/
  install-consumer tests pass. Fifteen focused tests also pass on the prior
  GL lookdev SDK.

Raw reports, traces, images and the dated report remain local per the
[report policy](README.md). Reproduce them with the
[Gaussian comparison commands](../guides/benchmarking.md#gaussian-comparisons).
Metal host comparisons and stage timestamps are recorded above; tile raster
remains open. These Vulkan results do not extend their support claim to Metal.

## v0.16.0 Vulkan Mesh scaling validation

Validated the supported Mesh submission and generated-material fallback boundary
on 2026-09-28. This completes the Mesh validation work item; remaining Gaussian
acceptance issues and the overall v0.16.0 exit criteria remain open.

- Eight opt-in configurations cover shared/diverse/textured Mesh, 2/4/8/16
  production vertex-arena blocks, and conventional generated-material execution
  at 1k/10k/100k draws. Diverse fixtures retain 16 triangle/quad resources and
  eight materials; textured fixtures add four images and two samplers.
- Static and camera-motion GPU phases report zero CPU preparation draw visits,
  geometry/texture/GPU Scene uploads, allocations and descriptor writes after
  warming. Per-frame material uniforms still refresh outside the upload counter.
- The correctness gate records 78 exact color/depth/primitive-ID/instance-ID
  comparison sets with rasterized foreground. Textured/arena fixtures also
  compare against an independent Forward renderer without GPU Scene tables.
- Generated artifacts execute conventionally with zero material fallback.
  Required GPU submission rejects; preferred mode records one submission
  fallback. Parameter edits and missing-module/restoration exercise invalidation
  and image recovery. Unique-variant preflight and completed-frame descriptor
  reuse remove warmed preparation traversal and descriptor churn. Conventional
  draw recording remains linear and is excluded from the preparation counter;
  generated bindless/GPU-driven execution remains unsupported.
- All 41 selected source/runtime/install-consumer CTest cases and four actual
  MaterialX compiler/artifact/runtime cases pass. The eight GPU configurations
  pass with validation enabled and zero renderer-owned diagnostic messages.

Local timing used Release/MSVC 19.51, Vulkan SDK 1.4.350.0, an NVIDIA RTX A5000
(driver 597.16), a fixed 256x256 four-AOV target and 30 warmed samples per phase.
Textured static preparation medians were 4.60/5.60/5.45 us at 1k/10k/100k draws;
moving-camera preparation medians were 5.75/5.00/6.20 us. Recording grows with
arena/pipeline batch count (up to 128 batches in the 16-block fixture).

These are serialized, same-device headless observations with clocks and power
unpinned, not universal timing gates, controlled host comparisons, or renderer
before/after measurements. The vertex sweep pads unreferenced vertices while
keeping index geometry in one block; arbitrary index-block packing and growing
material/module counts are outside this evidence. Raw captures remain local per
the [report policy](README.md). Reproduce the correctness gate and timing captures
with the fixtures and commands in the [benchmark guide](../guides/benchmarking.md).

## v0.14.1 Gaussian correctness MVP ✅

- ✅ Added standard Hydra `particleField` ingestion, float/half normalization,
  covariance evaluation, degree-0–3 spherical-harmonic payloads, host-neutral
  revisions, normalized particle ranges, and structured diagnostics without a
  custom USD schema or direct PLY/SPLAT parsing.
- ✅ Added deterministic CPU camera-space preparation, projection, culling,
  stable back-to-front sorting, procedural Vulkan ellipse rasterization,
  authored opacity/SH composition, static cache reuse, and stage telemetry.
- ✅ Added frame-owned packed-stream shadows so localized edits upload only
  changed prepared-instance ranges while reorder/visibility/size changes remain
  correct and completion-safe.
- ✅ Defined Gaussian picking as resource `primId` plus zero-based particle
  `instanceId`; a dedicated ID subpass retains the nearest contributing
  Gaussian without requiring `independentBlend` and preserves uncovered Mesh
  identity.
- ✅ Added the repository-owned CC0 8,192-particle degree-3 USDC corpus and a
  visible 597×540 reference image, with Tier 0 and HgiVulkan usdview comparison
  gates and zero renderer-owned Vulkan validation diagnostics.
- ✅ Captured local native Vulkan viewport evidence for 300 hidden VSync-off
  frames at 597×540: 1,536,743 ns average CPU frame time, 300 presented frames,
  386,856,000 presentation-copy bytes, and zero validation messages.

## v0.14.0 HgiMetal GPU-copy host presentation ✅

- ✅ Added HgiMetal render-driver discovery, Hgi-owned color targets, and
  same-`MTLDevice` GPU copy from leased Merlin color AOVs on validated OpenUSD
  26.05/26.08 packages, while preserving Tier 0 CPU RenderBuffers as the
  universal fallback.
- ✅ Added explicit capability, fallback, target-lifetime, copy, upload,
  completion, and direct-share evaluation telemetry, with completion-safe
  target retirement across resize and driver changes.
- ✅ Kept direct sharing affirmatively gated on device, storage, usage, pixel
  format, queue, completion, retirement, public-import, and implementation
  requirements; validated packages reject unavailable public texture import and
  select GPU copy.
- ✅ Hardened Forward lighting for stable signed normals and camera-light
  contribution without changing the Vulkan projection orientation.

## v0.13.1 HgiVulkan direct-path hardening ✅

- ✅ Added one affirmative direct-share gate covering physical/logical device
  identity, queue ownership, API/extensions, format/usage, sample count,
  tiling, memory constraints, public import and host consumption, completion
  retention, resize retirement, and implementation availability.
- ✅ Added stable per-gate rejection names and evaluation telemetry separately
  from the selected transfer fallback, so rejecting Tier 2 does not misreport a
  supported Tier 1 GPU-copy frame as an operational failure.
- ✅ Added sampled usage plus explicit usage, tiling, device-local memory, and
  exclusive-sharing metadata to exported color images, and made GPU copy
  validate the expanded source contract.
- ✅ Confirmed that OpenUSD 26.05/26.08 public Hgi creates Hgi-owned textures
  and aliases Hgi-owned sources but cannot import a Merlin-owned `VkImage`;
  both packages therefore report `public-texture-import-unavailable` and retain
  GPU copy instead of depending on private Hgi ownership internals.
- ✅ Passed self-hosted GPU capability
  [run 30655056809](https://github.com/animu-sphere/hydra-merlin/actions/runs/30655056809)
  at commit `8a2b4a4`: OpenUSD 26.05/26.08 HgiVulkan, Vulkan 1.4 Debug, and
  Vulkan 1.4 Release all completed successfully, including both 13-phase
  Tier 0/HgiVulkan usdview regressions.

## v0.13.0 HgiVulkan GPU-copy bridge ✅

- ✅ Added public Hydra-driver discovery, Hgi-owned color targets, and selected
  color GPU copy on the validated OpenUSD 26.05 and 26.08 packages while
  retaining Tier 0 CPU RenderBuffers as the universal fallback.
- ✅ Added a borrowed Vulkan 1.3 device/graphics-queue path for HgiVulkan hosts,
  with explicit image layout/ownership metadata and no device- or queue-idle
  wait; Merlin-owned Vulkan remains at the Vulkan 1.4 baseline.
- ✅ Exported selected renderer AOV images through move-only leases that retain
  frame targets through Resolve and return from Hgi command-buffer completion;
  depth and ID AOVs remain on CPU readback.
- ✅ Added structured selection, fallback, copy, byte, completion, and
  coarse-wait telemetry plus fallback coverage for missing, disabled,
  non-Vulkan, swapped, and operationally rejected host paths.
- ✅ Exercised Tier 0 and HgiVulkan through the same 13 scene/camera/material/
  diagnostic/lifetime/resize phases with bounded image differences and
  versioned readback/upload/copy performance evidence.

## v0.12.0 native Metal presentation ✅

- ✅ Added a private Cocoa/GLFW adapter that creates the `CAMetalLayer` while
  `Merlin::Metal` configures its device, pixel format, color space, pacing, and
  drawable pool and retains all command encoding and completion ownership.
- ✅ Added a renderer-owned fullscreen GPU presentation pass from the reference
  RGBA target to BGRA sRGB drawables, including linear sRGB-to-Display P3
  conversion and an explicit rejected boundary for future HDR output.
- ✅ Integrated the official Dear ImGui Metal renderer into the shared
  viewport UI without moving native handles or widget ownership into Core.
- ✅ Added resize-safe drawable extents, frames-in-flight drawable retention,
  vsync and drawable-count policy, presentation timing/copy telemetry, and
  zero-readback normal frames.
- ✅ Made `merlin-viewport` build and package with Metal alone while preserving
  explicit Vulkan selection and Apple automatic Metal preference in combined
  configurations.
- ✅ Exercised hidden compositor-backed Apple GPU presentation, resize,
  validation, exact presented/offscreen reference parity, Metal presentation
  target validation, Display P3 configuration, and explicit HDR rejection in
  Debug and Release test suites.

## v0.11.0 native Metal backend ✅

- ✅ Added the optional `Merlin::Metal` backend with native device/queue,
  renderer-owned runtime MSL compilation, Forward pipelines, depth, offscreen
  targets, and explicit submit/completion/resolve behavior.
- ✅ Consumed the same host-neutral Mesh, instance, camera, texture, sampler,
  basic material, opacity-mask, and render-product records as Vulkan, with
  color, depth, `primId`, and `instanceId` CPU readback.
- ✅ Added heap-backed scene residency, finite generation-checked texture and
  sampler slots, completion-protected reuse, frames-in-flight target/readback
  reuse, argument-buffer tier negotiation, and conventional Forward fallback.
- ✅ Made per-frame argument buffers dirty-only and retained zero target
  allocation and zero table update for a steady snapshot after frame contexts
  are seeded.
- ✅ Added actionable heap/table exhaustion classes and backend-neutral plus
  Metal-specific capacity, residency, allocation, retirement, and argument
  update telemetry.
- ✅ Exercised textured output, depth/picking identity, alpha-mask discard,
  replacement under stable slots, install-tree consumption, and runtime command
  completion on Apple Silicon; hosted macOS Debug/Release jobs retain
  compile/package evidence.

## v0.10.0 MaterialX shader-generation boundary ✅

- ✅ Generated the accepted constants, image/UV0/world-normal,
  add/multiply/mix, and minimum Standard Surface slice as deterministic,
  pass-neutral Slang material functions.
- ✅ Kept topology, parameter state, resource assignments, and target artifacts
  separately identified and verified the common ABI against SPIR-V and Metal
  reflection.
- ✅ Executed parameter and texture/sampler modules in renderer-owned Vulkan
  Forward with per-module descriptor/pipeline layouts, pipeline reuse across
  value and texture-content edits, and structured artifact/resource fallback.
- ✅ Added GPU image evidence for textured Standard Surface execution and
  missing-resource recovery without changing depth, picking, lighting, or AOV
  ownership.
- ✅ Serialized generated draw/fallback evidence through benchmark,
  renderer-report, native viewport, and Hydra regression reports.
- ✅ Retained and installed the accepted generated Slang, SPIR-V, Metal-target,
  and reflection artifacts under `shaders/v2/materialx`.

## Renderer foundation ✅

- ✅ Added the handle-based, host-neutral `RenderWorld` scene model.
- ✅ Added deterministic draw extraction without OpenUSD, Hydra, Vulkan, Qt, or
  DCC SDK types in the Core public API.
- ✅ Added a persistent Vulkan offscreen renderer with color/depth CPU readback,
  multiple frame contexts, revision-based scene upload, and one completion
  value for both render products.
- ✅ Added the opt-in Hydra 2 adapter with mesh topology, transform, visibility,
  camera synchronization, adapter-owned USD-path mapping, CPU RenderBuffers,
  and a Vulkan-backed render pass.
- ✅ Added deterministic Core, RenderBuffer, Vulkan offscreen, validation, and
  install-tree usdview stable-update tests.

## Vulkan 1.4 baseline ✅

- ✅ Made Vulkan 1.4 the minimum source-build, installed-package, loader, and
  physical-device API contract.
- ✅ Required a Vulkan 1.4-capable graphics queue and `glslc` at build time.
- ✅ Kept renderer-owned validation and performance warnings as failures while
  separating unrelated general loader or host diagnostics.
- ✅ Added versioned JSON evidence for SDK/header, loader, selected device,
  driver, API, timeline semaphore, and validation state.
- ✅ Retained the JSON evidence and render logs/images as capability-workflow
  artifacts.

## Reproducible CI and package baseline ✅

- ✅ Added Windows and Linux Core-only Debug and Release CI.
- ✅ Exercised source tests and the isolated install-tree consumer in hosted CI.
- ✅ Split hosted Core, Vulkan/headless, and OpenUSD/Hydra work by required
  capability, with explicit skip semantics for optional capabilities.
- ✅ Added a manually dispatched Windows capability workflow with separate
  headless and Hydra jobs and a `vulkan-1.4` runner-label contract.
- ✅ Pinned GitHub Actions, the LunarG Vulkan SDK download checksum, the
  OpenStrata CLI version, and the Animusphere OpenUSD runtime artifact digest.
- ✅ Kept assertion-based tests active in Release configurations and cancelled
  superseded Core CI runs per ref.
- ✅ Installed versioned CMake package files and exported `Merlin::RenderWorld`,
  `Merlin::RenderExtraction`, and optional `Merlin::Vulkan` targets.
- ✅ Defined Headless, benchmark, and Hydra as runtime-only install products so
  OpenUSD stays out of the exported Core/Vulkan dependency graph.
- ✅ Added versioned JSON dependency/package metadata to configured builds,
  install prefixes, and release archives.
- ✅ Added stable SemVer tag-driven Windows/Linux Core SDK release automation
  with project-version validation and SHA-256 checksum assets.
- ✅ Covered Vulkan/headless Debug and Release configurations in the manually
  dispatched capability workflow.

The capability workflow and its evidence contract are complete. Enrolling a
repository-scoped GPU runner for continuous execution remains in the
[backlog](../roadmap/backlog.md#cross-cutting-open-items).

## Public project baseline ✅

- ✅ Published the Apache License 2.0 text.
- ✅ Added contribution, security, and changelog policies.
- ✅ Added build/install, CMake package, support-matrix, and renderer
  architecture documentation.
- ✅ Documented the intentionally unavailable MaterialX, advanced viewport, and
  low-copy GPU interop features for v0.1.0.

## Measurement baseline ✅

- ✅ Added `merlin-benchmark` with a versioned, fixed-order JSON schema and
  commit, build type, compiler, OS, GPU/driver, Vulkan API, and resolution
  metadata.
- ✅ Added CPU scopes for scene update, extraction, upload, command recording,
  readback, and total frame time.
- ✅ Added per-frame draw/triangle, transfer byte, allocation, pipeline, and
  scene/pipeline cache counters.
- ✅ Added first-frame, steady-state median, and scene-edit baselines with CI
  assertions for static-scene zero-upload/allocation/pipeline behavior.

## MaterialIR and basic shading ✅

- ✅ Added host-neutral material parameter blocks, texture/sampler bindings,
  feature masks, alpha/cutoff and double-sided state, plus independent resource
  revisions and structured extraction fallback records.
- ✅ Added completion-safe Vulkan texture/sampler residency, descriptor updates,
  and shader-module, descriptor-layout, and feature-keyed pipeline caches.
- ✅ Rendered base/vertex color, display opacity, normals, UV image textures,
  directional light, opaque surfaces, and alpha masks through the common
  renderer path; value-only edits reuse pipelines.
- ✅ Added a textured directional-lit headless reference scene and a basic Hydra
  `UsdPreviewSurface`/`UsdUVTexture`/distant-light translation with install-tree
  usdview evidence.

## Performance observability foundation ✅

- ✅ Split GPU scene update, command recording, queue submission, GPU timestamp
  execution, completion wait, and CPU readback; recorded selected AOVs, bytes,
  maps, resolves, descriptor work, allocation bytes, and visible primitives.
- ✅ Upgraded the renderer benchmark to versioned distribution and hitch
  summaries with static, camera, edit, AOV, million-triangle, 10,000-mesh,
  1,000-instance, and 4K fixtures.
- ✅ Added structural comparison reports with limiting-stage identification and
  optional controlled-hardware timing thresholds.
- ✅ Added Hydra Sync/fetch telemetry and combined it with OpenUSD Chrome trace
  scopes for scene-index processing, CPU-to-Hgi upload, composite, and
  presentation in a versioned install-tree report.
- ✅ Gated static and camera-only paths against irrelevant fetch, upload,
  allocation, shader, pipeline, and geometry-cache work, and retained all
  reports in capability CI.

## Incremental Hydra synchronization ✅

- ✅ Retained per-USD-path Hydra topology, points, primvar descriptors and
  values, normalized/indexed primvars, triangulation, packed geometry, material,
  transform, and visibility state across Sync calls.
- ✅ Added locator-aware points, topology, primvar, transform, visibility,
  camera, and material-parameter fast paths with conservative fallback for
  coarse or unavailable dirty information.
- ✅ Preserved distinct source and derived resource revisions plus normalized
  changed vertex/index ranges through RenderWorld, extraction, and Vulkan
  residency; compatible resident revisions receive exact partial uploads and
  incompatible shapes safely fall back to full upload.
- ✅ Added versioned host-neutral `merlin-diagnostic/v1` records with stable
  codes, source paths, dispositions, and recovery actions, bridged to Hydra
  warnings and telemetry.
- ✅ Validated removal/re-addition, malformed-topology rejection and recovery,
  per-phase cache/fetch/rebuild counts, exact changed-byte upload, and zero
  unrelated work through Core, Vulkan, and install-tree usdview regressions.
- ✅ Required and recorded the OpenUSD 26.05 shared SDK for Hydra builds and
  made Release-only MSVC SDK incompatibility fail with an actionable Debug
  diagnostic.
- ✅ Documented the standard OpenUSD `ParticleField3DGaussianSplat` through
  Hydra `particleField` ingestion boundary for the later Gaussian milestone,
  without adding a custom schema or direct PLY/SPLAT parser.

## Persistent snapshot upsert path ✅

- ✅ Replaced vector-per-revision extraction tables with immutable balanced
  storage that shares unchanged records and subtrees across snapshots.
- ✅ Made geometry, material, instance, and light upserts visit and copy only
  changed records; transform edits retain draws and visibility/material-binding
  edits rebuild only their dependent transient draw.
- ✅ Added snapshot visited/copied record, rebuilt-draw, and full-table fallback
  counters to benchmark JSON and Hydra performance events.
- ✅ Covered old-snapshot immutability and record identity sharing, including a
  localized edit in the 10,000-mesh regression fixture.

## Persistent snapshot structural edits ✅

- ✅ Replaced addition/removal full-table fallback with dense append and
  identity-preserving swap removal, updating only draws that reference removed
  or displaced geometry, material, and instance records.
- ✅ Added texture-to-material, sampler-to-material, mesh-to-instance, and
  material-to-instance reverse dependencies so structural binding invalidation
  scales with dependent records instead of the complete scene.
- ✅ Added validated snapshot-local dense indices to resource upsert deltas and
  retained handle-reconciliation compatibility for older snapshots in the
  Vulkan backend.
- ✅ Covered structural work counters, displaced record identity, immutable old
  snapshots, localized texture/sampler invalidation, and Vulkan resource and
  material regressions.

## Million-prim persistent snapshot scaling ✅

- ✅ Replaced linear free-slot scans and quadratic pending-change compaction in
  `RenderWorld` with explicit free-slot reuse and object-kind/handle indexing,
  keeping one-million-prim initial construction practical.
- ✅ Used ordered insertion hints and known dense indices during initial scene
  extraction so sorted resource, dependency, and draw construction avoids
  redundant logarithmic lookups without changing persistent table identity.
- ✅ Added a one-million-prim regression in which 100 transform edits visit and
  copy exactly 100 instance records, rebuild no draws or tables, and retain all
  unaffected record and draw identities.
- ✅ Added shared texture and sampler edits at the same one-million-prim scale;
  each visits and copies exactly its one resource record, rebuilds no draw or
  table, and retains the million instance/draw identities.
- ✅ Added 50 removals plus 50 additions at the same live prim count, covering
  displaced dense indices, identity-preserving swap removal, immutable prior
  snapshots, and bounded structural counters under the existing scale timeout.

## Descriptor-indexing negotiation ✅

- ✅ Probed every descriptor-indexing feature and sampled-image/sampler limit
  required by the planned bindless Forward path and enabled the feature chain
  only when the negotiated tables can be maintained.
- ✅ Added explicit conventional/bindless selection with versioned capability
  output and machine-readable configuration, feature, and limit fallback
  reasons.
- ✅ Covered exact limits, per-stage resource overhead, retained sampler
  allocation overhead, forced conventional configuration, and missing-feature
  fallback without requiring a GPU.

## Bindless resource-table foundation ✅

- ✅ Added finite texture and sampler slot tables with table identity,
  generation-checked handles, deterministic free-list reuse, actionable
  exhaustion errors, and current/peak/retiring/available telemetry.
- ✅ Reserved stable white, black, flat-normal, and error texture indices and
  deduplicated sampler slots by Vulkan-affecting descriptor values while
  excluding debug labels.
- ✅ Delayed slot reuse and generation advancement until the last completion
  value, coalesced dirty descriptor indices, and recorded allocation, reuse,
  update, retirement, collection, exhaustion, stale-generation, reference, and
  deduplication evidence.

## Vulkan bindless residency foundation ✅

- ✅ Connected revisioned Vulkan textures and samplers to the finite logical
  tables so unchanged resources retain indices and changed resources receive
  distinct completion-safe replacement slots.
- ✅ Materialized white, black, flat-normal, and error RGBA8 images, created a
  partially-bound update-after-bind sampled-image/sampler descriptor set, and
  rewrote only dirty elements.
- ✅ Deduplicated Vulkan sampler objects, rewrote collected descriptors to safe
  fallbacks before destroying replaced objects, and reused a slot only after
  its generation advanced at completion.
- ✅ Added bindless per-frame descriptor counters, benchmark residency evidence,
  and a validation-enabled two-frames-in-flight regression covering stable
  use, replacement, collection, fallback rewrite, reuse, and zero steady work.

## Bindless Forward activation ✅

- ✅ Added non-uniform sampled-image and sampler array shaders that consume the
  resident table indices while retaining conventional Forward as a selectable
  feature/limit/configuration fallback.
- ✅ Replaced per-material image descriptors on the bindless path with one
  global update-after-bind resource set and one persistent dynamic-material set
  per frame context, producing zero warmed static descriptor work.
- ✅ Added configurable automatic/conventional/bindless renderer selection,
  packaged both shader ABIs, and validated exact color, depth, primId, and
  instanceId parity on a textured scene.

## Persistent arena and upload telemetry ✅

- ✅ Exposed separate vertex/index arena capacity, resident/peak/free/retiring
  bytes, active/retiring ranges, block growth, completed release counts, free-
  span counts, and largest-free-span fragmentation evidence.
- ✅ Exposed persistently mapped geometry-upload-ring capacity, reservation,
  in-flight/peak bytes and regions, growth, wrap, and retired-buffer evidence.
- ✅ Split per-frame staged payload into vertex, index, and texture bytes,
  recorded aligned ring reservations and stable range reuse, and covered
  initial growth, localized edits, fragmentation recovery, and in-flight
  replacement through Vulkan and benchmark regressions.

## Asynchronous transfer and VRAM budgets ✅

- ✅ Selects a dedicated transfer family when timeline semaphores are available,
  submits geometry and texture uploads independently, and makes graphics wait
  on a separate upload timeline without changing frame completion tokens.
- ✅ Uses concurrent geometry-buffer sharing across queue families and explicit
  transfer-to-graphics ownership/layout transitions for sampled images, with a
  validated single-queue fallback.
- ✅ Probes `VK_EXT_memory_budget` heap capacity/budget/usage, applies a
  configurable renderer device-local limit before allocation, classifies both
  proactive denials and Vulkan OOM as `resource-exhausted`, and retains
  current/peak/allocation/release/exhaustion evidence.
- ✅ Extends capability and benchmark JSON with queue selection, upload timeline,
  ownership-transfer, and VRAM current/peak/capacity evidence; lifetime tests
  cover deterministic configured-limit exhaustion.
- ✅ Exposes descriptor backend/table capacity, VRAM limit, and transfer
  selection through the headless probe; the GPU capability workflow retains
  automatic/forced-conventional metadata plus an expected actionable
  bindless-capacity exhaustion log for every configured profile.
- ✅ Makes headless validation artifacts compare a forced-conventional render
  against the automatically selected bindless render when available, retaining
  exact color, depth, and primId expected/actual/diff images; capability CI now
  runs the resource-update, material, and bindless lifetime regressions too.

## Slang shader and ABI foundation ✅

- ✅ Replaced the conventional and bindless GLSL Forward sources with shared
  Slang modules while retaining the existing Vulkan descriptor negotiation and
  runtime SPIR-V entry points.
- ✅ Pinned Slang 2026.8.x and added dependency-aware SPIR-V and Metal codegen,
  per-target reflection, versioned build/install packaging, and deterministic
  cache keys with source, compiler, profile, capability, matrix-layout,
  optimization, entry-point, permutation, and generator provenance.
- ✅ Extracted stable C++ Forward push-constant, material-uniform, and resource-
  binding ABI declarations; reflected tests fail with actionable field or
  set/binding diagnostics and Core retains only semantic shader capabilities.
- ✅ Declared Metal non-uniform bindless indexing unsupported and selected the
  conventional Forward compile gate without weakening the Vulkan bindless path.
- ✅ Extended forced-conventional versus automatic-path artifacts to exact
  color, depth, primId, and instanceId comparisons, and removed the superseded
  GLSL runtime path.
- ✅ Confirmed the repository-scoped Windows x64 `vulkan-1.4` runner with a
  green Debug/Release Vulkan and OpenUSD 26.05 Hydra
  [capability run](https://github.com/animu-sphere/hydra-merlin/actions/runs/29508228337).
  Release evidence builds the `v0.7.0` tag on the same GPU: structural
  comparison passes with zero regressions, and the 120-frame steady-state GPU
  median moves from 1,231,700 ns to 1,188,085 ns (-3.5%). Raw reports and a
  non-gating 20% timing observation are retained for review.

## Backend-neutral viewport and Vulkan presentation ✅

- ✅ Added `Merlin::RenderBackend` with backend selection/factories,
  renderer-meaning capabilities and limits, logical presentation and completion
  handles, submit/resolve, common timings/counters, and structured errors.
- ✅ Adapted the existing Vulkan renderer and Hydra delegate to that contract
  while keeping Vulkan, Metal, GLFW, and native surface types out of Core and
  concrete backend/window types out of the Hydra public boundary.
- ✅ Added the permanent GLFW-hosted `merlin-viewport` product with
  usdview-style, `upAxis`-aware framing and tumble/track/dolly controls, resize,
  click-triggered ID readback, screenshots, title timing, backend selection,
  benchmark mode, and optional OpenUSD loading through Hydra.
- ✅ Added Vulkan surface/swapchain ownership, FIFO and vsync-off present-mode
  selection, GPU-only color blit into the acquired image, out-of-date/resize
  recovery, and per-swapchain-image completion semaphores.
- ✅ Retained exact viewport/offscreen color and depth parity, zero CPU readback
  on normal presentation frames, resize recreation, presentation-copy bytes,
  validation message counts, and CPU/GPU timing evidence in CTest and
  `merlin.viewport-benchmark/v1` reports.
- ✅ Added Core-only, installed-package, Vulkan viewport, and OpenUSD/Hydra USD
  viewport coverage plus a forbidden concrete-backend type scan.

## MaterialXGenSlang compiler foundation ✅

- ✅ Added the optional `material/merlin-materialx` component and exported
  `Merlin::MaterialX` target, privately linked to the pinned MaterialX 1.39.6
  MaterialXGenSlang implementation and disabled by default.
- ✅ Kept MaterialX SDK types out of the public compiler API and preserved
  Core/Vulkan builds with `MERLIN_ENABLE_MATERIALX=OFF`.
- ✅ Generated a graph-only `evaluateMaterial(MaterialInputs)` Slang function
  without a vertex stage, renderer entry point, AOV output, or lighting block
  for the initial constant, convert, add, multiply, and mix-capable slice.
- ✅ Retained generator/MaterialX provenance, logical input and uniform
  reflection, deterministic generated source, and a SHA-256 identity over the
  canonical input and source.
- ✅ Added actionable invalid-document, missing-library, renderable-selection,
  unsupported-node/output, and generation diagnostics at the integration
  boundary.
- ✅ Compiled one generated module through direct SPIR-V and Metal-target test
  wrappers with target reflection when `slangc` is available, and verified the
  optional installed-package consumer.
- ✅ Added image/UV0/world-normal generation and a renderer-owned minimum
  Standard Surface result for `base`, `base_color`, `metalness`,
  `specular_roughness`, and `normal`, while rejecting explicitly authored
  out-of-scope Standard Surface inputs.
- ✅ Kept topology, parameter, and texture-default identities separate across
  the Standard Surface slice and included portable loaded-library and
  transitive generator-source fingerprints in the module key.
- ✅ Compiled the textured Standard Surface result wrapper from one generated
  source through both SPIR-V and Metal targets without allowing resources to
  leak from the constant-buffer binding.
- ✅ Composed compiler, target, profile, capability, layout, optimization,
  debug, and target-option policy into a Core target-artifact key that keys
  generated and handwritten Slang identically, and made the `shaders/v2`
  manifest record the module sources, module identity, and artifact key that a
  test recomputes through that contract. Handwritten module sources come from
  the depfile the compiler emitted, so an added include cannot slip past the
  key.

This is the foundation stage of the boundary rather than the whole of it. The
common diagnostic/fallback contract, Vulkan Forward execution, and image
evidence that completed it are recorded in the v0.10.0 section above and
released in [v0.10.0](../releases/v0.10.0.md); the accepted
[MaterialXGenSlang policy](../design/materialxgenslang-boundary.md) remains the
authoritative ownership contract.
