# hdMerlin

[![Core CI](https://github.com/animu-sphere/hydra-merlin/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/animu-sphere/hydra-merlin/actions/workflows/ci.yml)

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

The native viewport provides USD stage loading, camera navigation, picking,
AOV inspection, screenshots, and timing and resource diagnostics. See the
[build and install guide](docs/guides/build-and-install.md) for host setup.

The Hydra-enabled Windows build below generates a launcher beside the
executable. It supplies the configured OpenUSD SDK runtime path without
changing the machine-wide `PATH`:

```powershell
./build-hydra2/adapters/merlin-viewport/Release/run-merlin-viewport.cmd --vsync off
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

See the [benchmark guide](docs/guides/benchmarking.md) for fixtures, report
fields, and comparison rules, and the
[execution lifetime design](docs/design/execution-lifetime.md) for submission,
residency, and readback contracts.

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
