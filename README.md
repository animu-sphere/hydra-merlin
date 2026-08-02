# hdMerlin

[![Core CI](https://github.com/animu-sphere/hydra-merlin/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/animu-sphere/hydra-merlin/actions/workflows/ci.yml)

hdMerlin is an OST-oriented, host-neutral raster renderer with Vulkan and native
Metal backends. The current implementation provides a handle-based
`RenderWorld`, deterministic extraction into an immutable resource-granular
`FrameSnapshot`, a backend-neutral render contract, persistent Vulkan offscreen
and GLFW-hosted swapchain presentation, and native Metal offscreen execution.
Submission/completion lifetime and selectable color/depth/primId/instanceId CPU
readback remain explicit. Its host-neutral `MaterialIR` supports revisioned
texture/sampler bindings and basic directional-lit, textured, vertex-colored,
opaque or alpha-masked shading.

The core library intentionally has no OpenUSD, Hydra, DCC, Qt, Vulkan, or Metal
types in its public API. Hydra and host integrations remain thin adapters
around that core.

## OpenStrata project

The repository is an OpenStrata renderer project targeting `cy2026`. OST 0.21.0
or newer is required for managed-build progress and timeout diagnostics,
profile-local preset generation, and the managed renderer launch lifecycle.
The default host-neutral lifecycle is:

```powershell
ost runtime pull cy2026 --profile core
ost build --check
ost build --jobs auto
ost validate --json
```

The existing CMake targets remain project-owned renderer units; adopting OST
does not split them into artificial packages or plugin bundles. Vulkan builds
emit the renderer evidence consumed by `ost validate`. For Hydra inspection,
materialize or adopt one real `usd`/`lookdev` OpenUSD runtime, then run `ost
renderer view --profile usd`. With no `--build-dir`, OST requests the `hydra2`
build intent, incrementally configures/builds a fingerprinted tree, stages the
install, discovers `hdMerlin`, and launches usdview. `--build-dir` is reserved
for an already configured and built external CMake tree; OST installs and
inspects that tree but does not rebuild it or claim managed-build evidence.

To build the Hydra-enabled standalone viewport and open a USD stage:

```powershell
ost renderer viewport --intent viewport-usd --profile usd -- `
  --usd C:/path/to/scene.usd --frames 1 --hidden
```

The `viewport-usd` intent is declared in `openstrata.toml`; use a real `usd`
runtime when the scene needs OpenUSD and its dependencies.

See the [OpenStrata project layout](docs/design/openstrata-project.md) for the
composition mapping and adoption decisions.

## Build

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Render the headless smoke image:

```powershell
./build/adapters/merlin-headless/Debug/merlin-headless.exe --frames 6 --output merlin.ppm
```

Run the native Vulkan viewport. In a Hydra USD session, use `Open USD...` in
the diagnostics panel to browse for a `.usd`, `.usda`, `.usdc`, or `.usdz`
stage. The panel includes rolling host/GPU timings and, for Gaussian stages,
particle, spherical-harmonic, projection, sorting, culling, cache, and upload
diagnostics. Hydra USD navigation follows usdview: Alt+left tumbles, Alt+middle
tracks, Alt+right and the wheel dolly, and `F` frames the stage. Arrow keys
pan, an unmodified left click reads picking IDs, and `S` writes a screenshot.

Hydra-enabled Windows developer builds generate a launcher beside the
executable. It supplies the configured OpenUSD SDK runtime path without
changing the machine-wide `PATH`:

```powershell
./build/adapters/merlin-viewport/Debug/run-merlin-viewport.cmd --vsync off
```

The default native scene also exposes `Open USD...`, so a stage path does not
need to be supplied on the command line.

Retain unchanged-frame expected/actual/diff evidence as PNG and OpenEXR:

```powershell
./build/adapters/merlin-headless/Debug/merlin-headless.exe `
  --frames 6 --artifact-dir artifacts --output merlin.ppm
```

Capture the reference-path performance baselines as deterministic JSON:

```powershell
./build/adapters/merlin-benchmark/Debug/merlin-benchmark.exe `
  --fixture reference --width 512 --height 512 --steady-frames 30 `
  --output benchmark.json
```

The v3 report records build/machine metadata, CPU/GPU stage distributions,
hitches, AOV selection, transfer/allocation/descriptor work, and structural
counters for first-frame, steady-state, camera, per-aspect edits, and AOV
combinations. Fixed million-triangle, 10,000-mesh, 1,000-instance, and 4K
fixtures are selectable explicitly. See the
[benchmark guide](docs/guides/benchmarking.md) for the schema and comparison
rules.

The renderer keeps three frame contexts by default and returns tightly packed
top-left products under renderer-specific completion tokens. `RenderRequest`
selects produced AOVs and CPU readback; `Submit` records and queues work without
waiting, and timeout-aware `Resolve` transfers only the selected products. GPU geometry
residency is resource-granular: per-mesh vertex/index ranges are suballocated
from device-local arenas, staged through a persistently mapped upload ring,
keyed by handle generation and revision, shared across instances, and retired
deterministically after the last referencing frame completes. Static scenes
perform zero upload, allocation, and pipeline work after warm-up, and
transform-, visibility-, and material-only edits stage zero geometry bytes.
Revisioned textures and samplers are cached independently, while material
parameter edits reuse the existing shader/pipeline variant. On descriptor-
indexing-capable devices, finite global sampled-image and deduplicated-sampler
tables preserve unchanged slot identity, materialize the four reserved fallback
images, update only dirty descriptor elements, and delay slot reuse and Vulkan
object destruction until the last referencing completion. Conventional Forward
remains the correctness fallback; negotiated devices automatically use the
non-uniform-indexed bindless shader path with persistent per-frame material
descriptors, so warmed static frames perform zero descriptor allocation or
update.

## Hydra 2 adapter

The OpenUSD adapter is opt-in so Hydra never becomes a transitive dependency of
normal Core or native-backend builds. Point `CMAKE_PREFIX_PATH` at an OpenUSD
26.05 or 26.08 SDK:

```powershell
cmake -S . -B build-hydra2 -G "Visual Studio 17 2022" -A x64 `
  -DMERLIN_ENABLE_HYDRA2=ON `
  -DCMAKE_PREFIX_PATH=C:/path/to/openusd
cmake --build build-hydra2 --config Release
ctest --test-dir build-hydra2 -C Release --output-on-failure
```

The Hydra slice provides mesh topology/transform/visibility and camera sync,
an adapter-owned USD path to Merlin handle map, color/depth CPU render buffers,
and a Vulkan- or Metal-backed render pass. The test suite separately verifies plugin
discovery and delegate creation, RenderBuffer resize/map lifetime, and an
install-tree `testusdview` first frame with rendered geometry.

The install-tree regression also emits a versioned Hydra performance report
and raw OpenUSD Chrome trace. Together they separate delegate Sync, scene-index
processing, RenderWorld/extraction, Vulkan CPU/GPU work, selected readback,
RenderBuffer map/resolve, CPU-to-Hgi upload, host composite, and presentation;
camera-only motion is gated against geometry/topology/primvar fetch or upload.

The current mesh path normalizes indexed and face-varying normals, display
color/opacity, and UVs, robustly triangulates concave polygonal faces, preserves
authored material binding identity, and supports native Hydra instancing. The
adapter translates a basic `UsdPreviewSurface` subset (constant parameters,
diffuse image texture, wrap mode, opacity mask) and distant lights into the
same `MaterialIR` used by headless rendering. The optional graph-only
MaterialXGenSlang compiler and generated Vulkan Forward parameter/resource
execution are present. Hydra MaterialX ingestion remains later integration
work.
Subdivision refinement remains future work. usdview presentation keeps Hydra's
CPU RenderBuffer-to-Hgi upload as the universal reference and fallback, while
v0.13.0 can select an HgiVulkan color GPU-copy path on validated OpenUSD
packages and retains depth and ID AOVs on CPU readback. v0.13.1 released the
direct-path hardening boundary: OpenUSD 26.05/26.08 report
`public-texture-import-unavailable`, so GPU copy remains selected.

## Capability boundaries and roadmap

The current renderer intentionally does not yet provide:

- Hydra MaterialX loading or general graph coverage beyond the optional
  v0.10.0 compiler prototype and the existing `UsdPreviewSurface` subset;
- the complete GPU Scene tables, GPU-driven indexed submission, an opaque
  Visibility Buffer path, meshlet rendering, or a Mesh Shader backend;
- advanced viewport features such as alpha blending, dome lighting, shadows,
  selection, or production culling;
- Vulkan/Hgi direct-share and external-interop presentation. HgiVulkan GPU copy
  is available for color on validated packages, with Tier 0 CPU transfer as the
  fallback; broader sharing paths remain capability- and evidence-gated rather
  than implied by the Vulkan backend.

These are roadmap boundaries, not implicit compatibility claims. See the
[support matrix](docs/reference/support-matrix.md) for current platform and
feature coverage.

v0.5.0 released the host-neutral MaterialIR and basic textured shading slice.
v0.6.0 released the measurement foundation and incremental Hydra sync work,
making changed-scene costs and host presentation separately observable. v0.7.0
released the persistent Mesh/future-Gaussian resource foundation, and v0.8.0
moved the Forward shader source of truth to Slang with reflected artifacts and
a Metal compile gate. The completed v0.9.0 work adds the minimum backend-neutral
render contract and dedicated cross-backend `merlin-viewport` with validated
Vulkan swapchain presentation and Hydra USD loading. v0.10.0 proved a
MaterialXGenSlang material-function slice, and v0.11.0 released native Metal
offscreen execution, heap residency, argument-buffer tables, and the matching
backend-neutral AOV/readback contract. v0.12.0 released native Metal viewport
presentation with GPU-only drawable output, resize-safe pacing, and the matching
developer UI path. v0.13.0 adds HgiVulkan with safe GPU copy; optional direct
sharing is a separate evidence gate, and v0.14.0 brings the equivalent HgiMetal
bridge. v0.14.1 completes the CPU-sorted Gaussian MVP, then v0.15.0–v0.22.0
advance persistent resources, GPU projection/sorting, contribution-aware and
hierarchical tiling, temporal reuse, LOD/streaming, and Vulkan/Metal production
hardening. Forward and Tier 0 CPU readback remain reference fallbacks. See the [current
milestone](docs/roadmap/current.md), [ordered backlog](docs/roadmap/backlog.md),
[multi-backend shader and presentation
strategy](docs/design/multibackend-slang-materialx.md), [Hgi host presentation
policy](docs/design/hgi-host-presentation.md), [Gaussian rendering roadmap](docs/design/gaussian-rendering-roadmap.md),
and [GPU-driven rendering policy](docs/design/gpu-driven-rendering.md) for
scope, dependencies, and exit criteria.

Gaussian support consumes the standard Gaussian representation exposed by
OpenUSD through Hydra. hdMerlin does not define a renderer-specific USD schema
or directly parse PLY/SPLAT files; conversion from external formats belongs to
separate FileFormat plugins or importers. Mesh and Gaussian resources share the
persistent RenderWorld, camera, transforms, visibility, allocation, lifetime,
and profiling infrastructure while retaining separate rendering algorithms.

The Merlin-owned Vulkan path requires a Vulkan 1.4-capable graphics queue and
Slang 2026.8.x (`slangc`) from the Vulkan SDK at build time. The HgiVulkan
integration may instead borrow the host's Vulkan 1.3 device and graphics queue;
that path is limited to the conventional descriptor backend validated with
OpenUSD 26.05 and 26.08.

## MaterialX prototype

The v0.10.0 work adds an optional compiler boundary that turns a deliberate
MaterialX graph subset into a renderer-consumable Slang material function. It
uses the official MaterialXGenSlang implementation pinned at
`38368ee04da84ce1f8837ecba7322dd6d81291f8`. A compatible prebuilt MaterialX
1.39.6 package is preferred; development builds can use an existing source tree
or explicitly fetch the pin:

```powershell
cmake -S . -B build-materialx -G "Visual Studio 17 2022" -A x64 `
  -DMERLIN_ENABLE_VULKAN=OFF `
  -DMERLIN_ENABLE_MATERIALX=ON `
  -DMERLIN_FETCH_MATERIALX=ON
cmake --build build-materialx --config Debug
ctest --test-dir build-materialx -C Debug -R merlin-materialx `
  --output-on-failure
```

Source fallback builds require CMake 3.26 or newer. The generated module owns
only graph evaluation; geometry, lighting, alpha policy, render passes,
resources, and AOV writes remain renderer-owned.

The current foundation deterministically generates renderer-owned material
results for constants, image/UV0/world-normal, add/multiply/mix, and the minimum
Standard Surface `base`, `base_color`, `metalness`, `specular_roughness`, and
`normal` slice. Logical reflection, portable standard-library/include
fingerprints, and a topology-only module key stay separate from typed parameter
and resource state. Core carries the versioned logical module/layout contract
and exact input-space requirements; the same generated sources compile for
SPIR-V and Metal targets, keyed by the same Core target-artifact contract that
keys handwritten Slang. Registered parameter-only and texture/sampler artifacts
execute through a renderer-owned Forward fragment pipeline with ABI/reflection
checks, pipeline reuse across value and texture-content edits, structured
runtime fallback telemetry, and retained SPIR-V/Metal/reflection evidence. See
the authoritative
[MaterialXGenSlang material boundary](docs/design/materialxgenslang-boundary.md)
and [current milestone](docs/roadmap/current.md).

## Supported configurations

The host-neutral libraries require CMake 3.24 and a C++20 compiler. Vulkan and
Hydra are optional dependency layers:

| Configuration | CMake options | Required dependencies |
|---|---|---|
| Core-only | `MERLIN_ENABLE_VULKAN=OFF` | C++20 compiler |
| Native Metal | `MERLIN_ENABLE_VULKAN=OFF`, `MERLIN_ENABLE_METAL=ON` | macOS, Apple Metal framework, and a Metal-capable device |
| Headless Vulkan | `MERLIN_ENABLE_VULKAN=ON` | Vulkan 1.4 loader/headers/device and Slang 2026.8.x |
| Vulkan viewport | `MERLIN_BUILD_VIEWPORT=ON` | Vulkan requirements; GLFW 3.4 or the pinned fetched fallback; pinned Dear ImGui 1.92.8 |
| Hydra 2 | `MERLIN_ENABLE_HYDRA2=ON` | A native GPU backend, a compatible OpenUSD SDK, and pinned Native File Dialog Extended 1.3.0 for the viewport; Linux viewport builds additionally require D-Bus development files for the desktop portal |
| MaterialX compiler | `MERLIN_ENABLE_MATERIALX=ON` | MaterialX 1.39.6 with MaterialXGenSlang; CMake 3.26+ for source fallback |

Windows with Visual Studio 2022 and AppleClang on macOS are validated
development paths. Core-only Debug and Release builds run on hosted Windows and
Linux CI; Core plus Metal compiles and packages on hosted Apple Silicon macOS.
GPU and
Hydra tests remain capability jobs: missing validation/device capabilities are
reported as skips where the test contract allows it, and OpenUSD build
configuration and C++ runtime ABI must match the consumer.

The manually dispatched `Vulkan and Hydra capability CI` workflow has separate
headless and Hydra jobs. Both require only a self-hosted Windows x64 runner with
the `vulkan-1.4` GPU/driver label. They download and checksum-verify LunarG
Vulkan SDK 1.4.350.0 into a cached workspace prefix. Hydra also obtains the
Animusphere OpenUSD 26.05/cy2026 runtime from its digest-pinned public GHCR
package using pinned `ost` 0.21.0. No operator-managed SDK installation is
required. The jobs run the 64-frame validation loop and install-tree usdview
stable-update regression, retaining dependency/runtime provenance, images,
regression logs, and CTest logs as evidence artifacts.

## Install and consume

Install a configured build into a staging prefix:

```powershell
cmake --install build --config Release --prefix C:/merlin
```

This installs the public headers, libraries, versioned CMake package files,
and, when enabled, `merlin-headless`, `merlin-benchmark`, and `merlin-viewport` with their
versioned SPIR-V, Metal compile-gate, reflection, and manifest artifacts. A
downstream CMake project can consume the package without referring
to the Merlin source tree:

```cmake
find_package(Merlin 0.1 REQUIRED COMPONENTS RenderBackend)
target_link_libraries(my-renderer PRIVATE Merlin::RenderBackend)
```

Available package components and targets are `RenderWorld`
(`Merlin::RenderWorld`), `RenderExtraction` (`Merlin::RenderExtraction`),
`RenderBackend` (`Merlin::RenderBackend`), and, for Vulkan-enabled builds,
`Vulkan` (`Merlin::Vulkan`). Apple Metal-enabled builds export `Metal`
(`Merlin::Metal`). MaterialX-enabled builds also export `MaterialX`
(`Merlin::MaterialX`). The install-consumer
CTest installs to an isolated prefix and verifies downstream configure, build,
link, and execution.

## Architecture boundary

Dependencies flow from adapters into the host-neutral scene model, deterministic
draw extraction, backend-neutral execution contract, and concrete backend.
Public Core APIs do not expose OpenUSD, Hydra, Vulkan, Metal, GLFW, Qt, or DCC types.
Hydra owns host path/dirty-bit translation; the GLFW adapter owns the window;
the private Dear ImGui host surface owns development widgets; Vulkan owns its
execution, readback, surface, swapchain, overlay render pass, and
synchronization; and Metal independently owns native execution, heaps,
argument buffers, completion, and readback.

## Project documentation

- [Current milestone](docs/roadmap/current.md)
- [Roadmap backlog](docs/roadmap/backlog.md)
- [Delivery history](docs/reports/delivery-history.md)
- [Release records](docs/releases/README.md)
- [Renderer architecture](docs/design/renderer-architecture.md)
- [MaterialXGenSlang material boundary](docs/design/materialxgenslang-boundary.md)
- [Multi-backend shader and presentation strategy](docs/design/multibackend-slang-materialx.md)
- [GPU-driven rendering policy](docs/design/gpu-driven-rendering.md)
- [Execution and render-product lifetime](docs/design/execution-lifetime.md)
- [OpenStrata project layout](docs/design/openstrata-project.md)
- [OST dogfooding reports](docs/reports/ost/README.md)
- [Build and install](docs/guides/build-and-install.md)
- [Benchmarking](docs/guides/benchmarking.md)
- [Using the CMake package](docs/guides/cmake-package.md)
- [Releasing](docs/guides/releasing.md)
- [Support matrix](docs/reference/support-matrix.md)
- [Contributing](CONTRIBUTING.md)
- [Security policy](SECURITY.md)
- [Changelog](CHANGELOG.md)

## License

hdMerlin is licensed under the [Apache License 2.0](LICENSE).
