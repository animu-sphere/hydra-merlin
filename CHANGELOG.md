# Changelog

All notable changes to hdMerlin will be documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project intends to follow [Semantic Versioning](https://semver.org/)
after its public API and release process are established.

## [Unreleased]

### Added

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

[Unreleased]: https://github.com/animu-sphere/hydra-merlin/compare/v0.3.0...main
[0.3.0]: https://github.com/animu-sphere/hydra-merlin/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/animu-sphere/hydra-merlin/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/animu-sphere/hydra-merlin/releases/tag/v0.1.0
