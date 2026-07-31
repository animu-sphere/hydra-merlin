# Delivery history (pre-release roadmap detail)

This is the granular delivery log for work completed toward hdMerlin foundation
releases. It
is **historical evidence**, not a description of current behavior (see
[design](../design/)) or planned work (see [roadmap](../roadmap/)). Once a
version ships, its stable summary belongs in the
[changelog](../../CHANGELOG.md).

Legend: ✅ done

---

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
