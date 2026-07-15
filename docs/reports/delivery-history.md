# Delivery history (pre-release roadmap detail)

This is the granular delivery log for work completed toward hdMerlin foundation
releases. It
is **historical evidence**, not a description of current behavior (see
[design](../design/)) or planned work (see [roadmap](../roadmap/)). Once a
version ships, its stable summary belongs in the
[changelog](../../CHANGELOG.md).

Legend: ✅ done

---

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
