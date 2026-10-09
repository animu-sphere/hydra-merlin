# Build and install

hdMerlin can be built as portable Core libraries, with native Metal offscreen
execution, with the Vulkan headless and native viewport products, or with the
opt-in Hydra 2 adapter. Build only the layers whose dependencies are available.

## Prerequisites

Every configuration requires:

- CMake 3.24 or newer;
- a C++20 compiler;
- a build system supported by CMake.

### Dependency matrix

Build-time dependencies are tools and SDKs used to produce the binaries.
Runtime dependencies are needed by the resulting application or host.

| Configuration | Additional build dependencies | Runtime requirements |
| --- | --- | --- |
| Core only | None | Platform C++ runtime; no GPU SDK, Slang or OpenUSD |
| Windows / Vulkan | Visual Studio 2022 C++ tools, Vulkan 1.4 headers/loader, Slang 2026.8.x; validated SDK: 1.4.350.0 | Vulkan 1.4-capable GPU/driver and loader; packaged shader artifacts |
| Linux / Vulkan | C++20 compiler, Ninja, Vulkan development loader, SDK 1.4.350.1 tools/headers, Slang 2026.8 | Vulkan 1.4 loader and driver; Mesa 25+ lavapipe provides CPU execution; packaged shader artifacts |
| macOS / Metal | Xcode with macOS SDK and offline `metal`/`metallib` tools, Slang 2026.8.x; Ninja for the commands below | macOS Metal framework and a supported GPU; local runtime validation is on Apple Silicon |
| Hydra 2 on either backend | OpenUSD 26.05 or 26.08 shared-library SDK matching the compiler and architecture | Matching OpenUSD libraries and plugin resources |
| usdview host | The Hydra build above | The OpenUSD package's compatible Python, Qt bindings and Python dependencies, including PyOpenGL/NumPy |
| Optional MaterialX | MaterialX 1.39.6+ with `MaterialXGenSlang`, or the pinned source; CMake 3.26+ for a source build | The selected MaterialX package's shared libraries, if applicable |

Slang is a shader compiler shared by both GPU backends. It emits SPIR-V for
Vulkan and MSL for Metal; Apple's offline compiler turns the Gaussian MSL into
a metallib. A Metal-only build does not need Vulkan headers, loader or SDK.
Slang and the offline Metal compiler are not required to run built applications
or to consume the installed `Merlin::Metal` library. Installed `Merlin::Vulkan`
SDK consumers still need Vulkan development files for its public link dependency.

### Windows dependency setup

