# Support matrix

**Status:** v0.4.0 implementation · **Last reviewed:** 2026-07-14

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
| Vulkan headers/loader/device | 1.4 | Vulkan/headless and Hydra |
| Vulkan SDK `glslc` | Compatible with Vulkan 1.4; 1.4.350.0 in capability workflow | Shader build |
| OpenUSD | 26.05 currently validated | Hydra 2 only |
| Python + `testusdview` | Matching the OpenUSD runtime | Install-tree Hydra host test |

OpenUSD shared/static mode, Debug/Release configuration, MSVC runtime, and plugin
ABI compatibility are currently operator responsibilities. Configure-time
compatibility checks remain planned work.

## Product and feature coverage

| Capability | Current status |
| --- | --- |
| Host-neutral scene model and draw extraction | Available |
| Vulkan color/depth/primId/instanceId rendering and CPU readback | Available |
| Explicit submit/completion/timeout-aware resolve | Available |
| Per-request AOV production and CPU readback selection | Available for color, depth, primId, and instanceId |
| PNG/EXR expected/actual/diff regression artifacts | Available for color, depth, and primId |
| Deterministic reference-path benchmark JSON and structural counters | Available |
| Core/Vulkan installed CMake targets | Available |
| Versioned dependency and package metadata | Available as installed JSON |
| Tag-driven Core SDK release automation | Available for stable SemVer tags |
| Hydra 2 indexed/face-varying mesh primvars and robust triangulation | Available |
| Authored Hydra material binding identity | Available; material-network shading remains planned |
| Hydra native and nested instancing | Available |
| Subdivision refinement | Unavailable |
| Advanced viewport lighting, shadows, selection, transparency, and culling | Unavailable |
| Vulkan/Hgi external-memory or zero-copy host presentation | Unavailable; CPU readback/upload reference path only |
| Houdini, Husk, Hydra 1, and Maya integration packages | Unavailable |

Unsupported inputs must produce an actionable diagnostic or explicit fallback;
they are not implied to work by the availability of the surrounding adapter.
