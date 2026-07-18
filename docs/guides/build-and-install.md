# Build and install

hdMerlin can be built as portable Core libraries, with the Vulkan headless and
native viewport products, or with the opt-in Hydra 2 adapter. Build only the layers whose
dependencies are available.

## Prerequisites

Every configuration requires:

- CMake 3.24 or newer;
- a C++20 compiler;
- a build system supported by CMake.

The Vulkan/headless configuration additionally requires Vulkan 1.4 headers and
loader, a Vulkan 1.4 physical device with a graphics queue, and the pinned
Slang 2026.8.x `slangc` shipped by Vulkan SDK 1.4.350.0. The Hydra configuration
also requires a compatible OpenUSD SDK;
OpenUSD 26.05 is the currently validated version.

The viewport uses GLFW 3.4. CMake first accepts an installed `glfw3` package;
otherwise it fetches the commit pinned in the top-level build and release
metadata. GLFW is private to the viewport and never becomes a Core dependency.

Windows builds are validated with Visual Studio 2022. Hosted Linux CI validates
Core-only Debug and Release builds with Ninja. See the
[support matrix](../reference/support-matrix.md) for the exact coverage.

## Core-only

Core has no Vulkan or OpenUSD dependency.

Windows with Visual Studio 2022:

```powershell
cmake -S . -B build-core -G "Visual Studio 17 2022" -A x64 `
  -DMERLIN_ENABLE_VULKAN=OFF
cmake --build build-core --config Debug --parallel
ctest --test-dir build-core -C Debug --output-on-failure
```

Linux with Ninja:

```bash
cmake -S . -B build-core -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DMERLIN_ENABLE_VULKAN=OFF
cmake --build build-core --parallel
ctest --test-dir build-core --output-on-failure
```

Use `Release` in place of `Debug` to verify the release configuration.

## Vulkan, headless rendering, and native viewport

When `MERLIN_ENABLE_VULKAN=ON` (the default), CMake locates Vulkan 1.4 and
Slang 2026.8.x, builds the Vulkan backend and versioned shader artifacts, and
builds `merlin-headless` and, by default, `merlin-viewport`.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DMERLIN_ENABLE_VULKAN=ON
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
./build/adapters/merlin-headless/Debug/merlin-headless.exe `
  --frames 6 --output merlin.ppm
./build/adapters/merlin-viewport/Debug/merlin-viewport.exe `
  --vsync off --benchmark viewport.json
```

USD stages use usdview-compatible initial framing: the render/proxy bounds,
authored Y/Z `upAxis`, 60-degree vertical FOV, maximum bounds dimension, and a
1.1 frame-fit margin. `F` restores that framing. Alt+left drag tumbles,
Alt+middle (or Alt+Ctrl+left) tracks, Alt+right and the wheel dolly, arrow keys
pan, an unmodified left click triggers `primId` and `instanceId` readback, `S`
writes a PPM screenshot, and Escape closes the window. Normal frames present
through the Vulkan swapchain without CPU readback. Use
`-DMERLIN_BUILD_VIEWPORT=OFF` for a Vulkan/headless-only build.

The Vulkan tests distinguish an unavailable optional device or validation
capability from a renderer failure. Review CTest output rather than treating a
skip as exercised GPU coverage.

## Hydra 2

Hydra is opt-in and requires Vulkan. Point `CMAKE_PREFIX_PATH` at the OpenUSD
install prefix containing its CMake package and runtime layout.

```powershell
cmake -S . -B build-hydra2 -G "Visual Studio 17 2022" -A x64 `
  -DMERLIN_ENABLE_HYDRA2=ON `
  -DCMAKE_PREFIX_PATH=C:/path/to/openusd
cmake --build build-hydra2 --config Release --parallel
ctest --test-dir build-hydra2 -C Release --output-on-failure
./build-hydra2/adapters/merlin-viewport/Release/merlin-viewport.exe `
  --usd C:/path/to/scene.usda
