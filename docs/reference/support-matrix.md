# Support matrix

**Status:** v0.6.0 · **Last reviewed:** 2026-07-16

This matrix separates a required contract from a configuration actually
exercised by project CI or local capability validation. An unlisted platform may
work, but is not currently claimed as supported evidence.

## Toolchain and platform coverage

| Platform | Core | Vulkan/headless | Hydra 2 | Evidence level |
| --- | --- | --- | --- | --- |
| Windows x64, Visual Studio 2022 | Debug/Release | Debug/Release with Vulkan 1.4 | Release with OpenUSD 26.05 | Core hosted CI; GPU/Hydra local validation and manual capability workflow |
| Linux x64, hosted runner with Ninja | Debug/Release | Not continuously exercised | Not continuously exercised | Core hosted CI |
| macOS | Not validated | Not validated | Not validated | No current claim |

A repository-scoped Windows GPU runner has not yet been enrolled, so the manual
capability workflow is not continuous evidence. Enrollment is tracked in the
[backlog](../roadmap/backlog.md#cross-cutting-open-items).

## Dependency contract

| Dependency | Minimum or validated version | Required by |
| --- | --- | --- |
| CMake | 3.24 | All builds |
| C++ compiler | C++20 | All builds |
| OpenStrata CLI | 0.17.0 | Managed build/validation, managed Hydra view, and capability CI |
| Vulkan headers/loader/device | 1.4 | Vulkan/headless and Hydra |
| Vulkan SDK `glslc` | Compatible with Vulkan 1.4; 1.4.350.0 in capability workflow | Shader build |
| OpenUSD | 26.05 currently validated | Hydra 2 only |
| Python + `testusdview` | Matching the OpenUSD runtime | Install-tree Hydra host test |

Hydra configuration verifies the exact OpenUSD 26.05 header version and shared
library target layout. On MSVC it rejects a Debug hdMerlin build when the SDK
does not export Debug libraries. Plugin discovery and the install-tree usdview
test then load the runtime from that SDK root. Compiler/toolset ABI differences
between separately produced OpenUSD 26.05 SDKs remain the operator's
responsibility.

## Product and feature coverage

| Capability | Current status |
| --- | --- |
| Host-neutral scene model and draw extraction | Available |
| Host-neutral MaterialIR and revisioned texture/sampler resources | Available |
| Basic Vulkan material shading | Base/vertex color, display opacity, normals, UV RGBA8 textures, directional light, opaque/alpha-mask, and double-sided state are available |
| Vulkan color/depth/primId/instanceId rendering and CPU readback | Available |
| Explicit submit/completion/timeout-aware resolve | Available |
| Per-request AOV request and CPU readback selection | CPU transfer is selectable for color, depth, primId, and instanceId; the current fixed pass may still write unrequested attachments |
| PNG/EXR expected/actual/diff regression artifacts | Available for color, depth, and primId |
| Deterministic benchmark and comparison JSON | v3 CPU/GPU stage distributions, bindless/geometry/transfer/VRAM residency telemetry, fixed scale/AOV/4K fixtures, structural regression gates, and opt-in controlled-hardware timing thresholds are available |
| Hydra/host performance evidence | Versioned phase summaries plus raw OpenUSD Chrome traces cover delegate, scene-index, renderer, CPU-to-Hgi upload, composite, and presentation scopes |
| Core/Vulkan installed CMake targets | Available |
| Versioned dependency and package metadata | Available as installed JSON |
| Tag-driven Core SDK release automation | Available for stable SemVer tags |
| Hydra 2 indexed/face-varying mesh primvars and robust triangulation | Available with persistent per-path source caches, semantic revisions, and changed-range upload; OpenUSD 26.05 may emit a coarse `primvars` locator, which is value-compared before rebuild/upload |
| Hydra material and light translation | Authored binding identity plus a basic `UsdPreviewSurface`/`UsdUVTexture` and distant-light subset are available; general MaterialX/network translation remains planned |
| Slang shader source and Metal compile gate | Planned for v0.8.0; the current Vulkan path still builds GLSL with `glslc` |
| MaterialXGenSlang material-function prototype | Planned for v0.10.0; production MaterialX and Visibility quality remains v0.18.0 work |
| Native Metal backend and residency | Planned for v0.11.0; no current Metal execution support is claimed |
| Native Metal viewport presentation | Planned for v0.12.0 after the backend-neutral viewport boundary |
| HgiMetal host presentation bridge | Planned for v0.13.0 with direct-share, GPU-copy, and CPU-readback fallback tiers validated per supported host |
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
| Structured render errors | Vulkan boundary exposes stable invalid-request/token, resource-busy/exhausted, timeout, device-lost, unsupported, and backend-failure classes |
| Host-neutral diagnostic sink | `merlin-diagnostic/v1` is available with stable codes, dispositions, source paths, and named recovery; Hydra forwards records to OpenUSD diagnostics and telemetry |
| Standard OpenUSD Gaussian ingestion and rendering | The OpenUSD 26.05 `ParticleField3DGaussianSplat` → `usdVolImaging` → Hydra `particleField` boundary is accepted and documented; host-neutral Gaussian resources and rendering remain v0.14.0 work |
| Subdivision refinement | Unavailable |
| Dome/multi-light viewport lighting, shadows, selection, alpha blending, and production culling | Unavailable |
| Vulkan/Hgi low-copy host presentation | Unavailable; CPU readback/upload reference path remains universal and any v0.13.0 low-copy adapter is evidence-gated |
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
