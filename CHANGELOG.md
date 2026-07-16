# Changelog

All notable changes to hdMerlin will be documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project intends to follow [Semantic Versioning](https://semver.org/)
after its public API and release process are established.

## [Unreleased]

## [0.8.0] - 2026-07-17

### Added

- A backend-neutral shader capability/permutation contract and stable Forward
  shader ABI with reflected C++/Slang size, offset, entry-point, resource-class,
  descriptor set, and binding validation.
- Versioned `shaders/v1` packages containing Slang-generated SPIR-V, Metal
  compile-gate source, target reflection, and a deterministic SHA-256 manifest
  with compiler, profile, capability, dependency, generator, permutation, and
  cache-compatibility provenance.
- An explicit Metal conventional-Forward fallback declaration for the
  unsupported non-uniform bindless resource-indexing feature.
- Self-hosted Release capability evidence that builds the `v0.7.0` tag on the
  same GPU and retains matching raw benchmark and comparison JSON reports.

### Changed

- Vulkan conventional and bindless Forward now compile from shared Slang
  modules with pinned Slang 2026.8.x instead of the removed GLSL runtime source.
- Build-tree, Hydra-plugin, and install-tree shader packaging now tracks each
  generated artifact incrementally and preserves the versioned manifest and
  reflection files.
- Reference comparison artifacts now cover exact `instanceId` output alongside
  color, depth, and `primId`.

## [0.7.0] - 2026-07-16

### Added

- SceneExtractor snapshot provenance and resource deltas with explicit base
  revisions, per-kind upsert/removal queues, and geometry/texture/sampler
  reconciliation counters in benchmark and Hydra performance evidence.
- Structurally shared `FrameSnapshot` resource and draw tables with copy-on-
  write record replacement, immutable older revisions, and machine-readable
  visited/copied record, rebuilt-draw, and full-rebuild fallback counters.
- Vulkan-facing bindless sampled-image and deduplicated-sampler residency with
  finite generation-checked slots, materialized white/black/flat-normal/error
  images, dirty-only descriptor writes, completion-safe replacement, and
  current/peak/capacity/retirement telemetry in benchmark evidence.
- A non-uniform-indexed bindless Forward shader path with one global resource
  set and one persistent dynamic-material set per frame context, plus exact
  color/depth/primId/instanceId parity coverage against conventional Forward.
- Persistent vertex/index arena and mapped geometry-upload-ring telemetry for
  capacity, resident/free/retiring bytes, free-span fragmentation, range reuse,
  growth, wrap, and completion collection, with per-frame vertex/index/texture
  staged-byte evidence.
- Dedicated transfer-queue uploads with timeline synchronization and sampled-
  image ownership transitions, plus a validated single-queue fallback.
- `VK_EXT_memory_budget` heap evidence and renderer-owned device-local
  current/peak tracking, configurable hard VRAM limits, and structured
  `resource-exhausted` failure before overcommit.
- One-million-prim localized texture/sampler scaling evidence that visits one
  changed resource without copying dependent instances or rebuilding draws.
- Headless capability controls and workflow artifacts for automatic versus
  forced-conventional descriptor selection and actionable bindless-table
  capacity exhaustion.
- Validation image artifacts now compare forced-conventional output with the
  automatically selected bindless path for color, depth, and primId.

### Changed

- Localized geometry, material, instance, and light upserts now copy only the
  changed extraction records. Transform-only edits retain every draw record;
  visibility and material-binding edits rebuild only the dependent draw.
- Vulkan command recording caches dense resource-index and draw views by
  persistent-table identity, retaining constant-time hot-path lookup without
  rebuilding the views on static frames.
- Vulkan Mesh and image/sampler residency now skips table traversal for static
  snapshots, reconciles only dirty resources for continuous revisions, and
  falls back to full reconciliation for foreign, manually constructed, or
  revision-skipping snapshots.
- Pending texture upload commit/abandon handling now visits only textures
  touched by the submission instead of every resident texture.
- Descriptor-indexing-capable devices automatically select bindless Forward;
  configuration, feature, or limit failures retain conventional Forward with a
  machine-readable fallback reason.

## [0.6.0] - 2026-07-15

### Added

- `merlin-benchmark/v3` CPU/GPU stage distributions with queue timestamps,
  median/p95/p99/maximum and hitch summaries, expanded AOV/descriptor/map/wait/
  allocation counters, and fixed reference, million-triangle, 10,000-mesh,
  1,000-instance, AOV-combination, and 4K fixtures.
- Structural benchmark comparison reports with opt-in controlled-hardware
  timing thresholds and limiting-stage identification.
- Versioned Hydra performance JSON and raw OpenUSD Chrome traces separating
  delegate Sync, scene-index work, RenderWorld/extraction, Vulkan execution,
  readback, RenderBuffer map/resolve, CPU-to-Hgi upload, host composite, and
  presentation scopes.
- Host-neutral `merlin-diagnostic/v1` records with stable codes, source paths,
  dispositions, and named recovery actions, bridged to Hydra diagnostics and
  telemetry.
- A documented OpenUSD 26.05 Gaussian ingestion boundary using the standard
  `ParticleField3DGaussianSplat` and Hydra `particleField` representation,
  without a custom USD schema or direct PLY/SPLAT parser.

### Changed

- Capability CI now runs and retains comparable benchmark and Hydra
  performance evidence. Camera-only and static usdview phases enforce zero
  irrelevant geometry fetch/upload, allocation, shader, and pipeline work.
- Consolidated the bindless, GPU Scene, Visibility Buffer, and meshlet direction
  into one staged design and aligned the architecture, roadmap, benchmark
  contracts, support claims, capability fallbacks, and project overview with
  indexed-indirect-first delivery.
- Hydra mesh Sync now retains per-path topology, points, primvar, normalized
  payload, triangulation, and material state; transform, visibility, camera,
  and material-parameter edits avoid unrelated mesh work.
- RenderWorld, extraction, and Vulkan residency now preserve per-aspect
  revisions and changed ranges, allowing exact partial vertex/index uploads
  with safe full-upload fallback when the resident base revision differs.
- Hydra regression event schema v4 reports triangulation/packing rebuilds,
  changed vertices, coarse primvar invalidation, and diagnostics, and validates
  localized points, topology, primvar, transform, visibility, camera, and
  material-parameter phases.
- Hydra configuration now requires the validated OpenUSD 26.05 shared SDK,
  records the detected version, and rejects MSVC Debug builds when only Release
  OpenUSD libraries are available.

## [0.5.0] - 2026-07-14

### Added

- Host-neutral `MaterialIR` parameter blocks, feature masks, alpha/cutoff and
  double-sided state, revisioned RGBA8 textures and samplers, deterministic
  extraction records, and structured material fallback reporting.
- Vulkan image/sampler residency with completion-safe retirement, shader-module,
  descriptor-layout, and feature-keyed pipeline caches, plus base/vertex color,
  normals, UV image textures, directional lighting, and alpha-mask shading.
- Basic Hydra `UsdPreviewSurface`, `UsdUVTexture`, and distant-light translation
  into the same renderer path, covered by an install-tree textured usdview
  regression and a GPU material-resource lifecycle test.

### Changed

- The headless reference scene now exercises textured, directional-lit shading
  while preserving expected/actual/diff artifact generation.
- Rebased the forward roadmap around performance observability, incremental
  Hydra synchronization, a shared persistent Mesh/Gaussian resource model, a
  native Vulkan viewport, and standard OpenUSD Gaussian ingestion without a
  renderer-specific USD schema.
- Updated the capability workflow to OST 0.17.0 and documented the managed
  `ost renderer view` lifecycle separately from the external/prebuilt
  `--build-dir` escape hatch.

## [0.4.0] - 2026-07-14

### Added

- Explicit Vulkan `RenderRequest` → `Submit` → renderer-specific
  `CompletionToken` → timeout-aware `Resolve` execution, with frame-owned
  targets/readback lifetime and generation-safe geometry updates while older
  submissions remain in flight.
- Per-request color, depth, primId, and instanceId selection with independent
  Tier 0 CPU readback, structured invalid-request/resource-busy/timeout/
  device-lost/unsupported/backend error classification, and single-use token
  validation.
- Dependency-free RGBA PNG and float/uint OpenEXR sinks plus deterministic
  color/depth/primId expected/actual/diff artifact sets retained by Vulkan
  capability CI.
- OpenStrata 0.16 renderer-project manifests, logical target composition,
  machine-readable renderer evidence, and the `ost renderer view` development
  path for the co-built Hydra adapter, with a v0.17.0 dogfooding handoff covering
  build correctness, renderer adoption, evidence composition, and DCC hosts.

### Changed

- Capability CI now pins OST 0.16.0 and exercises `ost build` plus
  `ost validate`; release preparation keeps the OpenStrata project version in
  sync with `VERSION`.

## [0.3.0] - 2026-07-13

### Added

- Practical Hydra mesh normalization for normals, display color/opacity, UVs,
  indexed primvars, and constant/uniform/vertex/varying/face-varying
  interpolation. Primvar-only edits replace only the packed vertex payload.
- Deterministic ear-clipping triangulation for concave polygons, with holes and
  actionable malformed, out-of-range, degenerate, and self-intersecting
  topology diagnostics.
- Native Hydra PointInstancer transforms, including nested instancers and
  instance translate/rotate/scale/matrix primvars, flattened to shared Merlin
  geometry with independently keyed instances.
- `primId` and `instanceId` Vulkan attachments, CPU readback products, and
  Hydra `HdFormatInt32` RenderBuffer delivery alongside color and depth.
- Regression gates covering authored material binding, indexed face-varying
  primvars, concave polygons, native instancing, one million triangles, 10,000
  small meshes, and 256 repeated primvar edits.
- A root `VERSION` source of truth and `prepare-release` command that finalizes
  Unreleased changelog notes, dates the release, and updates comparison links.

### Changed

- Bumped the project and installed package metadata version to 0.3.0.
- Reduced mandatory release bookkeeping to `VERSION` and `CHANGELOG.md`;
  detailed records and product documentation now change only when their
  content changes.

## [0.2.0] - 2026-07-13

### Changed

- Replaced the monolithic `ExtractedScene` with an immutable, resource-granular
  `FrameSnapshot` split into geometry, material, instance, and draw records.
  Records are keyed by serialized handle (slot index plus generation) and
  resource revision; geometry payloads carry independent points/topology
  revisions and are shared across snapshots and instances. This is a breaking
  API change for extraction and Vulkan backend consumers.
- Reworked Vulkan geometry residency: per-mesh vertex/index ranges are
  suballocated from device-local arenas with first-fit free lists, staged
  through a persistently mapped ring buffer, uploaded only for the sub-resource
  whose revision changed (in place when the aligned range size is unchanged),
  and retired deterministically once the last frame that could reference them
  completes.
- Bumped the benchmark schema to `merlin-benchmark/v2`: a shared-geometry
  fixture (two meshes, two materials, three instances) and per-aspect edit
  baselines (`edit-transform`, `edit-visibility`, `edit-material`,
  `edit-points`, `remove-mesh`) replace the single `scene-edit` baseline, with
  geometry cache and suballocation churn counters in every baseline.

### Added

- `merlin-vulkan-resource-update` GPU test enforcing the v0.2.0 exit criteria:
  zero steady-state upload/allocation/pipeline work, zero geometry bytes for
  transform-, visibility-, and material-only edits, in-place dirty-range
  points updates, geometry sharing across instances, deterministic retirement
  of removed resources, and generation-safe handle reuse.
- `FrameCounters` fields `geometry_cache_hits`, `geometry_cache_misses`,
  `buffer_suballocation_count`, and `buffer_range_release_count`, and
  `RendererStatistics` fields `geometry_range_retirements`,
  `pending_geometry_retirements`, and `geometry_arena_blocks`.

## [0.1.0] - 2026-07-12

### Added

- Host-neutral `RenderWorld` and deterministic draw extraction libraries.
- Persistent Vulkan 1.4 offscreen rendering with color/depth CPU readback.
- Opt-in Hydra 2 adapter and install-tree usdview stable-update validation.
- Versioned CMake package exports for Core and optional Vulkan consumers.
- Windows/Linux Core CI and manually dispatched Vulkan/Hydra capability CI.
- Machine-readable Vulkan runtime provenance and retained validation artifacts.
- Deterministic `merlin-benchmark` JSON reports with environment metadata,
  per-stage CPU timings, structural counters, and first-frame, steady-state,
  and scene-edit baselines.
- Public contribution, security, build/install, package, support, architecture,
  roadmap, and delivery-history documentation.
- Versioned installed JSON metadata describing dependencies, configured layers,
  exported targets, and runtime-only products.
- Stable SemVer tag-driven Windows/Linux Core SDK releases with project-version
  validation and SHA-256 checksum assets.

Granular pre-release progress is retained in the
[delivery history](docs/reports/delivery-history.md).

[Unreleased]: https://github.com/animu-sphere/hydra-merlin/compare/v0.8.0...main
[0.8.0]: https://github.com/animu-sphere/hydra-merlin/compare/v0.7.0...v0.8.0
[0.7.0]: https://github.com/animu-sphere/hydra-merlin/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/animu-sphere/hydra-merlin/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/animu-sphere/hydra-merlin/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/animu-sphere/hydra-merlin/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/animu-sphere/hydra-merlin/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/animu-sphere/hydra-merlin/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/animu-sphere/hydra-merlin/releases/tag/v0.1.0