```

The Hydra configuration accepts the validated OpenUSD 26.05 shared SDK and
records its detected header version in release metadata. It rejects other
versions/layouts. On MSVC, a Debug hdMerlin build is also rejected when the SDK
exports only Release libraries; use `--config Release` or provide a matching
Debug OpenUSD SDK. Compiler/toolset ABI compatibility still has to match the
consumer, and the discovery/usdview tests must run against the same runtime
root used at configure time.

## Optional MaterialX compiler

`MERLIN_ENABLE_MATERIALX=ON` builds the independent `Merlin::MaterialX`
material-function compiler. It does not require Vulkan and does not add a
MaterialX dependency to Core. A compatible MaterialX 1.39.6 package providing
`MaterialXGenSlang` is preferred. Development builds may point at a compatible
source tree or explicitly fetch the tested revision:

```powershell
cmake -S . -B build-materialx -G "Visual Studio 17 2022" -A x64 `
  -DMERLIN_ENABLE_VULKAN=OFF `
  -DMERLIN_ENABLE_MATERIALX=ON `
  -DMERLIN_FETCH_MATERIALX=ON
cmake --build build-materialx --config Debug --parallel
ctest --test-dir build-materialx -C Debug -R merlin-materialx `
  --output-on-failure
```

Set `MERLIN_MATERIALX_SOURCE_DIR` instead of
`MERLIN_FETCH_MATERIALX=ON` to reuse an existing compatible source tree.
MaterialX source fallback builds require CMake 3.26 or newer. The build tests
graph-only generation unconditionally and registers SPIR-V and Metal-target
compile gates when the required `slangc` is available. Generated modules are
not yet executed by Vulkan Forward; current and planned coverage is recorded in
the [support matrix](../reference/support-matrix.md) and
[v0.10.0 boundary](../design/materialxgenslang-boundary.md).

## Install

Install a configured build into an isolated prefix:

```powershell
cmake --install build --config Release --prefix C:/merlin
```

Core headers, libraries, and versioned CMake package files are always installed.
Vulkan-enabled builds also install the Vulkan library, `merlin-headless`,
`merlin-benchmark`, `merlin-viewport`, and
`<prefix>/<bindir>/shaders/v1` with SPIR-V, Metal compile-gate source,
reflection JSON, and the deterministic artifact manifest. MaterialX-enabled
builds also install the `Merlin::MaterialX` library, public compiler header, and
CMake component. Hydra-enabled builds install the `hdMerlin` plugin below
`<prefix>/<libdir>/usd/hdMerlin` and its smoke fixture below
`<prefix>/<datadir>/merlin/tests`. Every configuration also installs
`<prefix>/<datadir>/merlin/VERSION` plus
`merlin-release-metadata.json` with dependency, feature, exported-target, and
runtime-product information.

See [Using the CMake package](cmake-package.md) for downstream integration.
Maintainers should also see [Releasing](releasing.md) for the tag-driven release
contract.

## Useful options

| Option | Default | Purpose |
| --- | --- | --- |
| `MERLIN_BUILD_TESTS` | `ON` | Build and register the test suite. |
| `MERLIN_ENABLE_VULKAN` | `ON` | Build Vulkan, shaders, and headless products. |
| `MERLIN_ENABLE_HYDRA2` | `OFF` | Build the OpenUSD Hydra 2 adapter; requires Vulkan. |
| `MERLIN_ENABLE_MATERIALX` | `OFF` | Build the optional `Merlin::MaterialX` graph-only compiler. |
| `MERLIN_FETCH_MATERIALX` | `OFF` | Fetch the pinned MaterialX source when no compatible package/source tree is supplied. |
| `MERLIN_MATERIALX_SOURCE_DIR` | empty | Use an existing compatible MaterialX source tree. |
| `MERLIN_BUILD_VIEWPORT` | `ON` | Build the GLFW-hosted native viewport; requires Vulkan. |

Use a new build directory when changing dependency roots or major capability
options. Existing CMake caches can otherwise retain an older Vulkan or OpenUSD
installation.
