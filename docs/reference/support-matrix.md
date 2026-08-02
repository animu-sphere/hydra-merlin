# Support matrix

**Status:** v0.14.0 released · v0.14.1 complete pending release · **Last reviewed:** 2026-08-02

This matrix separates a required contract from a configuration actually
exercised by project CI or local capability validation. An unlisted platform may
work, but is not currently claimed as supported evidence.

## Toolchain and platform coverage

| Platform | Core | Vulkan/headless | Vulkan viewport | Hydra 2 | Evidence level |
| --- | --- | --- | --- | --- | --- |
| Windows x64, Visual Studio 2022 | Debug/Release | Debug/Release with Vulkan 1.4 | Debug/Release with GLFW; Release with Hydra USD loading | Release with OpenUSD 26.05 and 26.08 | Core hosted CI plus local Vulkan/MaterialX, native viewport, Hydra host-presentation, and Gaussian corpus validation; capability workflow retains the hardware evidence |
| Linux x64, hosted runner with Ninja | Debug/Release | Not continuously exercised | Not continuously exercised | Not continuously exercised | Core hosted CI |
| macOS 14, Apple Silicon, AppleClang 16 | Debug/Release | Native Metal offscreen Debug/Release; Vulkan not claimed | Native Metal `CAMetalLayer` viewport; Vulkan not claimed | Release with OpenUSD 26.08 and Metal | Hosted compile/package coverage plus local Apple GPU runtime AOV/residency, native presentation, and Kitchen Set Hydra validation |

A repository-scoped Windows x64 GPU runner is enrolled with the `vulkan-1.4`
label. The manual capability workflow exercises Vulkan Debug/Release and Hydra
Release on demand; it is capability evidence rather than a per-commit required
check.

## Dependency contract

| Dependency | Minimum or validated version | Required by |
| --- | --- | --- |
| CMake | 3.24 | All builds |
| C++ compiler | C++20 | All builds |
| OpenStrata CLI | 0.21.0 | Managed build/validation, renderer viewport intents, redacted diagnostics, runtime provenance, and capability CI |
| Vulkan headers/loader/device | 1.4 | Vulkan/headless and Vulkan-backed Hydra |
| Vulkan SDK `slangc` | Slang 2026.8.x; Vulkan SDK 1.4.350.0 in capability workflow | Shader build and SPIR-V/Metal compile gates |
| GLFW | 3.4; pinned commit fallback recorded in release metadata | `merlin-viewport` window/input plus Vulkan and Cocoa/Metal presentation adapters |
| Dear ImGui | 1.92.8 at pinned revision `8936b58fe26e8c3da834b8f60b06511d537b4c63` | Private `merlin-viewport` development UI |
| OpenUSD | 26.05 and 26.08 currently validated | Hydra 2 only |
| MaterialX | 1.39.6 prototype pin at `38368ee04da84ce1f8837ecba7322dd6d81291f8`; source builds require CMake 3.26+ | Optional `Merlin::MaterialX` compiler |
| Metal | macOS system framework; Metal argument-buffer tier 2 negotiated at runtime | Optional `Merlin::Metal` backend |
| Python + `testusdview` | Matching the OpenUSD runtime | Install-tree Hydra host test |

Hydra configuration verifies an accepted OpenUSD header version and the shared
library target layout. On MSVC it rejects a Debug hdMerlin build when the SDK
does not export Debug libraries. Plugin discovery and the install-tree usdview
test then load the runtime from that SDK root. Compiler/toolset ABI differences
between separately produced OpenUSD SDKs remain the operator's responsibility.

## Product and feature coverage

