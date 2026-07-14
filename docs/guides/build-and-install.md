# Build and install

hdMerlin can be built as portable Core libraries, with the Vulkan/headless
backend, or with the opt-in Hydra 2 adapter. Build only the layers whose
dependencies are available.

## Prerequisites

Every configuration requires:

- CMake 3.24 or newer;
- a C++20 compiler;
- a build system supported by CMake.

The Vulkan/headless configuration additionally requires Vulkan 1.4 headers and
loader, a Vulkan 1.4 physical device with a graphics queue, and `glslc` from the
Vulkan SDK. The Hydra configuration also requires a compatible OpenUSD SDK;
OpenUSD 26.05 is the currently validated version.

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

## Vulkan and headless rendering

When `MERLIN_ENABLE_VULKAN=ON` (the default), CMake locates Vulkan 1.4 and
`glslc`, builds the Vulkan backend and shaders, and builds `merlin-headless`.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DMERLIN_ENABLE_VULKAN=ON
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
./build/adapters/merlin-headless/Debug/merlin-headless.exe `
  --frames 6 --output merlin.ppm
```

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
```

The Hydra configuration accepts the validated OpenUSD 26.05 shared SDK and
records its detected header version in release metadata. It rejects other
versions/layouts. On MSVC, a Debug hdMerlin build is also rejected when the SDK
exports only Release libraries; use `--config Release` or provide a matching
Debug OpenUSD SDK. Compiler/toolset ABI compatibility still has to match the
consumer, and the discovery/usdview tests must run against the same runtime
root used at configure time.

## Install

Install a configured build into an isolated prefix:

```powershell
cmake --install build --config Release --prefix C:/merlin
```

Core headers, libraries, and versioned CMake package files are always installed.
Vulkan-enabled builds also install the Vulkan library, `merlin-headless`, and its
SPIR-V shaders. Hydra-enabled builds install the `hdMerlin` plugin below
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

Use a new build directory when changing dependency roots or major capability
options. Existing CMake caches can otherwise retain an older Vulkan or OpenUSD
installation.