Install Visual Studio 2022 with C++ desktop tools, CMake, and the validated
[LunarG Vulkan SDK 1.4.350.0](https://vulkan.lunarg.com/sdk/home).
Open a new PowerShell after installation and check:

```powershell
cmake --version
$env:VULKAN_SDK
& "$env:VULKAN_SDK/Bin/slangc.exe" -version
```

`slangc` must report 2026.8.x. CMake finds it on `PATH` or under
`VULKAN_SDK/Bin` / `VULKAN_SDK/bin`. A standalone
[Slang 2026.8 release](https://github.com/shader-slang/slang/releases/tag/v2026.8)
can also be selected with
`-DMERLIN_SLANGC_EXECUTABLE=C:/path/to/slang/bin/slangc.exe`; this does not replace
the Vulkan SDK. Keep the compiler's accompanying libraries alongside it.
GPU execution additionally needs a compatible vendor driver; installing SDK
headers alone does not provide Vulkan 1.4 hardware support.

### Linux lavapipe validation

The Linux Vulkan workflow uses Ubuntu 26.04, Mesa lavapipe, pinned LunarG SDK
1.4.350.1 and standalone Slang 2026.8. The SDK supplies headers, SPIR-V tools and
validation layers; the distribution supplies the loader and software driver.
Mesa lavapipe gained Vulkan 1.4 in
[Mesa 25.0](https://docs.mesa3d.org/relnotes/25.0.0.html). Older distribution
packages can still expose only Vulkan 1.3; installing SDK headers cannot upgrade
the driver's API. Use `vulkaninfo --summary` to check the actual device.

From the repository root on Ubuntu 26.04:

```bash
sudo apt-get update
sudo apt-get install --no-install-recommends -y \
  build-essential cmake ninja-build curl libvulkan-dev mesa-vulkan-drivers \
  libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev xvfb xauth
source .github/scripts/setup-vulkan-linux.sh
vulkaninfo --summary
cmake --preset vulkan -B build-linux -G Ninja \
  -DMERLIN_SLANGC_EXECUTABLE="$PWD/.ci/linux-vulkan/slang-2026.8/bin/slangc" \
  -DCMAKE_BUILD_TYPE=Release -DMERLIN_BUILD_VIEWPORT=ON -DGLFW_BUILD_WAYLAND=OFF
cmake --build build-linux --parallel 4
mkdir -p build-linux/evidence
build-linux/adapters/merlin-headless/merlin-headless \
  --probe --validate --metadata build-linux/evidence/lavapipe.json
xvfb-run -a ctest --test-dir build-linux --output-on-failure \
  --no-tests=error --output-junit lavapipe-tests.xml
python3 scripts/check-linux-vulkan-evidence.py \
  --metadata build-linux/evidence/lavapipe.json --junit build-linux/lavapipe-tests.xml
```

The sourced setup verifies archive SHA-256 values, exports the SDK/compiler paths,
and selects exactly one system lavapipe ICD through `VK_DRIVER_FILES` and the
legacy `VK_ICD_FILENAMES`. Its `.ci` tools remain reusable by later build shells.
The evidence checker requires Mesa's `llvmpipe` Vulkan driver, API 1.4, validation,
and passing shader ABI, headless, Gaussian, lifetime/update/material, benchmark
contract, X11 viewport and install-consumer cases. A required test skip fails the
gate; optional capability skips remain visible. The workflow separately validates
every production SPIR-V artifact with `spirv-val --target-env vulkan1.4`.

This proves software-driver correctness and X11 presentation under Xvfb. Native
Wayland, Linux Hydra hosts, other drivers and hardware performance require
separate evidence. Software execution has no GPU timing threshold.

### macOS dependency setup

Install CMake, Ninja and Git, plus Xcode with the macOS SDK and
[Metal developer tools](https://developer.apple.com/metal/tools/).
Verify the selected toolchain:

```bash
cmake --version
ninja --version
xcode-select -p
xcrun -sdk macosx --find metal
xcrun -sdk macosx --find metallib
```

If either Metal tool is missing, complete the Metal toolchain installation for
that Xcode. Command Line Tools alone may not include the offline compiler.
For multiple Xcode installations, set `DEVELOPER_DIR` to the intended
`Xcode.app/Contents/Developer` directory in the build shell.

Download [Slang 2026.8](https://github.com/shader-slang/slang/releases/tag/v2026.8)
for macOS (`aarch64` for Apple Silicon), and extract the complete archive into
a persistent tools directory. Add its `bin` directory to `PATH`:

```bash
export PATH="/path/to/slang-2026.8/bin:$PATH"
slangc -version
```

Alternatively pass `-DMERLIN_SLANGC_EXECUTABLE=/path/to/slang-2026.8/bin/slangc`
to CMake. Use a persistent location rather than a temporary download directory,
because CMake retains the absolute compiler path in its cache. No Vulkan SDK
or MoltenVK installation is required for the Metal commands below.

### Viewport and optional host dependencies

The viewport uses GLFW 3.4. CMake first accepts an installed `glfw3` package;
otherwise it fetches the commit pinned in `cmake/MerlinVersions.cmake` and release
metadata. GLFW is private to the viewport and never becomes a Core dependency.
The viewport also fetches the pinned Dear ImGui 1.92.8 revision and compiles
only its core plus the required official GLFW, Vulkan, and Metal backends.
Dear ImGui stays private to the executable and does not enter an installed
Merlin target. Hydra-enabled viewport builds likewise embed Native File Dialog
Extended 1.3.0 for the platform USD chooser. Windows uses the system file
dialog, macOS uses AppKit, and Linux uses XDG Desktop Portal; Linux viewport
builds therefore need the D-Bus development package (for example,
`libdbus-1-dev` on Debian/Ubuntu).
For large Hydra2 stages opened through usdview, set
`MERLIN_METAL_HEAP_MIB=N` to raise the Metal scene heap from its 64 MiB default;
the native Merlin viewport exposes the equivalent `--metal-heap-mib N` option.

Metal Gaussian GPU execution is opt-in. The native viewport accepts
`--backend metal --gaussian-gpu require --gaussian-raster sorted-stream`;
select `--gaussian-raster tiled` for the Metal tile path. Use `prefer` to permit
the GPU sorted stream when tiled allocation is unavailable, or CPU fallback
when GPU execution is unavailable. Tile pair overflow also selects the GPU
sorted stream for that frame. Hydra exposes the same policy through
`merlin:gpuDrivenGaussian:mode` and `merlin:gpuDrivenGaussian:raster`.
The tiled path has offscreen image and overflow tests plus local usdview/HgiMetal
image and performance checks on Apple M3. Other devices remain unverified.
Metal `BackendOptions` has independent
`gaussian_residency_budget_bytes` and `gaussian_scratch_budget_bytes` limits
(1 GiB each by default), separate from the mesh/texture heap setting above.
Hydra hosts can override these with `MERLIN_METAL_GAUSSIAN_RESIDENCY_MIB` and
`MERLIN_METAL_GAUSSIAN_SCRATCH_MIB` (positive integer MiB). Residency includes
upload staging and versions retained by unfinished commands; a full first
upload therefore needs more than just the final attribute size.
The native viewport accepts `--metal-gaussian-residency-mib N` and
`--metal-gaussian-scratch-mib N` for the same limits.
For local Release measurements, run `merlin-metal-gaussian-tests --benchmark`:
it emits per-frame CSV for deterministic 64K/1M scenes, including static,
camera and localized-edit frames. Static GPU frames still recompute, so CPU
cached rendering can be faster; these measurements are not CI timing gates.

Windows builds are validated with Visual Studio 2022. Hosted Linux CI validates
Core-only Debug and Release builds with Ninja. Hosted Apple Silicon macOS CI
compiles and packages Core plus Metal in Debug and Release; local runtime
evidence exercises an Apple GPU. Metal Gaussian compute/image tests use the
backend availability probe, including scene-heap support, before GPU execution;
a non-null Metal device alone is insufficient. Hosted Metal jobs retain CTest
logs and JUnit results. Required hosted and SDK-release checks exclude the exact
`gpu`, `runtime`, and `host-smoke` labels. Shader/ABI checks and installed package
linking run without GPU execution. The scheduled/manual Metal capability
workflow runs Debug/Release runtime tests and retains skip reasons; a skipped
runtime test does not establish GPU coverage. Installed execution lives in
`merlin-metal-install-runtime` and `merlin-vulkan-install-runtime`, whose CTest
fixture first stages and builds the package consumers. An unfiltered local
CTest run continues to exercise both package linking and runtime. See the
[support matrix](../reference/support-matrix.md) for the exact coverage.

The separate `Hydra compilation CI` workflow builds the pinned Windows shared
OpenUSD 26.05/26.08 Release SDKs on hosted Windows 2022 runners. It checks CPU
adapter contracts, shader ABI, installed Core/Vulkan consumer linking and installed
Hydra plugin discovery with the same exact runtime-label exclusion above.
Its configure step disables post-link renderer execution with
`-DMERLIN_GENERATE_RENDERER_REPORT=OFF`. The evidence checker requires this
setting and rejects missing/skipped selected cases or accidental host/device
test selection. OpenUSD SDK provisioning uses verified OCI artifacts
and the SDK's Python 3.13 ABI, and omits `ost runtime validate`, which probes
devices and renders through a host. The capability workflow opts into that
validation separately. The copy-only Windows Vulkan SDK is supplemented with
the SHA-256-pinned upstream VMA 3.3.0 header; no prepared GPU runner is needed
for compilation. Hosted Hydra results remain pending until both SDK jobs run.

## Shared CMake presets

The checked-in presets work without OpenStrata. They select existing feature
options; dependency locations still come from the environment, `CMAKE_PREFIX_PATH`,
or explicit cache entries. OpenStrata-generated presets in `CMakeUserPresets.json`
can coexist with these presets.

```sh
cmake --preset core
cmake --build --preset core --parallel
ctest --preset core
```

`core`, `vulkan`, `vulkan-hydra`, `metal`, `metal-hydra`, and `materialx` select
the corresponding SDK layers. Metal presets are available only on macOS.
`developer` selects a Debug Vulkan viewport build; `ci` selects Release Core.
Other presets use Release. Supply `-G` at configure time to choose a generator.
The binary directory is `build/<preset-name>`.

SDK presets disable the viewport and MaterialX fetching. The MaterialX preset
requires an installed compatible package or `-DMERLIN_MATERIALX_SOURCE_DIR=...`;
fetching the pinned source requires explicit `-DMERLIN_FETCH_MATERIALX=ON`.
To include a native viewport, pass `-DMERLIN_BUILD_VIEWPORT=ON` with `vulkan`,
`metal`, or their `-hydra` variants. GLFW/ImGui/NFD use the fetching behavior
described above; a fresh build needs Git and network access. `developer`
already enables the Vulkan viewport.
To override the build configuration, use `-DCMAKE_BUILD_TYPE=Debug` for a
single-config generator and `--config Debug` / `-C Debug` for build / test.

## CMake maintenance

The top-level file assembles the target graph. `MerlinOptions.cmake` owns feature
defaults, `MerlinVersions.cmake` owns compatibility pins, and
`MerlinCompiler.cmake` applies private build-only policy through
`merlin_target_defaults()`. Call that helper for each new Merlin C++ target;
third-party targets must not use it. Public C++20 requirements remain on the
SDK targets.

Dependency discovery lives in `MerlinDependencies.cmake` and
`MerlinMaterialX.cmake`. The directory-scoped dependency macros preserve OpenUSD
import visibility and detected-version propagation. `MerlinShaders.cmake` owns
Slang artifact generation and installation, including the retained MaterialX
pipeline; `MerlinMetalShaders.cmake` owns the native Gaussian metallib.
`MerlinInstall.cmake` owns SDK exports, `MerlinPackaging.cmake` owns
release metadata and notices, and `MerlinOpenStrata.cmake` owns project-identity
validation and renderer report hooks. None requires the OpenStrata CLI/runtime.

Installed consumers can request `Core`, `RenderWorld`, `RenderExtraction`,
`RenderBackend`, `Vulkan`, `Metal`, or `MaterialX`. `Core` loads the three core
targets without discovering optional dependencies. Hydra remains a plugin,
not an exported SDK component. Existing component-free discovery retains its
optional backend discovery behavior; request `COMPONENTS Core` to avoid it.

## Core-only

Core has no Vulkan or OpenUSD dependency.

Use the existing `core` preset, which explicitly disables both GPU backends.
This also avoids the Apple default of enabling Metal when using raw CMake options.

Windows with Visual Studio 2022:

```powershell
cmake --preset core -G "Visual Studio 17 2022" -A x64
cmake --build --preset core --parallel
ctest --preset core
```

macOS or Linux with Ninja:

```bash
cmake --preset core -G Ninja
cmake --build --preset core --parallel
ctest --preset core
```

These commands use Release and `build/core`. For a separate Debug Ninja build,
use `cmake --preset core -B build/core-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug`,
then build and test that directory:

```sh
cmake --build build/core-debug
ctest --test-dir build/core-debug --output-on-failure
```

A `-B` override applies only to that invocation; the subsequent build/test
presets still refer to their default directory.

## GPU-free MaterialX generation gate

The hosted CI workflow adds a separate MaterialX generation matrix for Windows
and Linux in Debug and Release. It uses the source revision pinned in CMake,
without a Vulkan/Metal backend or Slang target compiler. It runs the Core tests,
MaterialX graph generation, material ABI and diagnostics, and an installed
`Merlin::MaterialX` consumer. Logs, JUnit results, and generated Slang sources
are uploaded as `materialx-generation-<os>-<configuration>` artifacts.

To reproduce the Linux configuration (CMake 3.26 or newer for the source build):

```bash
sudo apt-get update
sudo apt-get install --no-install-recommends -y libx11-dev libxt-dev
cmake -S . -B build-materialx -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DMERLIN_ENABLE_VULKAN=OFF -DMERLIN_ENABLE_METAL=OFF \
  -DMERLIN_ENABLE_MATERIALX=ON -DMERLIN_FETCH_MATERIALX=ON \
  -DMERLIN_MATERIALX_SLANGC_EXECUTABLE:FILEPATH=
cmake --build build-materialx --parallel 4
ctest --test-dir build-materialx --output-on-failure --no-tests=error \
  --output-junit materialx-tests.xml
```

The pinned MaterialX package configuration requires X11 and Xt development
files on Linux even with its render modules disabled. They are needed for
installed package discovery; this generation gate does not need a display
server or GPU.

On Windows, use `-G "Visual Studio 17 2022" -A x64` and pass `--config Debug`
to the build and `-C Debug` to CTest. The explicitly empty compiler cache entry
disables optional Slang target artifacts even when an SDK is installed. Omit
that argument in a fresh build tree to enable target compilation when `slangc`
is available. This gate provides no SPIR-V/Metal target or GPU runtime evidence.

## macOS — Metal backend and native viewport

On macOS, `MERLIN_ENABLE_METAL=ON` (the Apple-platform default) builds the
optional `Merlin::Metal` backend. It uses the system Metal and Foundation
frameworks. Building requires Slang 2026.8.x (`slangc` on `PATH`, or
`MERLIN_SLANGC_EXECUTABLE`) and Xcode's Metal tools (`xcrun -sdk macosx metal`
and `metallib`). Gaussian raster shaders compile from shared Slang to MSL and
then to an embedded metallib at build time; installed consumers need neither
compiler nor shader search paths. Mesh shaders still compile MSL during backend
creation. Core-only builds require neither shader compiler.

The Gaussian MSL, reflection, manifest, metallib, and library checksum install
under `bin/shaders/v2/metal`. The manifest uses the shared module/artifact identity
contract for Slang outputs; the separate checksum identifies the native library.
The Metal compilation defaults to macOS 14.0, honoring an explicit
`CMAKE_OSX_DEPLOYMENT_TARGET`.

```bash
cmake --preset metal -G Ninja -DMERLIN_BUILD_VIEWPORT=ON
cmake --build --preset metal --parallel
ctest --preset metal
./build/metal/adapters/merlin-viewport/merlin-viewport \
  --backend metal --vsync on
```

The backend renders Mesh snapshots offscreen with basic materials, RGBA
textures/samplers, directional light, opacity masks, depth, and
color/depth/`primId`/`instanceId` products. Metal argument-buffer tier 2 selects
the table path; other devices retain conventional Forward. CPU readback remains
the universal correctness path.

With `MERLIN_BUILD_VIEWPORT=ON` (explicitly enabled above), the configuration builds a
Metal-only `merlin-viewport`. Its Cocoa adapter owns the `CAMetalLayer` while
`Merlin::Metal` owns drawable acquisition, GPU presentation encoding, pacing,
and completion. Normal frames perform no CPU readback. `--vsync on|off`,
resize, screenshots, picking, benchmark capture, and the Dear ImGui diagnostic
surface use the same command-line and host behavior as the Vulkan viewport.
The current output policy is SDR sRGB by default, with Display P3 represented
in the Metal presentation contract and HDR kept as an explicit unsupported
future extension rather than inferred from a pixel format.

## Windows — Vulkan, headless rendering, and native viewport

When `MERLIN_ENABLE_VULKAN=ON` (the default), CMake locates Vulkan 1.4 and
Slang 2026.8.x, builds the Vulkan backend and versioned shader artifacts, and
builds `merlin-headless` and, by default, `merlin-viewport`.

```powershell
cmake --preset vulkan -G "Visual Studio 17 2022" -A x64 -DMERLIN_BUILD_VIEWPORT=ON
cmake --build --preset vulkan --parallel
ctest --preset vulkan
./build/vulkan/adapters/merlin-headless/Release/merlin-headless.exe `
  --frames 6 --output merlin.ppm
./build/vulkan/adapters/merlin-viewport/Release/merlin-viewport.exe `
  --backend vulkan --vsync off --benchmark viewport.json
```

The preset explicitly disables Metal and uses Release. For Debug with this
Visual Studio build, use `cmake --build --preset vulkan --config Debug` and
`ctest --preset vulkan -C Debug`; the executables then live under `Debug`.
For Metal/Ninja, use a separate directory with `-DCMAKE_BUILD_TYPE=Debug`
and build/test that directory, as in the Core example.

USD stages use usdview-compatible initial framing: the render/proxy bounds,
authored Y/Z `upAxis`, 60-degree vertical FOV, maximum bounds dimension, and a
1.1 frame-fit margin. `F` restores that framing. Alt+left drag tumbles,
Alt+middle (or Alt+Ctrl+left) tracks, Alt+right and the wheel dolly, arrow keys
pan, an unmodified left click triggers `primId` and `instanceId` readback, `S`
writes a PPM screenshot, and Escape closes the window. Normal frames present
through the Vulkan swapchain without CPU readback. The diagnostics panel can
apply a new clear color or opt into continuous color CPU readback; each request
shows its applied revision or rejection reason, and the readback toggle is
deliberately called out as a timing-affecting inspection mode. Use
`-DMERLIN_BUILD_VIEWPORT=OFF` for a Vulkan/headless-only build.

The Vulkan tests distinguish an unavailable optional device or validation
capability from a renderer failure. Review CTest output rather than treating a
skip as exercised GPU coverage.

## Hydra 2

Hydra is opt-in and requires either the Vulkan or Metal backend. Point
`CMAKE_PREFIX_PATH` at the OpenUSD install prefix containing its CMake package
and runtime layout.

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

For usdview, use the Python environment supplied or specified by the OpenUSD
package, including its Qt bindings, PyOpenGL and NumPy. `PYTHONPATH` must expose
`<openusd>/lib/python`; `PXR_PLUGINPATH_NAME` must expose the installed Merlin
`<prefix>/lib/usd/hdMerlin/resources` directory. On Windows, add the OpenUSD
`bin` and `lib` directories to the process `PATH`; on macOS, expose the SDK's
`lib` through `DYLD_LIBRARY_PATH` when the package needs it. Launch the SDK's
`usdview` with `--renderer Merlin /path/to/scene.usdc`. These host dependencies
are separate from `slangc` and the backend's SDK.

On Windows, `run-merlin-viewport.cmd` is generated beside the developer-build
executable and prepends the configured OpenUSD `bin` and `lib` directories for
that process only. This avoids a machine-wide `PATH` edit. The executable can
still be invoked directly when the same runtime directories are already
discoverable. On macOS and Linux, configure the platform loader path for the
selected OpenUSD SDK before launching the executable directly.

The Hydra configuration accepts the validated OpenUSD 26.05 and 26.08 shared
SDKs and records the selected header version in release metadata. It rejects
other versions/layouts. On MSVC, a Debug hdMerlin build is also rejected when
the SDK exports only Release libraries; use `--config Release` or provide a
matching Debug OpenUSD SDK. Compiler/toolset ABI compatibility still has to
match the consumer, and the discovery/usdview tests must run against the same
runtime root used at configure time. Metal viewports can raise the default
64 MiB scene heap for large stages with `--metal-heap-mib N`.

`MERLIN_ENABLE_HGI_VULKAN_BRIDGE` is offered only when both `MERLIN_ENABLE_HYDRA2`
and `MERLIN_ENABLE_VULKAN` are on, and defaults to `ON` there; a Metal-only
Hydra 2 build simply does not present it. It discovers the application-owned
Hgi driver at runtime and publishes an Hgi-owned target for the color AOV.
OpenUSD version support does not imply that a package ships a Vulkan Hgi
driver; missing and non-Vulkan drivers retain the CPU RenderBuffer path and
report the rejection. Disable the option to build only that universal
fallback.

`MERLIN_ENABLE_HGI_METAL_BRIDGE` is offered only when both `MERLIN_ENABLE_HYDRA2`
and `MERLIN_ENABLE_METAL` are on, and defaults to `ON` there. It publishes an
Hgi-owned Metal target, selects a same-device Metal-local copy when the native
HgiMetal target is available, and retains CPU readback for unsupported package
compositions or operational failures. HDR is rejected; SDR sRGB and Display P3
remain explicit target contracts.

## Optional MaterialX compiler

`MERLIN_ENABLE_MATERIALX=ON` builds the independent `Merlin::MaterialX`
material-function compiler. It does not require Vulkan and does not add a
MaterialX dependency to Core. A compatible MaterialX 1.39.6 package providing
`MaterialXGenSlang` is preferred. Development builds may point at a compatible
source tree or explicitly fetch the tested revision:

```powershell
cmake --preset materialx -G "Visual Studio 17 2022" -A x64 `
  -DMERLIN_FETCH_MATERIALX=ON
cmake --build --preset materialx --parallel
ctest --preset materialx -R merlin-materialx `
  --output-on-failure
```

Set `MERLIN_MATERIALX_SOURCE_DIR` instead of
`MERLIN_FETCH_MATERIALX=ON` to reuse an existing compatible source tree.
MaterialX source fallback builds require CMake 3.26 or newer. The build tests
graph-only generation unconditionally and registers SPIR-V and Metal-target
compile gates when the required `slangc` is available. Consult the
[support matrix](../reference/support-matrix.md) for current execution coverage
and the [MaterialX boundary](../design/materialxgenslang-boundary.md) for
ownership and ABI rules.

## Install

Install a configured build into an isolated prefix:

```powershell
cmake --install build/vulkan --config Release --prefix C:/merlin
```

For the macOS Metal build:

```bash
cmake --install build/metal --prefix /path/to/merlin-install
```

Use `build/vulkan-hydra` or `build/metal-hydra` to install the plugin as well.
Choose a writable prefix; these examples do not require a system-wide install.

Core headers, libraries, and versioned CMake package files are always installed.
Metal-enabled builds install `Merlin::Metal`, its public backend/resource-table
headers, an independent package export, and the Gaussian MSL/reflection,
manifest and metallib/checksum under `<prefix>/<bindir>/shaders/v2/metal`.
Vulkan-enabled builds also install the Vulkan library, `merlin-headless`,
`merlin-benchmark`, `merlin-viewport`, and
`<prefix>/<bindir>/shaders/v2` with SPIR-V, Metal compile-gate source,
reflection JSON, and the deterministic artifact manifest. MaterialX-enabled
builds also install the `Merlin::MaterialX` library, public compiler header, and
CMake component. When `slangc` is available, they also install generated
material and Standard Surface Slang, SPIR-V, Metal-target, and
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
| `MERLIN_ENABLE_HGI_VULKAN_BRIDGE` | `ON` when Hydra 2 and Vulkan are both on, otherwise unavailable | Build HgiVulkan driver discovery and the Hgi-owned color presentation target. |
| `MERLIN_ENABLE_HGI_METAL_BRIDGE` | `ON` when Hydra 2 and Metal are both on, otherwise unavailable | Build HgiMetal driver discovery, Metal-local color copy, and the Hgi-owned presentation target. |
| `MERLIN_ENABLE_MATERIALX` | `OFF` | Build the optional `Merlin::MaterialX` graph-only compiler. |
| `MERLIN_FETCH_MATERIALX` | `OFF` | Fetch the pinned MaterialX source when no compatible package/source tree is supplied. |
| `MERLIN_MATERIALX_SOURCE_DIR` | empty | Use an existing compatible MaterialX source tree. |
| `MERLIN_BUILD_VIEWPORT` | `ON` in raw CMake; `OFF` in SDK presets | Build the GLFW-hosted native viewport; requires Vulkan or Metal. |
| `MERLIN_SLANGC_EXECUTABLE` | discovered | Select Slang 2026.8.x for Vulkan/Metal shader compilation. |
| `CMAKE_PREFIX_PATH` | environment/cache | Locate optional OpenUSD, GLFW or MaterialX SDK packages. |
| `CMAKE_OSX_DEPLOYMENT_TARGET` | Metal shader default: `14.0` | Set the macOS deployment target. |

Use a new build directory when changing dependency roots or major capability
options. Existing CMake caches can otherwise retain an older Vulkan or OpenUSD
installation.
