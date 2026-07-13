# hdMerlin

[![Core CI](https://github.com/animu-sphere/hydra-merlin/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/animu-sphere/hydra-merlin/actions/workflows/ci.yml)

hdMerlin is an OST-oriented, host-neutral Vulkan raster renderer. The current
implementation provides a handle-based `RenderWorld`, deterministic extraction
into an immutable resource-granular `FrameSnapshot`, and a persistent Vulkan
offscreen renderer with explicit submission/completion lifetime and selectable
color/depth/primId/instanceId CPU readback.

The core library intentionally has no OpenUSD, Hydra, DCC, Qt, or Vulkan types
in its public API. Hydra and host integrations will remain thin adapters around
that core.

## OpenStrata project

The repository is an OpenStrata renderer project targeting `cy2026`. With OST
0.16.0 or newer, the default host-neutral lifecycle is:

```powershell
ost runtime pull cy2026 --profile core
ost build
ost validate --json
```

The existing CMake targets remain project-owned renderer units; adopting OST
does not split them into artificial packages or plugin bundles. Vulkan builds
emit the renderer evidence consumed by `ost validate`. After configuring the
optional Hydra adapter against a real OpenUSD runtime, it can be opened through
`ost renderer view --build-dir <build-dir> --profile usd`.

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
  --width 512 --height 512 --steady-frames 30 --output benchmark.json
```

The report records build/machine metadata, CPU scope timings, and structural
counters for first-frame, steady-state, and per-aspect edit scenarios
(transform, visibility, material, points, removal). See the
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

The current mesh path normalizes indexed and face-varying normals, display
color/opacity, and UVs, robustly triangulates concave polygonal faces, preserves
authored material binding identity, and supports native Hydra instancing.
Material-network shading, subdivision refinement, and zero-copy Vulkan/Hgi
interop remain future work; usdview presentation currently uses Hydra's CPU
RenderBuffer-to-Hgi upload path.

## Capability boundaries and roadmap

The current renderer intentionally does not yet provide:

- MaterialX loading, graph translation, or authored Hydra material-network
  shading; authored binding identity is retained but currently uses the basic
  material path;
- advanced viewport features such as alpha mask/blend, dome lighting, shadows,
  selection, or production culling;
- Vulkan/Hgi external-memory or other zero-copy GPU presentation; Hydra uses
  CPU RenderBuffer readback followed by the host's Hgi upload path.

These are roadmap boundaries, not implicit compatibility claims. See the
[support matrix](docs/reference/support-matrix.md) for current platform and
feature coverage.

The active v0.4.1 milestone focuses on release integrity, host-neutral
diagnostics, compatibility checks, and durable GPU/Hydra validation. The ordered
path after that establishes MaterialIR and basic textured shading before adding
MaterialX translation, makes usdview performance observable before attempting
presentation interop, then adds viewport essentials. Tier 0 CPU readback remains
the correctness and fallback path throughout. See the
[current milestone](docs/roadmap/current.md) and
[ordered backlog](docs/roadmap/backlog.md) for scope and exit criteria.

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
package using pinned `ost` 0.16.0. No operator-managed SDK installation is
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
- [Execution and render-product lifetime](docs/design/execution-lifetime.md)
- [OpenStrata project layout](docs/design/openstrata-project.md)
- [OST v0.16 renderer-adoption dogfooding report](docs/reports/2026-07-13-v0.16.0-renderer-adoption-v0.17.0-asks.md)
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
