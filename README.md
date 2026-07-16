# hdMerlin

[![Core CI](https://github.com/animu-sphere/hydra-merlin/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/animu-sphere/hydra-merlin/actions/workflows/ci.yml)

hdMerlin is an OST-oriented, host-neutral Vulkan raster renderer. The current
implementation provides a handle-based `RenderWorld`, deterministic extraction
into an immutable resource-granular `FrameSnapshot`, and a persistent Vulkan
offscreen renderer with explicit submission/completion lifetime and selectable
color/depth/primId/instanceId CPU readback. Its host-neutral `MaterialIR`
supports revisioned texture/sampler bindings and basic directional-lit,
textured, vertex-colored, opaque or alpha-masked shading.

The core library intentionally has no OpenUSD, Hydra, DCC, Qt, Vulkan, or Metal
types in its public API. Hydra and host integrations remain thin adapters
around that core.

## OpenStrata project

The repository is an OpenStrata renderer project targeting `cy2026`. OST 0.17.0
or newer is required for atomic build evidence and the managed Hydra view loop.
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
the normal Core/Vulkan build. Point `CMAKE_PREFIX_PATH` at an OpenUSD 26.05 SDK:

```powershell
cmake -S . -B build-hydra2 -G "Visual Studio 17 2022" -A x64 `
  -DMERLIN_ENABLE_HYDRA2=ON `
  -DCMAKE_PREFIX_PATH=C:/path/to/openusd
cmake --build build-hydra2 --config Release
ctest --test-dir build-hydra2 -C Release --output-on-failure
```

The Hydra slice provides mesh topology/transform/visibility and camera sync,
an adapter-owned USD path to Merlin handle map, color/depth CPU render buffers,
and a Vulkan-backed render pass. The test suite separately verifies plugin
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
same `MaterialIR` used by headless rendering. MaterialX translation,
subdivision refinement, and zero-copy Vulkan/Hgi interop remain future work;
usdview presentation currently uses Hydra's CPU RenderBuffer-to-Hgi upload
path.

## Capability boundaries and roadmap

The current renderer intentionally does not yet provide:

- MaterialX loading or general graph translation beyond the basic
  `UsdPreviewSurface` subset;
- the complete GPU Scene tables, GPU-driven indexed submission, an opaque
  Visibility Buffer path, meshlet rendering, or a Mesh Shader backend;
- advanced viewport features such as alpha blending, dome lighting, shadows,
  selection, or production culling;
- Vulkan/Hgi external-memory or other zero-copy GPU presentation; Hydra uses
  CPU RenderBuffer readback followed by the host's Hgi upload path.

These are roadmap boundaries, not implicit compatibility claims. See the
[support matrix](docs/reference/support-matrix.md) for current platform and
feature coverage.

v0.5.0 released the host-neutral MaterialIR and basic textured shading slice.
v0.6.0 released the measurement foundation and incremental Hydra sync work,
making changed-scene costs and host presentation separately observable. v0.7.0
released the persistent Mesh/future-Gaussian resource foundation. Before the
Gaussian and GPU-driven shader families expand, the active v0.8.0 milestone
moves the shader source of truth from GLSL to Slang while preserving Vulkan
output and enforcing a Metal compile gate. The ordered ladder then extracts the
minimum backend-neutral render contract and dedicated cross-backend
`merlin-viewport`, proves a MaterialXGenSlang material-function slice, and
brings up native Metal plus an HgiMetal host presentation bridge. The later path
advances through Gaussian rendering, persistent draw identity, GPU-driven
Mesh/Gaussian execution, experimental opaque Visibility, production MaterialX
quality, static meshlets, and only then optional Mesh Shader/Hi-Z/LOD. Forward
and Tier 0 CPU readback remain reference fallbacks. See the [current
milestone](docs/roadmap/current.md), [ordered backlog](docs/roadmap/backlog.md),
[multi-backend shader and presentation
strategy](docs/design/multibackend-slang-materialx.md), and [GPU-driven
rendering policy](docs/design/gpu-driven-rendering.md) for scope, dependencies,
and exit criteria.

Gaussian support will consume the standard Gaussian representation exposed by
OpenUSD through Hydra. hdMerlin will not define a renderer-specific USD schema
or directly parse PLY/SPLAT files; conversion from external formats belongs to
separate FileFormat plugins or importers. Mesh and Gaussian resources share the
persistent RenderWorld, camera, transforms, visibility, allocation, lifetime,
and profiling infrastructure while retaining separate rendering algorithms.

The Vulkan path requires a Vulkan 1.4-capable graphics queue and `glslc`
from the Vulkan SDK at build time.

## Supported configurations

The host-neutral libraries require CMake 3.24 and a C++20 compiler. Vulkan and
Hydra are optional dependency layers:

| Configuration | CMake options | Required dependencies |
|---|---|---|
| Core-only | `MERLIN_ENABLE_VULKAN=OFF` | C++20 compiler |
| Headless Vulkan | `MERLIN_ENABLE_VULKAN=ON` | Vulkan 1.4 loader/headers/device and `glslc` |
| Hydra 2 | `MERLIN_ENABLE_HYDRA2=ON` | Vulkan requirements and a compatible OpenUSD SDK |

Windows with Visual Studio 2022 is the currently validated development path.
Core-only Debug and Release builds run on hosted Windows and Linux CI. GPU and
Hydra tests remain capability jobs: missing validation/device capabilities are
reported as skips where the test contract allows it, and OpenUSD build
configuration and C++ runtime ABI must match the consumer.

The manually dispatched `Vulkan and Hydra capability CI` workflow has separate
headless and Hydra jobs. Both require only a self-hosted Windows x64 runner with
the `vulkan-1.4` GPU/driver label. They download and checksum-verify LunarG
Vulkan SDK 1.4.350.0 into a cached workspace prefix. Hydra also obtains the
Animusphere OpenUSD 26.05/cy2026 runtime from its digest-pinned public GHCR
package using pinned `ost` 0.17.0. No operator-managed SDK installation is
required. The jobs run the 64-frame validation loop and install-tree usdview
stable-update regression, retaining dependency/runtime provenance, images,
regression logs, and CTest logs as evidence artifacts.

## Install and consume

Install a configured build into a staging prefix:

```powershell
cmake --install build --config Release --prefix C:/merlin
```

This installs the public headers, libraries, versioned CMake package files,
and, when enabled, `merlin-headless` and `merlin-benchmark` with their SPIR-V
shaders. A downstream CMake project can consume the package without referring
to the Merlin source tree:

```cmake
find_package(Merlin 0.1 REQUIRED COMPONENTS RenderExtraction)
target_link_libraries(my-renderer PRIVATE Merlin::RenderExtraction)
```

Available package components and targets are `RenderWorld`
(`Merlin::RenderWorld`), `RenderExtraction` (`Merlin::RenderExtraction`), and,
for Vulkan-enabled builds, `Vulkan` (`Merlin::Vulkan`). The install-consumer
CTest installs to an isolated prefix and verifies downstream configure, build,
link, and execution.

## Architecture boundary

Dependencies flow from adapters into the host-neutral scene model, deterministic
draw extraction, and then the offscreen backend. Public Core APIs do not expose
OpenUSD, Hydra, Vulkan, Qt, or DCC types. Hydra owns host path/dirty-bit
translation, while the Vulkan backend owns execution and CPU render-product
readback without owning a native window or swapchain.

## Project documentation

- [Current milestone](docs/roadmap/current.md)
- [Roadmap backlog](docs/roadmap/backlog.md)
- [Delivery history](docs/reports/delivery-history.md)
- [Release records](docs/releases/README.md)
- [Renderer architecture](docs/design/renderer-architecture.md)
- [GPU-driven rendering policy](docs/design/gpu-driven-rendering.md)
- [Execution and render-product lifetime](docs/design/execution-lifetime.md)
- [OpenStrata project layout](docs/design/openstrata-project.md)
- [OST v0.17 dogfooding and v0.18 asks](docs/reports/2026-07-15-v0.17.0-dogfooding-v0.18.0-asks.md)
- [Historical OST v0.16 renderer-adoption dogfooding report](docs/reports/2026-07-13-v0.16.0-renderer-adoption-v0.17.0-asks.md)
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
