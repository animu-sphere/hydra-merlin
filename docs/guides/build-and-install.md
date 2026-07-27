# Build and install

hdMerlin can be built as portable Core libraries, with native Metal offscreen
execution, with the Vulkan headless and native viewport products, or with the
opt-in Hydra 2 adapter. Build only the layers whose dependencies are available.

## Prerequisites

Every configuration requires:

- CMake 3.24 or newer;
- a C++20 compiler;
- a build system supported by CMake.

The Vulkan/headless configuration additionally requires Vulkan 1.4 headers and
loader, a Vulkan 1.4 physical device with a graphics queue, and the pinned
Slang 2026.8.x `slangc` shipped by Vulkan SDK 1.4.350.0. The Hydra configuration
also requires a compatible OpenUSD SDK; OpenUSD 26.05 and 26.08 are currently
validated.

The viewport uses GLFW 3.4. CMake first accepts an installed `glfw3` package;
otherwise it fetches the commit pinned in the top-level build and release
metadata. GLFW is private to the viewport and never becomes a Core dependency.
The viewport also fetches the pinned Dear ImGui 1.92.8 revision and compiles
only its core plus the required official GLFW, Vulkan, and Metal backends.
Dear ImGui stays private to the executable and does not enter an installed
Merlin target.

Windows builds are validated with Visual Studio 2022. Hosted Linux CI validates
Core-only Debug and Release builds with Ninja. Hosted Apple Silicon macOS CI
compiles and packages Core plus Metal in Debug and Release; local runtime
evidence exercises an Apple GPU. See the
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

## Native Metal backend and viewport

On macOS, `MERLIN_ENABLE_METAL=ON` (the Apple-platform default) builds the
optional `Merlin::Metal` backend. It uses the system Metal and Foundation
frameworks and compiles its renderer-owned MSL at runtime, so the standalone
Metal offline compiler is not required.

```bash
cmake -S . -B build-metal -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DMERLIN_ENABLE_VULKAN=OFF \
  -DMERLIN_ENABLE_METAL=ON
cmake --build build-metal --parallel
ctest --test-dir build-metal --output-on-failure
./build-metal/adapters/merlin-viewport/merlin-viewport \
  --backend metal --vsync on
```

The backend renders Mesh snapshots offscreen with basic materials, RGBA
textures/samplers, directional light, opacity masks, depth, and
color/depth/`primId`/`instanceId` products. Metal argument-buffer tier 2 selects
the table path; other devices retain conventional Forward. CPU readback remains
the universal correctness path.

With `MERLIN_BUILD_VIEWPORT=ON` (the default), the same configuration builds a
Metal-only `merlin-viewport`. Its Cocoa adapter owns the `CAMetalLayer` while
`Merlin::Metal` owns drawable acquisition, GPU presentation encoding, pacing,
and completion. Normal frames perform no CPU readback. `--vsync on|off`,
resize, screenshots, picking, benchmark capture, and the Dear ImGui diagnostic
surface use the same command-line and host behavior as the Vulkan viewport.
The current output policy is SDR sRGB by default, with Display P3 represented
in the Metal presentation contract and HDR kept as an explicit unsupported
future extension rather than inferred from a pixel format.

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

Hydra is opt-in and requires either the Vulkan or Metal backend. Point
`CMAKE_PREFIX_PATH` at the OpenUSD install prefix containing its CMake package
and runtime layout.

```powershell
cmake -S . -B build-hydra2 -G "Visual Studio 17 2022" -A x64 `
  -DMERLIN_ENABLE_HYDRA2=ON `
  -DCMAKE_PREFIX_PATH=C:/path/to/openusd
cmake --build build-hydra2 --config Release --parallel
ctest --test-dir build-hydra2 -C Release --output-on-failure
./build-hydra2/adapters/merlin-viewport/Release/merlin-viewport.exe `
  --usd C:/path/to/scene.usda
```

The Hydra configuration accepts the validated OpenUSD 26.05 and 26.08 shared
SDKs and records the selected header version in release metadata. It rejects
other versions/layouts. On MSVC, a Debug hdMerlin build is also rejected when
the SDK exports only Release libraries; use `--config Release` or provide a
matching Debug OpenUSD SDK. Compiler/toolset ABI compatibility still has to
match the consumer, and the discovery/usdview tests must run against the same
runtime root used at configure time. Metal viewports can raise the default
64 MiB scene heap for large stages with `--metal-heap-mib N`.

`MERLIN_ENABLE_HGI_VULKAN_BRIDGE` follows `MERLIN_ENABLE_HYDRA2` by default
and requires both Hydra 2 and Vulkan. It discovers the application-owned Hgi
driver at runtime. OpenUSD version support does not imply that a package ships
a Vulkan Hgi driver; missing and non-Vulkan drivers retain the CPU RenderBuffer
path and report the rejection. Disable the option to build only that universal
fallback.

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
Metal-enabled builds install `Merlin::Metal`, its public backend/resource-table
headers, and an independent package export.
Vulkan-enabled builds also install the Vulkan library, `merlin-headless`,
`merlin-benchmark`, `merlin-viewport`, and
`<prefix>/<bindir>/shaders/v2` with SPIR-V, Metal compile-gate source,
reflection JSON, and the deterministic artifact manifest. MaterialX-enabled
builds also install the `Merlin::MaterialX` library, public compiler header, and
CMake component. When `slangc` is available, they also install the retained
v0.10.0 prototype and Standard Surface Slang, SPIR-V, Metal-target, and
reflection evidence below
`<prefix>/<datadir>/merlin/shaders/v2/materialx`. Hydra-enabled builds install
the `hdMerlin` plugin below
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
| `MERLIN_ENABLE_METAL` | `ON` on Apple, otherwise `OFF` | Build the optional native Metal offscreen backend. |
| `MERLIN_ENABLE_HYDRA2` | `OFF` | Build the OpenUSD Hydra 2 adapter; requires Vulkan or Metal. |
| `MERLIN_ENABLE_HGI_VULKAN_BRIDGE` | follows `MERLIN_ENABLE_HYDRA2` | Build HgiVulkan driver discovery and Hgi-owned presentation targets; requires Hydra 2 and Vulkan. |
| `MERLIN_ENABLE_MATERIALX` | `OFF` | Build the optional `Merlin::MaterialX` graph-only compiler. |
| `MERLIN_FETCH_MATERIALX` | `OFF` | Fetch the pinned MaterialX source when no compatible package/source tree is supplied. |
| `MERLIN_MATERIALX_SOURCE_DIR` | empty | Use an existing compatible MaterialX source tree. |
| `MERLIN_BUILD_VIEWPORT` | `ON` | Build the GLFW-hosted native viewport; requires Vulkan. |

Use a new build directory when changing dependency roots or major capability
options. Existing CMake caches can otherwise retain an older Vulkan or OpenUSD
installation.