| Capability | Current status |
| --- | --- |
| Host-neutral scene model and draw extraction | Available |
| Host-neutral MaterialIR and revisioned texture/sampler resources | Available |
| Backend-neutral renderer contract | `Merlin::RenderBackend` provides factory/selection, renderer capabilities and limits, logical presentation/completion handles, submit/resolve, common telemetry, and errors without concrete GPU/window types |
| Native Vulkan viewport | `merlin-viewport` provides GLFW window/input, a Dear ImGui capability/timing/residency/AOV/material diagnostic surface, usdview-style tumble/track/dolly/frame-all navigation with Y/Z `upAxis`, resize, click-triggered ID picking, screenshots, benchmark mode, vsync selection, and optional Hydra USD loading |
| Vulkan swapchain presentation | GPU-only offscreen-to-swapchain blit with per-image completion, out-of-date/resize recovery, zero CPU readback by default, and exact offscreen product parity evidence |
| Basic Vulkan material shading | Base/vertex color, display opacity, normals, UV RGBA8 textures, directional light, opaque/alpha-mask, and double-sided state are available |
| Vulkan color/depth/primId/instanceId rendering and CPU readback | Available for Mesh and the CPU-sorted Gaussian MVP; Gaussian `primId` is the resource handle and `instanceId` is the zero-based particle index of the nearest contributing splat in front of opaque depth |
| Explicit submit/completion/timeout-aware resolve | Available |
| Per-request AOV request and CPU readback selection | CPU transfer is selectable for color, depth, primId, and instanceId; the current fixed pass may still write unrequested attachments |
| PNG/EXR expected/actual/diff regression artifacts | Exact comparison is available for color, depth, primId, and instanceId |
| Deterministic benchmark and comparison JSON | v3 CPU/GPU stage distributions, bindless/geometry/transfer/VRAM residency telemetry, fixed scale/AOV/4K fixtures, structural regression gates, and opt-in controlled-hardware timing thresholds are available |
| Hydra/host performance evidence | Versioned phase summaries plus raw OpenUSD Chrome traces cover delegate, scene-index, renderer, CPU-to-Hgi upload, composite, and presentation scopes; the bundled 8,192-particle Gaussian corpus has Tier 0/HgiVulkan reference-image smokes and a 300-frame native viewport capture |
| Installed CMake targets | `Merlin::RenderWorld`, `Merlin::RenderExtraction`, `Merlin::RenderBackend`, optional `Merlin::Vulkan`, optional `Merlin::Metal`, and optional `Merlin::MaterialX` are available |
| Versioned dependency and package metadata | Available as installed JSON |
| Tag-driven Core SDK release automation | Available for stable SemVer tags |
| Hydra 2 indexed/face-varying mesh primvars and robust triangulation | Available with persistent per-path source caches, semantic revisions, and changed-range upload; OpenUSD 26.05 may emit a coarse `primvars` locator, which is value-compared before rebuild/upload |
| Hydra material and light translation | Authored binding identity plus a basic `UsdPreviewSurface`/`UsdUVTexture` and distant-light subset are available; general MaterialX/network translation remains planned |
| Slang shader source and Metal compile gate | Slang is the Forward source of truth; conventional and bindless SPIR-V plus conventional Metal/reflection artifacts are packaged under `shaders/v2`, with Metal non-uniform bindless access explicitly falling back to conventional Forward |
| Shader module and target-artifact identity | Available: one host-neutral contract keys generated and handwritten Slang. Module identity covers source and include content, recovered from the compiler's own depfile; a separate artifact key adds compiler, target, profile, capability, layout, optimization, debug, and target-option policy. The build writes both into the shader manifest and a test recomputes every one of them through the contract |
| MaterialXGenSlang material-function prototype | Available for v0.10.0: optional `Merlin::MaterialX` emits deterministic graph-only Slang functions and renderer-owned minimum Standard Surface results for constants, image/UV0/world-normal, add/multiply/mix, `base`, `base_color`, `metalness`, `specular_roughness`, and `normal`. Portable library/include fingerprints feed topology-only module keys separated from parameter/resource state. Registered parameter-only and texture/sampler artifacts execute in renderer-owned Vulkan Forward after ABI/reflection checks, reuse pipelines across value and texture-content edits, and report structured fallback/capability telemetry. The same sources retain installed SPIR-V, Metal-target, and reflection evidence. General MaterialX documents, tangent-space normal mapping, production IBL, and Hydra MaterialX ingestion are not claimed |
| Material ABI agreement | `merlin.material-abi/v1` is available in Core. A consumer declares the result fields it reads and the geometry inputs it can build, and a module is checked against that rather than in isolation. A compiled artifact's reflected interface is checked back against the module's logical one by name, type, and array size, so SPIR-V and Metal describe one material through their own native bindings and agree with each other by agreeing with the module. The same contract owns the pass-neutrality rule, which `Merlin::MaterialX` applies to its own output; both generated modules and all four of their SPIR-V/Metal artifacts are checked in the test suite, including the dropped, retyped, and undeclared-parameter cases |
| Native Metal backend and residency | Available for offscreen Mesh Forward: native device/queue, runtime MSL, buffers/textures/samplers, heap residency, generation-checked argument-buffer tables with conventional fallback, frames-in-flight retirement, basic material/opacity mask, color/depth/primId/instanceId AOVs, CPU readback, capacity diagnostics, and Metal-specific telemetry |
| Native Metal viewport presentation | Available: adapter-owned `CAMetalLayer`, renderer-owned drawable encoding, GPU-only offscreen-to-drawable presentation, resize/frames-in-flight safety, sRGB/Display P3 SDR policy with an explicit future HDR boundary, vsync/drawable-count pacing, Dear ImGui integration, presentation telemetry, and exact offscreen reference parity |
| HgiVulkan host presentation bridge | Available since v0.13.0: public-driver discovery, Hgi-owned color targets, Tier 0 upload fallback, and selected color GPU copy are supported on the validated OpenUSD 26.05/26.08 packages when their imported `hgiVulkan` target is present. Merlin borrows the Hgi Vulkan 1.3 device/graphics queue for its conventional renderer path, exports one color image, copies it with explicit barriers, and releases its lease from Hgi command-buffer completion; depth and id AOVs stay on CPU readback. Both runtime smokes prove no color Map/upload and no coarse wait through resize, and the 13-phase image/performance fixture supplies Tier 0 comparison evidence. Merlin-owned Vulkan remains 1.4. v0.13.1 adds explicit direct-share gates and stable rejection telemetry; current packages report `public-texture-import-unavailable` because public Hgi cannot import a Merlin-owned `VkImage`, so GPU copy remains selected. |
| HgiMetal host presentation bridge | Available since v0.14.0 on validated OpenUSD 26.05/26.08 packages: Hgi-owned color targets receive same-device Metal GPU copies from leased renderer AOVs with completion-safe resize retirement and Tier 0 fallback. Direct sharing remains rejected because the public host texture-import contract is unavailable. |
| Hydra native and nested instancing | Available |
| Bindless texture/sampler residency | Finite generation-checked tables, reserved fallback images, dirty Vulkan descriptor writes, deduplicated samplers, completion-safe replacement, and telemetry are available on negotiated devices |
| Bindless Forward and common GPU Scene ABI | Non-uniform-indexed Forward is automatically selected after feature/limit negotiation and has exact conventional-path image parity coverage; complete geometry/material/instance/draw tables remain planned for v0.15.0, and conventional Forward remains the fallback |
| Asynchronous resource upload | A dedicated transfer family is selected when timeline semaphores are available; geometry uses cross-family concurrent buffers, sampled images use explicit ownership/layout transitions, and a single-queue path remains the fallback |
| VRAM budget and exhaustion | `VK_EXT_memory_budget` capacity/budget/usage is probed when available; all renderer device-local allocations have current/peak telemetry, an optional hard byte limit, and actionable `resource-exhausted` failure |
| GPU-driven indexed Mesh submission | Planned for v0.16.0; current Mesh submission is not claimed to have draw-count-independent CPU cost |
| Opaque Visibility Buffer | Planned experimental path for v0.17.0; current shading is Forward |
| Static meshlet indexed-indirect rendering | Planned for v0.19.0 from standard Hydra mesh data; no custom USD schema is planned |
| Mesh Shader, Hi-Z, and discrete meshlet LOD | Planned as optional, capability- and benchmark-selected v0.20.0 paths with indexed fallback |
| Hierarchical meshlets and virtualized geometry | Post-v1 research direction; unavailable and not implied by static meshlet support |
| Structured render errors | The common boundary exposes stable invalid-request/token, resource-busy/exhausted, timeout, device-lost, unsupported, backend-unavailable, and backend-failure classes; Vulkan maps native failures into it |
| Host-neutral diagnostic sink | `merlin-diagnostic/v1` is available with stable codes, dispositions, source paths, and named recovery; Hydra forwards records to OpenUSD diagnostics and telemetry |
| Material diagnostic and fallback contract | `merlin.material-diagnostic/v1` is available in Core with stable per-category codes, material/element/node/input/document/target and generator/compiler context, the declared simplification, basic-material, and error-material ladder under host policy, and an evidence aggregate reporting the worst fallback reached. Records flatten onto `merlin-diagnostic/v1`, and Vulkan Forward exposes the aggregate through frame telemetry and the renderer capability report. |
| Standard OpenUSD Gaussian ingestion and rendering | Available as the v0.14.1 Vulkan correctness MVP: OpenUSD 26.05/26.08 `ParticleField3DGaussianSplat` reaches Hydra `particleField`, host-neutral normalized resources, deterministic CPU projection/culling/SH evaluation/sorting, procedural ellipse rasterization, mixed Mesh composition, localized packed-stream uploads, Gaussian IDs/picking, telemetry, and bundled CC0 corpus/reference-image evidence. Persistent device-local resources follow in v0.15.0 and GPU-driven projection/sorting in v0.16.0. |
| Subdivision refinement | Unavailable |
| Dome/multi-light viewport lighting, shadows, alpha blending, and production culling | Unavailable; basic Mesh and Gaussian ID picking is available |
| Vulkan/Hgi low-copy host presentation | Selected color GPU copy is available with the validated HgiVulkan packages; CPU readback/upload remains the universal fallback and serves depth/id AOVs. Same-device direct sharing is explicitly rejected as `public-texture-import-unavailable` on OpenUSD 26.05/26.08, and future external interop remains separately capability-, public-contract-, and benchmark-gated. |
| Houdini, Husk, Hydra 1, and Maya integration packages | Unavailable |

