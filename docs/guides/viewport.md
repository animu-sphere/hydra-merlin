# Running the viewport and Hydra hosts

This guide covers launching the native `merlin-viewport`, opening USD stages,
using hdMerlin inside usdview, and selecting Gaussian execution. Build the
products first with [build and install](build-and-install.md). For validated
platforms, hosts and feature limits, see the
[support matrix](../reference/support-matrix.md).

## OpenStrata-managed launch

The `viewport-usd` intent declared in `openstrata.toml` builds the
Hydra-enabled standalone viewport and opens a USD stage:

```powershell
ost renderer viewport --intent viewport-usd --profile usd -- `
  --usd C:/path/to/scene.usd --frames 1 --hidden
```

Omit `--frames` and `--hidden` to explore the stage interactively. Use a real
`usd` runtime when the scene needs OpenUSD and its dependencies. To inspect a
stage in usdview instead, run `ost renderer view --profile usd`. Runtime
selection and external build directories are described in the
[OpenStrata project layout](../design/openstrata-project.md#managed-hydra-view-normal-path).

## Native viewport

Windows / Vulkan:

```powershell
./build/vulkan/adapters/merlin-viewport/Release/merlin-viewport.exe `
  --backend vulkan --vsync off --benchmark viewport.json
```

macOS / Metal:

```bash
./build/metal/adapters/merlin-viewport/merlin-viewport --backend metal --vsync on
```

Run `merlin-viewport --help` for the complete option list. Common options are
`--backend vulkan|metal`, `--vsync on|off`, `--width`/`--height`,
`--frames N` with `--hidden` for scripted runs, `--screenshot`, and
`--benchmark <file>`. Metal viewports can raise the default 64 MiB scene heap
for large stages with `--metal-heap-mib N`.

### Opening USD stages

USD loading and `Open USD...` require a Hydra-enabled build (`vulkan-hydra` or
`metal-hydra`). On Windows, the generated `run-merlin-viewport.cmd` beside the
developer-build executable prepends the configured OpenUSD `bin` and `lib`
directories for that process only, avoiding a machine-wide `PATH` edit:

```powershell
./build/vulkan-hydra/adapters/merlin-viewport/Release/run-merlin-viewport.cmd `
  --usd C:/path/to/scene.usdc
```

On macOS and Linux, expose the SDK's libraries through the platform loader
path:

```bash
DYLD_LIBRARY_PATH=/path/to/openusd/lib \
  ./build/metal-hydra/adapters/merlin-viewport/merlin-viewport \
  --backend metal --usd /path/to/scene.usdc
```

### Navigation and inspection

USD stages use usdview-compatible initial framing: the render/proxy bounds,
authored Y/Z `upAxis`, 60-degree vertical FOV, maximum bounds dimension, and a
1.1 frame-fit margin.

| Input | Action |
| --- | --- |
| `F` | Restore the initial framing |
| Alt+left drag | Tumble |
| Alt+middle drag, or Alt+Ctrl+left drag | Track |
| Alt+right drag, mouse wheel | Dolly |
| Arrow keys | Pan |
| Left click | Pick (`primId`/`instanceId` readback) |
| `S` | Write a PPM screenshot |
| Escape | Close the window |

Normal frames present through the swapchain or `CAMetalLayer` without CPU
readback. The Dear ImGui diagnostics panel shows capabilities, timing,
residency, AOVs and materials. It can apply a new clear color or opt into
continuous color CPU readback; each request shows its applied revision or
rejection reason. Continuous readback is a timing-affecting inspection mode.

## usdview and other Hydra hosts

Use the Python environment supplied or specified by the OpenUSD package,
including its Qt bindings, PyOpenGL and NumPy:

- `PYTHONPATH` must expose `<openusd>/lib/python`.
- `PXR_PLUGINPATH_NAME` must expose the installed Merlin
  `<prefix>/lib/usd/hdMerlin/resources` directory.
- On Windows, add the OpenUSD `bin` and `lib` directories to the process
  `PATH`; on macOS, expose the SDK's `lib` through `DYLD_LIBRARY_PATH` when the
  package needs it.

Launch the SDK's `usdview` with `--renderer Merlin /path/to/scene.usdc`. These
host dependencies are separate from `slangc` and the backend's SDK.

### Presentation path

Hydra presentation uses CPU RenderBuffers (Tier 0) as the universal path. When
the build includes the HgiVulkan or HgiMetal bridge and the host supplies a
matching Hgi driver, Merlin copies AOVs into Hgi-owned targets on the GPU
instead. Set `MERLIN_HYDRA2_DISABLE_HGI_METAL_BRIDGE=1` before creating a
Hydra delegate to force Tier 0 on Metal without rebuilding the plugin. The
[Hgi host presentation policy](../design/hgi-host-presentation.md) defines
both paths.

### Metal memory limits

For large stages opened through a Hydra host, `MERLIN_METAL_HEAP_MIB=N` raises
the Metal scene heap from its 64 MiB default (the viewport equivalent is
`--metal-heap-mib N`). Gaussian residency and scratch have separate limits,
1 GiB each by default:

| Limit | Hydra environment variable | Viewport option |
| --- | --- | --- |
| Gaussian residency | `MERLIN_METAL_GAUSSIAN_RESIDENCY_MIB` | `--metal-gaussian-residency-mib N` |
| Gaussian scratch | `MERLIN_METAL_GAUSSIAN_SCRATCH_MIB` | `--metal-gaussian-scratch-mib N` |

Values are positive integer MiB. Residency includes upload staging and versions
retained by unfinished commands, so a full first upload needs more than the
final attribute size.

## Gaussian execution

The CPU-sorted path is the default and the reference. Two policies select
optional GPU execution:

| Policy | Values | Meaning |
| --- | --- | --- |
| GPU execution | `disabled` (default), `prefer`, `require` | `prefer` falls back per stage when a GPU stage is unavailable; `require` reports unsupported capabilities instead of falling back. |
| Raster | `sorted-stream`, `tiled` | `tiled` selects the compute tile raster. A tiled frame that exceeds its pair capacity keeps the complete GPU sorted-stream draw. |

In the native viewport, pass `--gaussian-gpu` and `--gaussian-raster`:

```powershell
ost renderer viewport --intent viewport-usd --profile usd -- `
  --usd C:/path/to/gaussians.usd --gaussian-gpu prefer --gaussian-raster tiled
```

In usdview, the **Hydra Settings** menu has **GPU Gaussian execution**
(`prefer` when checked) and **Gaussian tile raster** checkboxes. Other Hydra
hosts see the same two flags, `merlin:gpuDrivenGaussian:enabled` and
`merlin:gpuDrivenGaussian:tiled`, and can also set
`merlin:gpuDrivenGaussian:mode` and `merlin:gpuDrivenGaussian:raster` to the
names above, for example to select `require`. A rejected value is reported as
a `renderer-settings.*` warning and the previous policy stays applied. The
developer UI's Gaussian counters show the selected path. The
[renderer settings](../design/renderer-settings.md) and
[Gaussian rendering](../design/gaussian-rendering.md) designs define the
contract.

For local Metal Release measurements, `merlin-metal-gaussian-tests --benchmark`
emits per-frame CSV for deterministic 64K/1M scenes, including static, camera
and localized-edit frames. These measurements are not CI timing gates; see
[benchmarking](benchmarking.md) for comparable evidence.
