# Backlog

Planned but inactive work follows the [current milestone](current.md).
Shipped changes belong in the [changelog](../../CHANGELOG.md), and pipeline
contracts belong in [design](../design/). Version labels may move when
capability, integration, or benchmark evidence changes the dependency order.

## Phase D — GPU scalability and shading quality

### Late v0.16.x onward — Forward lighting and shading quality

**Goal:** Stabilize native and Hydra-hosted Forward images under camera motion,
including camera-light energy, direct/environment lighting, linear/sRGB
boundaries, exposure, tone mapping, and handwritten/generated material parity.

**Depends on:** The active GPU-driven foundation and a declared host camera-light
contract. Forward remains the reference for optional shading paths.

**Exit:** Differential Kitchen and focused material fixtures cover Camera Light
ON/OFF, motion/static frames, Tier 0 and Hgi presentation, and both backends
where available. No supported mode needs the camera light disabled to avoid
white clipping or flicker; costs and unsupported lighting are diagnosed.

### v0.17.0 — Contribution-aware Gaussian culling and adaptive bounds

**Goal:** Add conservative Gaussian/tile contribution bounds and selectable
quality thresholds; retain `Exact` as the validation path.

**Depends on:** Image-producing GPU Gaussian tiling from the
[current milestone](current.md). See the
[Gaussian rendering design](../design/gaussian-rendering.md).

**Exit:** Rejection, pair, saturation, and early-termination counters accompany
image and temporal-tolerance evidence without unexplained view-dependent
popping.

### Experimental opaque Visibility Buffer — independent Mesh track

**Goal:** Add selectable opaque indexed-triangle ID raster and basic-PBR
resolve, with Forward handling transparent or unsupported materials.

**Depends on:** GPU-driven indexed Forward and the common GPU Scene. See the
[GPU-driven design](../design/gpu-driven-rendering.md).

**Exit:** Stable picking/primitive identity, deterministic Forward differential
images, explicit pass barriers and timestamps, and separate raster/resolve
cost evidence.

### v0.18.0 — Hierarchical Gaussian tiles; production MaterialX and lighting

**Goal:** Select bounded two-level Gaussian tiling only when measured 4K/8K
p95/p99 benefit justifies it. Independently broaden Standard Surface,
UV/normal/opacity/emissive coverage, environment and authored lighting, shadows,
asynchronous compilation, residency, and Forward/Visibility material parity.

**Depends on:** Flat Gaussian tile evidence, the
[MaterialX function boundary](../design/materialxgenslang-boundary.md), and
Forward quality fixtures. Material evaluation does not own lighting or passes.

**Exit:** Overflow falls back safely; representative material/lighting fixtures
and backend comparisons meet declared tolerances. Unsupported inputs have
structured fallback; static and parameter-only edits avoid steady-state
compile/pipeline creation.

### v0.19.0 — Temporal Gaussian reuse; static meshlet rendering

**Goal:** Reuse only compatibility-classified Gaussian preprocessing and build
material-homogeneous meshlets from standard Hydra mesh data, with indexed-indirect
submission and conventional fallbacks.

**Depends on:** Stable Gaussian tile/quality evidence and GPU-driven indexed
Mesh execution. See the [Gaussian](../design/gaussian-rendering.md) and
[GPU-driven](../design/gpu-driven-rendering.md) designs.

**Exit:** `PreprocessOnly` is exact under scene/camera/target invalidation;
meshlet build/cache behavior and material partitions are deterministic,
Forward/Visibility images agree within tolerance, and large static scenes
improve without regressing small or dynamic fallbacks.

### v0.20.0 — Gaussian LOD/compression; optional Mesh Shader, Hi-Z, mesh LOD

**Goal:** Add deterministic Gaussian chunk LOD and versioned compression with
full-precision fallback. Add benchmark-selected Mesh Shader, conservative
previous-frame Hi-Z, and meshlet LOD behind indexed-indirect fallback.

**Depends on:** Stable Gaussian residency and static indexed meshlets.

**Exit:** Image/temporal evidence covers LOD transitions, invalidation, and
motion; a declared hardware profile shows a repeatable Mesh Shader win, with
correct indexed fallback elsewhere.

## Phase E — Large scenes, hosts, and production readiness

### v0.21.0 — Gaussian streaming and parallel ingestion

**Goal:** Add prioritized chunk requests, asynchronous decode/upload, bounded
VRAM eviction/prefetch, and deterministic parallel Hydra processing and
RenderWorld commits. Other resource streaming remains independently gated.

**Depends on:** Completion-safe Gaussian LOD/residency and stable extraction.

**Exit:** Over-VRAM scenes stream needed chunks with bounded hitches and quality
fallback; large dirty sets scale across CPU cores without nondeterministic
scene output or uninitialized memory.

### v0.22.0 — Gaussian backend parity and production hardening

**Goal:** Align Vulkan/Metal Gaussian ABI, quality, AOV/picking, telemetry, and
reference tolerances while allowing backend-specific kernels.

**Depends on:** Validated Gaussian optimization and fallback modes.

**Exit:** Comparable backend images and costs, overflow/device-loss diagnostics,
cache recovery, reproducible benchmarks, and host-presentation validation.

### Production host integrations

**Goal:** Integrate Houdini Solaris viewport, then Husk batch, Hydra 1 where
required by a supported host, and Maya Hydra when justified.

**Depends on:** Stable Hydra 2, settings/diagnostics, capability reporting,
package discovery, AOV/picking semantics, and documented material/lighting
boundaries. DCC packages own host metadata and UI; Core has no DCC SDK types.

**Exit:** Each supported host has discoverable packages, runtime compatibility
diagnostics, representative scene smokes, and comparable frame-stage evidence.
The [OpenStrata composition contract](../design/openstrata-project.md) governs
runtime artifact assembly.

### v1.0.0 — Production interactive renderer

**Goal:** Productize native Vulkan and Apple Silicon Metal viewports, stable
Hydra 2 and at least one production DCC integration, production materials and
lighting, standard Gaussian rendering, selection, diagnostics, and packaging.

**Depends on:** Versioned settings, declared capabilities, explicit fallback,
and cross-platform correctness/performance evidence. Visibility, Mesh Shader,
Hi-Z, out-of-core support for every resource, and complete MaterialX graphs
remain optional rather than universal release gates.

**Exit:** Each claimed platform, host, and rendering path has documented AOV,
material, lighting, fallback, compatibility, and performance evidence in the
[support matrix](../reference/support-matrix.md).

## Cross-cutting planned work

- **GPU capability matrix:** Expand validated NVIDIA/AMD and optional Intel
  hardware profiles and add Metal GPU evidence; separate missing-runner evidence
  from product failure. Extend feature/limit reporting before new paths use it.
- **OpenUSD compatibility:** Extend validated shared-SDK configurations with
  runtime plugin ABI diagnostics for supported hosts.
- **Build and exported products:** Justify independent options and resolve
  `Merlin::Hydra2`/`Merlin::Headless` packaging without adding OpenUSD as a
  Core/Vulkan transitive dependency.
- **OST template extraction:** Extract only boundaries proven by Merlin and a
  second consumer; retain renderer-specific policies until separately proven.
- **Post-v1 research:** Hierarchical meshlets and virtualized geometry require
  separately approved simplification, crack handling, streaming, paging,
  residency, and recovery evidence; static meshlets do not imply them.
