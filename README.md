# hdMerlin

hdMerlin is an OST-oriented, host-neutral Vulkan raster renderer. The first
implementation slice provides a handle-based `RenderWorld`, committed change
sets, and a Vulkan offscreen triangle renderer with CPU readback.

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
./build/adapters/merlin-headless/Debug/merlin-headless.exe --output merlin.ppm
```

The current slice requires a Vulkan 1.2-capable graphics queue and `glslc`
from the Vulkan SDK at build time.