## Future GPU path capability tiers

These tiers describe selection policy, not current support claims. Every feature
and relevant limit is probed and included in the versioned capability report;
meeting the Vulkan 1.4 baseline alone does not select every fast path.

| Path | Additional selection contract | Required fallback |
| --- | --- | --- |
| Bindless Forward / GPU Scene | Required descriptor-indexing, non-uniform access, table-size, and update/lifetime behavior pass validation | Conventional Forward descriptors |
| GPU-driven indexed | Indirect count/draw identity and compute culling pass correctness and performance gates | Conventional indexed submission |
| Visibility | Storage/compute resolve and ID attachment support; fragment-shader barycentrics are optional because reconstruction is available | Forward opaque/material fallback |
| Meshlet indexed indirect | Compute compaction and meshlet data limits pass builder, culling, identity, and performance gates | GPU-driven or conventional indexed geometry |
| Meshlet Mesh Shader | `VK_EXT_mesh_shader`, device limits, subgroup behavior, driver stability, and a named-profile benchmark win | Meshlet indexed indirect |

The complete ABI, encoding, and fallback rules are in the
[GPU-driven rendering policy](../design/gpu-driven-rendering.md).

Unsupported inputs must produce an actionable diagnostic or explicit fallback;
they are not implied to work by the availability of the surrounding adapter.
Future capability order and completion criteria are tracked in the
[current milestone](../roadmap/current.md) and [backlog](../roadmap/backlog.md).
The v0.10.0 ownership and acceptance contract is the
[MaterialXGenSlang material boundary](../design/materialxgenslang-boundary.md).
