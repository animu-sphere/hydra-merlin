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

## Hydra 2 adapter skeleton

The OpenUSD adapter is opt-in so Hydra never becomes a transitive dependency of
the normal Core/Vulkan build. Point `CMAKE_PREFIX_PATH` at an OpenUSD 26.05 SDK:

```powershell
cmake -S . -B build-hydra2 -G "Visual Studio 17 2022" -A x64 `
  -DMERLIN_ENABLE_HYDRA2=ON `
  -DCMAKE_PREFIX_PATH=C:/path/to/openusd
cmake --build build-hydra2 --config Release
ctest --test-dir build-hydra2 -C Release --output-on-failure
```

The current Hydra slice provides the renderer plugin, delegate/pass skeleton,
color/depth AOV descriptors, `plugInfo.json`, and a GPU-independent discovery
test. Mesh/camera sync and render-buffer handoff are the next adapter slice.

The Vulkan path requires a Vulkan 1.2-capable graphics queue and `glslc`
from the Vulkan SDK at build time.
