# Changelog

All notable changes to hdMerlin will be documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project intends to follow [Semantic Versioning](https://semver.org/)
after its public API and release process are established.

## [Unreleased]

## [0.12.0] - 2026-07-27

### Added

- Native Metal presentation for `merlin-viewport` through an adapter-owned
  `CAMetalLayer`, completion-safe drawables, a renderer-owned GPU presentation
  pass, vsync/frame-pacing policy, resize handling, presentation timing/copy
  telemetry, and explicit SDR color-space plus future HDR extension contracts.
- Dear ImGui Metal rendering in the existing development viewport UI, with the
  same camera, input, AOV, screenshot, benchmark, picking, and diagnostic
  behavior as the Vulkan viewport.
- Metal-only viewport builds and an Apple GPU smoke test covering hidden
  compositor-backed presentation, resize, zero-readback frames, and exact
  offscreen reference-product parity.
- Hydra USD loading through the Metal viewport, including configurable Metal
  scene-heap capacity and OpenUSD 26.08 shared-SDK compatibility alongside the
  existing 26.05 contract.

### Changed

- `merlin-viewport --backend automatic` now selects native Metal presentation
  on Apple platforms while explicit Vulkan selection retains the existing GLFW
  surface path.
- Hydra viewport projection conversion now follows the selected native backend,
  preserving Vulkan's reflected Y convention without inverting Metal scenes.

## [0.11.0] - 2026-07-27

### Added

- Optional native `Merlin::Metal` backend and installed CMake component for
  Apple platforms, using the same backend-neutral `FrameSnapshot`,
  submission/completion, render-product, diagnostic, and telemetry contracts
  as Vulkan.
- Metal device/queue setup, runtime-compiled renderer-owned Forward shaders,
  heap-backed Mesh and texture residency, samplers, camera transforms, depth,
  opacity masks, offscreen color/depth/primId/instanceId AOVs, and selectable
  CPU readback.
- Tier-2 argument-buffer texture/sampler tables with conventional Forward
  fallback, finite generation-checked slots, completion-protected retirement,
  per-frame dirty-only encoding, capacity errors, and Metal-specific heap,
  table, allocation, and update telemetry.
- Apple Silicon runtime evidence for textured rendering, depth and picking IDs,
  alpha-mask discard, resource replacement, stable slots, and zero steady-state
  target allocation or argument-table update, plus hosted macOS Debug/Release
  compile/package coverage and an isolated `Merlin::Metal` install consumer.

### Changed

- Automatic backend selection now has a production Metal factory to select on
  Apple platforms; Core-only and Vulkan configurations retain their existing
  dependency and fast-path boundaries.
- Release metadata now records the optional Metal configuration and exported
  target.

## [0.10.0] - 2026-07-27

### Added

- OpenStrata 0.19.0 integration: CI pinning, a declared `viewport-usd` build
  intent, documented USD viewport launch, redacted diagnostics, and
  producer-session-bound renderer evidence.
- Optional `Merlin::MaterialX` package and install target using the official
  pinned MaterialXGenSlang generator without exposing MaterialX SDK types in
  its public API or adding MaterialX to Core.
- Deterministic graph-only `evaluateMaterial` source generation, logical
  input/uniform reflection, topology-only SHA-256 module identity, separate
  parameter/resource-state identities, and structured local diagnostics for
  the initial constant and add/multiply/mix prototype.
- A versioned, host-neutral generated-material contract in Core covering module
  identity/revision, typed parameter state, logical texture/sampler bindings,
  exact object/world/UV0 input requirements, and independent extraction
  revisions without MaterialX or backend-native types.
- SPIR-V and Metal-target compile wrappers and reflection evidence generated
  from the same MaterialX-produced Slang module when `slangc` is available.
- Image, UV0, world-normal, and minimum Standard Surface material-result
  generation for `base`, `base_color`, `metalness`, `specular_roughness`, and
  `normal`, with out-of-scope Standard Surface inputs diagnosed explicitly.
- Portable SHA-256 evidence for loaded MaterialX standard-library documents
  and transitive generator source includes, included in topology-only module
  identity without host-specific absolute paths.
- A host-neutral shader module identity and target-artifact key in Core that
  key MaterialX-generated and handwritten Slang identically, covering source
  and include content separately from compiler, target, profile, capability,
  layout, optimization, debug, target-option, ABI, permutation, and feature
  policy.
- `shaders/v2` artifact packages recording module sources, module identity, and
  artifact key per artifact, plus a test that recomputes every recorded
  identity through the Core contract so the build system cannot drift from it.
- A host-neutral material diagnostic and fallback contract in Core covering the
  required document, unsupported-node/input/conversion, missing
  library/include/texture, generation, compile, target, reflection, ABI, and
  cache failure categories; the material/element/node/input/document/target and
  generator/compiler context each record carries; the declared
  simplification, basic-material, and error-material fallback ladder with its
  host policy; and an evidence aggregate that summarizes the worst fallback
  reached. The contract belongs to Core, so a MaterialX document, a Hydra
  network, and a future material source classify the same failure identically.
  A record fills a context field only where it can attribute the failure that
  precisely, so a host is never pointed at an element or node that did not fail.
- `Merlin::MaterialX` diagnostics for unsupported type conversions, missing
  generator includes, and image resources the document left without a filename,
  plus the authored node category and input name on every record that is
  attributable to one.
- A host-neutral material ABI contract in Core covering what a consumer of a
  generated module declares it can supply and will read, whether a module
  satisfies it, whether a compiled artifact's reflected interface agrees with
  the module's logical one, and whether generated source declared any part of a
  render pass. Agreement between a module and a target is semantic rather than
  positional, so SPIR-V and Metal describe one material through their own native
  bindings; two targets that agree with one module therefore agree with each
  other. A consumer states its own half of the ABI, so the same module can be
  usable by a pass that reads only a base color and rejected by one that shades.
  A name the module itself declared twice is reported against the module rather
  than as a target disagreeing with whichever declaration was listed last.
- Cross-target ABI evidence recomputed from the reflection the Slang compiler
  emitted for every generated-material artifact: SPIR-V and Metal reflection for
  both the graph-only prototype and the minimum Standard Surface module is
  restated in the Core vocabulary and checked back against the module they were
  built from, together with the negative cases that prove a dropped, retyped, or
  undeclared parameter is caught.
- A `Merlin::MaterialX` bridge that classifies integration-local diagnostics
  against the Core contract and flattens them onto the existing host-neutral
  `merlin-diagnostic/v1` sink, so hosts keep one diagnostic stream while
  evidence consumers keep the structure. A rejected document reports the
  basic-material fallback rather than a simplification it never generated, and
  a warning against a material that still generated takes no rung at all: it
  reaches the host as an ignored record and is not counted as a substitution in
  fallback evidence.
- Vulkan Forward execution for registered parameter-only generated-material
  artifacts. The backend verifies the Core consumer ABI and target reflection,
  packs typed instance values into target-specific uniform offsets, reuses the
  module pipeline across value-only edits, and retains renderer ownership of
  lighting, depth, picking, AOV writes, and the render pass.
- Runtime generated-material capability, draw, fallback, and worst-rung
  telemetry on both the Vulkan-native and backend-neutral result contracts,
  together with structured diagnostics for missing/corrupt artifacts,
  ABI/reflection disagreement, and unsupported bindless composition.
- Vulkan Forward texture/sampler execution for generated MaterialX modules,
  with per-module concrete descriptor and pipeline layouts, logical-resource
  validation, reuse of existing texture/sampler residency, and explicit
  missing-resource fallback.
- GPU image evidence for the MaterialX image, multiply, world-normal, and
  minimum Standard Surface path, including texture-content edits that reuse
  the generated pipeline.
- Retained and installed MaterialX prototype and Standard Surface Slang,
  SPIR-V, Metal-target, and reflection artifacts under
  `shaders/v2/materialx`.
- Generated-material draw/fallback/worst-rung evidence in benchmark, headless
  renderer-report, native viewport benchmark, and Hydra regression
  serialization.

### Changed

- Module, parameter, resource, and artifact identities now share one
  length-prefixed record encoding and SHA-256 implementation in Core rather
  than a MaterialX-local copy. Field names are length-prefixed alongside field
  values, so no name or value can imitate a record separator.
- Shader packages moved from `shaders/v1` to `shaders/v2`. Each artifact now
  records `module_sources` and `module_identity` in place of
  `dependency_sha256`, and `artifact_key` in place of `cache_key`; the package
  `sources` inventory and every module's include closure are recovered from the
  depfile the compiler emitted rather than declared by the build system.
- The Forward shader ABI is version 3: conventional material constants moved
  to descriptor binding 31, reserving bindings 1 through 30 for generated
  material resources while keeping renderer-owned vertex and fragment stages
  on one layout.
- Shader debug policy is a compile input that reaches both the `slangc`
  invocation and the artifact key, rather than a value the manifest asserted.
- A reflected value whose MaterialX type has no MaterialIR equivalent, or whose
  authored default cannot be read as its declared type, is diagnosed as an
  unsupported conversion rather than an unsupported input. The graph is
  supported in those cases; only the boundary crossing is not. A generator
  include or standard-library document that cannot be read, or that resolves
  outside every registered data root, is likewise diagnosed as a missing
  include rather than a generic generation failure.
- An image node the document left without a filename is now rejected at
  generation instead of producing a module carrying a resource identifier no
  host could resolve.
- `Merlin::MaterialX` checks its own generated source against the Core
  pass-neutrality rule and rejects a module that declared an entry point, bound
  a system value, named a binding slot or a pass mode, allocated group-shared
  storage, or discarded a fragment. hdMerlin owns the render pass, so such a
  module would take part of it over once a renderer composed it; it is now
  refused at generation rather than noticed by whoever composed it. Whole
  families are rejected rather than their members seen so far, so any
  `[[vk::...]]` attribute counts. A struct field merely *named* after a system
  value is left alone, a `:` that closes a conditional expression binds nothing,
  and a construct named in a comment or a string literal is not one that was
  declared.
- Forward lighting now accepts renderer-composed surface color and shading
  normal inputs through a pass-owned helper; the existing handwritten
  base-color/texture path remains an adapter over that same function.
- `CompileOptions` accepts a logical `source_document` identifier, and
  `CompileResult` echoes it alongside the MaterialX and generator versions, so
  a failed compile still reports where it came from and which versions produced
  it without a host-absolute path entering a diagnostic. A dependency that
  cannot be read or resolved is likewise reported by filename rather than by its
  resolved host path, and a bridged version names what it is a version of, so a
  host never has to guess whether it is reading the MaterialX library version or
  the shader generator's.

### Removed

- `MaterialFunctionModule::cache_key`, the transitional alias for
  `module_key`. A compiled artifact's identity is now a distinct key built by
  `merlin::MakeShaderArtifactKey`, so one field named after "cache" no longer
  stands for two different questions.

## [0.9.0] - 2026-07-18

### Added

- `Merlin::RenderBackend`, a backend-neutral factory, selection, capability,
  limit, submission, completion, presentation-target, telemetry, and structured
  error contract used by Vulkan and Hydra without native GPU/window types.
- `merlin-viewport`, a GLFW-hosted Vulkan application with resize, camera,
  picking, screenshots, benchmark output, vsync control, and optional OpenUSD
  stage loading through Hydra with bounds-based initial camera framing.
- Vulkan swapchain presentation with GPU-only offscreen-to-present blits,
  out-of-date/resize recovery, per-image completion semaphores, validation
  coverage, and FIFO or vsync-off present-mode selection.
- Boundary, install-consumer, viewport/headless parity, zero-readback,
  resize, Vulkan validation, and non-background Hydra USD viewport tests.

### Changed

- Hydra render execution now consumes the backend-neutral renderer contract;
  normal native viewport frames omit CPU AOV readback while usdview retains its
  RenderBuffer compatibility path.
- Hydra USD viewport framing and navigation now follow usdview's free-camera
  defaults and gestures, honor Y/Z `upAxis`, and preserve front-face culling
  while correcting Vulkan framebuffer Y orientation.
- Release metadata now records the viewport product and pinned GLFW 3.4
  fallback commit.

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

[Unreleased]: https://github.com/animu-sphere/hydra-merlin/compare/v0.12.0...main
[0.12.0]: https://github.com/animu-sphere/hydra-merlin/compare/v0.11.0...v0.12.0
[0.11.0]: https://github.com/animu-sphere/hydra-merlin/compare/v0.10.0...v0.11.0
[0.10.0]: https://github.com/animu-sphere/hydra-merlin/compare/v0.9.0...v0.10.0
[0.9.0]: https://github.com/animu-sphere/hydra-merlin/compare/v0.8.0...v0.9.0
[0.8.0]: https://github.com/animu-sphere/hydra-merlin/compare/v0.7.0...v0.8.0
[0.7.0]: https://github.com/animu-sphere/hydra-merlin/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/animu-sphere/hydra-merlin/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/animu-sphere/hydra-merlin/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/animu-sphere/hydra-merlin/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/animu-sphere/hydra-merlin/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/animu-sphere/hydra-merlin/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/animu-sphere/hydra-merlin/releases/tag/v0.1.0
