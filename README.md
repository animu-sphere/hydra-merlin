# hdMerlin

hdMerlin is an OST-oriented, host-neutral Vulkan raster renderer. The current
implementation provides a handle-based `RenderWorld`, deterministic draw
extraction, and a persistent Vulkan offscreen renderer with color/depth CPU
readback.

The core library intentionally has no OpenUSD, Hydra, DCC, Qt, or Vulkan types
in its public API. Hydra and host integrations will remain thin adapters around
that core.

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

The renderer keeps three frame contexts by default, uploads an extracted scene
only when its revision changes, and returns tightly packed top-left color and
depth products under one completion value.

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

The current mesh path fan-triangulates polygonal faces and uses the fallback
material. Hydra instancing, subdivision refinement, authored materials, and
zero-copy Vulkan/Hgi interop remain future work; usdview presentation currently
uses Hydra's CPU RenderBuffer-to-Hgi upload path.

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
package using pinned `ost` 0.13.0. No operator-managed SDK installation is
required. The jobs run the 64-frame validation loop and install-tree usdview
stable-update regression, retaining dependency/runtime provenance, images,
regression logs, and CTest logs as evidence artifacts.

## Install and consume

Install a configured build into a staging prefix:

```powershell
cmake --install build --config Release --prefix C:/merlin
```

This installs the public headers, libraries, versioned CMake package files, and,
when enabled, `merlin-headless` with its SPIR-V shaders. A downstream CMake
project can consume the package without referring to the Merlin source tree:

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
- [Release records](docs/releases/README.md)
- [Renderer architecture](docs/design/renderer-architecture.md)
