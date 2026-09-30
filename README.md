# Hydra Merlin

[![Core CI](https://github.com/animu-sphere/hydra-merlin/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/animu-sphere/hydra-merlin/actions/workflows/ci.yml)
[![License: Apache-2.0](https://img.shields.io/github/license/animu-sphere/hydra-merlin)](LICENSE)

hdMerlin is an OST-oriented, host-neutral raster renderer with independent
Vulkan and Metal backends. It provides a revisioned scene model, immutable
snapshots, explicit submission/completion, native viewports, and a Hydra 2
adapter. See the [support matrix](docs/reference/support-matrix.md) for current
validated configurations and feature limits.

The core library intentionally has no OpenUSD, Hydra, DCC, Qt, Vulkan, or Metal
types in its public API. Hydra and host integrations remain thin adapters
around that core.

## OpenStrata project

The repository is an OpenStrata renderer project targeting `cy2026`. See the
[support matrix](docs/reference/support-matrix.md) for the validated CLI version.
The default host-neutral lifecycle is:

```powershell
ost runtime pull cy2026 --profile core
ost build --check
ost build --jobs auto
ost validate --json
```

For Hydra inspection, materialize or adopt a compatible `usd`/`lookdev`
OpenUSD runtime, then run `ost renderer view --profile usd`. See the
[build and install guide](docs/guides/build-and-install.md) for runtime and
external build-directory workflows.

To build the Hydra-enabled standalone viewport and open a USD stage:

```powershell
ost renderer viewport --intent viewport-usd --profile usd -- `
  --usd C:/path/to/scene.usd --frames 1 --hidden
```

The `viewport-usd` intent is declared in `openstrata.toml`; use a real `usd`
runtime when the scene needs OpenUSD and its dependencies.

Omit `--frames` and `--hidden` to explore the stage interactively. For
Gaussian scenes, `--gaussian-gpu` selects GPU preparation, sorting, and raster
(`disabled`, `prefer`, `require`), and `--gaussian-raster` selects the
sorted-stream or compute tile raster (`sorted-stream`, `tiled`):

```powershell
ost renderer viewport --intent viewport-usd --profile usd -- `
  --usd C:/path/to/gaussians.usd --gaussian-gpu prefer --gaussian-raster tiled
```

Metal supports Gaussian splats through the CPU reference and opt-in GPU
projection, SH evaluation, radix sorting, indirect ellipse rasterization and
compute tile rasterization from a build-time compiled, embedded metallib.
Both GPU paths preserve opaque mesh depth and resource/particle picking IDs.
`--gaussian-gpu disabled` remains the default. `prefer` falls back when a path
is unavailable; `require` reports unsupported capabilities. A tiled frame that
exceeds its pair capacity keeps the complete GPU sorted-stream draw.

`prefer` falls back per stage when a GPU stage is unavailable, and the
developer UI's Gaussian counters show the selected path. `require` is rejected
when GPU Gaussian execution is unavailable.

In usdview, the **Hydra Settings** menu has **GPU Gaussian execution**
(`prefer` when checked) and **Gaussian tile raster** checkboxes. Other Hydra
hosts see the same two flags, `merlin:gpuDrivenGaussian:enabled` and
`merlin:gpuDrivenGaussian:tiled`, and can also set
`merlin:gpuDrivenGaussian:mode` and `merlin:gpuDrivenGaussian:raster` to the
names above, for example to select `require`. A rejected value is reported as
a `renderer-settings.*` warning and the previous policy stays applied.

See the [OpenStrata project layout](docs/design/openstrata-project.md) for the
composition mapping and adoption decisions.

## Build

Choose the native backend for your platform. Both build shaders with Slang
2026.8.x; dependency versions are pinned in
[`cmake/MerlinVersions.cmake`](cmake/MerlinVersions.cmake).

| Build | Required tools and SDKs | Products |
| --- | --- | --- |
| Windows / Vulkan | CMake 3.24+, Visual Studio 2022 C++ tools, Vulkan 1.4 SDK (validated: 1.4.350.0), Slang 2026.8.x | Native viewport, headless renderer, benchmark |
| macOS / Metal | CMake 3.24+, Ninja, Xcode with Metal tools, standalone Slang 2026.8.x | Native Metal viewport |
| Core only | CMake 3.24+, C++20 compiler and build system | Host-neutral libraries; no Slang, Vulkan, Metal or OpenUSD dependency |

The viewport fetches pinned GLFW (unless installed) and Dear ImGui sources;
Git and network access are needed on a fresh checkout. Hydra additionally
requires a compatible OpenUSD 26.05 or 26.08 SDK and fetches Native File Dialog
Extended. See the [dependency setup](docs/guides/build-and-install.md#prerequisites)
for build/runtime requirements and compiler discovery.

### Windows — Vulkan

Install the validated [LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home)
and check that its `slangc` reports 2026.8.x. Configure the existing `vulkan`
preset with Visual Studio 2022, x64 and the native viewport enabled:

```powershell
& "$env:VULKAN_SDK/Bin/slangc.exe" -version
cmake --preset vulkan -G "Visual Studio 17 2022" -A x64 -DMERLIN_BUILD_VIEWPORT=ON
cmake --build --preset vulkan --parallel
ctest --preset vulkan
./build/vulkan/adapters/merlin-viewport/Release/merlin-viewport.exe --backend vulkan
```

Render a headless smoke image or capture benchmark data:

```powershell
./build/vulkan/adapters/merlin-headless/Release/merlin-headless.exe `
  --frames 6 --artifact-dir artifacts --output merlin.ppm
./build/vulkan/adapters/merlin-benchmark/Release/merlin-benchmark.exe `
  --fixture reference --width 512 --height 512 --steady-frames 30 `
  --output benchmark.json
```

### macOS — Metal

Install [Xcode's Metal tools](https://developer.apple.com/metal/tools/) and
[Slang 2026.8](https://github.com/shader-slang/slang/releases/tag/v2026.8),
using the macOS archive for your CPU architecture. Put its `bin` directory on
`PATH`, or pass `-DMERLIN_SLANGC_EXECUTABLE=/path/to/slang/bin/slangc` when
configuring. Use the existing `metal` preset with Ninja and the viewport
enabled; this configuration does not require a Vulkan SDK.

```bash
slangc -version
xcrun -sdk macosx --find metal
xcrun -sdk macosx --find metallib
cmake --preset metal -G Ninja -DMERLIN_BUILD_VIEWPORT=ON
cmake --build --preset metal --parallel
ctest --preset metal
./build/metal/adapters/merlin-viewport/merlin-viewport --backend metal
```

The Gaussian metallib is compiled at build time and embedded in the backend.
Slang and the offline Metal tools are build dependencies, not application runtime
dependencies. Metal does not currently build `merlin-headless` or `merlin-benchmark`.

### Core only

```sh
cmake --preset core
cmake --build --preset core --parallel
ctest --preset core
```

All examples use Release. The native viewports provide camera navigation,
picking, AOV inspection, screenshots and diagnostics. USD stage loading and
`Open USD...` require the Hydra variants below. See the
[build and install guide](docs/guides/build-and-install.md) for Debug builds,
SDK-only presets and installation, and the
[benchmark guide](docs/guides/benchmarking.md) for fixtures and comparisons.

## Hydra 2 adapter

The OpenUSD adapter is opt-in so Hydra never becomes a transitive dependency of
Core or native-backend builds. Point `CMAKE_PREFIX_PATH` at an OpenUSD 26.05 or
26.08 SDK matching your compiler, architecture and Python runtime.

Windows / Vulkan:

```powershell
cmake --preset vulkan-hydra -G "Visual Studio 17 2022" -A x64 `
  -DMERLIN_BUILD_VIEWPORT=ON -DCMAKE_PREFIX_PATH=C:/path/to/openusd
cmake --build --preset vulkan-hydra --parallel
ctest --preset vulkan-hydra
./build/vulkan-hydra/adapters/merlin-viewport/Release/run-merlin-viewport.cmd `
  --usd C:/path/to/scene.usdc
```

macOS / Metal:

```bash
cmake --preset metal-hydra -G Ninja \
  -DMERLIN_BUILD_VIEWPORT=ON -DCMAKE_PREFIX_PATH=/path/to/openusd
cmake --build --preset metal-hydra --parallel
ctest --preset metal-hydra
DYLD_LIBRARY_PATH=/path/to/openusd/lib \
  ./build/metal-hydra/adapters/merlin-viewport/merlin-viewport \
  --backend metal --usd /path/to/scene.usdc
```

usdview also needs the SDK's compatible Python, Qt bindings and Python packages;
these are host dependencies, not Metal or Slang dependencies. See the
[Hydra setup](docs/guides/build-and-install.md#hydra-2) for plugin discovery and
runtime paths.

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

The Hydra adapter translates scene data and a basic `UsdPreviewSurface`
subset into renderer-neutral resources. See the
[support matrix](docs/reference/support-matrix.md) for validated Hydra,
MaterialX, and host-presentation paths.

## Capability boundaries and roadmap

For present capabilities and limitations, use the
[support matrix](docs/reference/support-matrix.md). The
[current milestone](docs/roadmap/current.md) and
[backlog](docs/roadmap/backlog.md) track incomplete work; the
[changelog](CHANGELOG.md) records shipped changes. Architecture and fallback
decisions are in the [renderer design](docs/design/renderer-architecture.md).

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

The optional compiler boundary turns a deliberate
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

See the [MaterialXGenSlang material boundary](docs/design/materialxgenslang-boundary.md)
for ownership and ABI rules and the
[support matrix](docs/reference/support-matrix.md) for validated coverage.

## Supported configurations

See the [support matrix](docs/reference/support-matrix.md) for validated
platforms, dependencies, feature availability, and evidence level. Optional
build configurations and SDK setup are documented in the
[build and install guide](docs/guides/build-and-install.md).

## Install and consume

Install a configured build into a staging prefix:

```powershell
cmake --build build --config Release
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
- [Code of Conduct](CODE_OF_CONDUCT.md)
- [Security policy](SECURITY.md)
- [Changelog](CHANGELOG.md)

## License

hdMerlin is licensed under the [Apache License 2.0](LICENSE).
