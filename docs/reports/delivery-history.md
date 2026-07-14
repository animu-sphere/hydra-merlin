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
