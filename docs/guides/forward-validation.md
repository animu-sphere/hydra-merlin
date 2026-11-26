# Forward validation

This guide runs the Forward lighting gates and compares backends. The
[Forward lighting design](../design/forward-lighting.md#validation-contract)
defines what each gate checks and its tolerances; the
[support matrix](../reference/support-matrix.md) records what the resulting
evidence establishes.

Host gates need an available GPU, a window service, and an OpenUSD runtime that
includes usdview (an SDK-only package does not). CTest reports an unavailable
device or capability as a skip; review the output rather than treating a skip
as exercised coverage.

## Exposure and tone mapping

Native/API clients set `RenderRequest::output_transform` to
`{exposure_ev, ToneMapping::None}` or `{exposure_ev, ToneMapping::Reinhard}`.
Default construction selects 0 EV/Reinhard. The development viewport exposes
Forward exposure and tone mapping under renderer settings; use Apply to commit
the draft. Hydra hosts set `merlin:exposureEv` (float/double) and
`merlin:toneMapping` (`none`/`reinhard` string/token). These affect lit Forward;
Gaussian, clear color and unlit diagnostic colors keep their existing policy.
None can clip at the RGBA8 AOV and does not provide HDR output.

Run the CPU and Vulkan output gates:

```powershell
ctest --test-dir build/vulkan -C Release `
  -R 'merlin-(output-transform|vulkan-output-|metal-forward-source-abi)' --output-on-failure
```

The Vulkan modes are separate cases so optional-path skips stay visible.
`merlin-metal-forward-source-abi` produces native MSL and checks reflection
without Xcode or device execution. The Metal lighting gate includes the shared
native output fixture. Hydra lighting gates additionally capture
`usdview-first-frame-forward-output.json` and `output-*.png`, with linear/sRGB
oracle comparisons and exact image restoration; Tier 0/Hgi comparison includes
those 16 output images.

## Native gates

Vulkan material and lighting fixtures are part of the Vulkan test suite. The
generated-material comparison runs as `merlin-vulkan-generated-material` when
the optional MaterialX compiler and its Forward artifacts are enabled. Native
output lives under `tests/forward-images-{handwritten,generated}/{color,white}`.

Run the native Metal gate in a Metal build:

```sh
ctest --test-dir build/metal -R '^merlin-metal-forward-lighting$' --output-on-failure
```

CTest enables Metal API and shader validation. PPM images, expected/actual
center RGB, resource counters and rejection counts live under
`tests/forward-images-metal/{color,white}`. The same gate, and each
`merlin-vulkan-output-<mode>` case, also runs the analytic
[surface geometry](../design/surface-geometry.md) fixture: 21 single-patch
cases compare the `geometric-normal`, `shading-normal-base`, `normal`,
`front-facing`, `tangent` and `bitangent` views with a double-precision oracle
(at most 1/255 per channel) and require single-sided back faces to stay
uncovered. Eight cases bind a constant tangent-space normal texture and cover
generated and authored tangents, scaled and green-flipped maps, transformed,
mirrored and double-sided frames, the derivative frame and the disabled map
without texture coordinates. The usdview normals fixture adds the same
normal-map probes through UsdPreviewSurface.

## Vulkan host gates

Run the focused lighting fixtures in a Vulkan/Hydra build with testusdview
available:

```powershell
ctest --test-dir build/vulkan-hydra -C Release `
  -R '^merlin-hydra2-forward-lighting' --output-on-failure
```

CTest also selects the native material fixture needed by the center
comparison. An SDK exporting `hgiVulkan` adds the Hgi host and full-image
comparison cases. Select the other gates with:

| Gate | CTest selection | Output |
| --- | --- | --- |
| Lighting | `-R '^merlin-hydra2-forward-lighting'` | `adapters/merlin-hydra2/forward-lighting-*-install` |
| First-frame motion | `-R '^merlin-hydra2-forward-motion'` | `adapters/merlin-hydra2/forward-motion-*-install`, `forward-motion-comparison.json` |
| Qt navigation | `-R '^merlin-hydra2-forward-navigation'` | `adapters/merlin-hydra2/forward-navigation-*-install`, `forward-navigation-comparison.json` |
| Surface normals | `-R '^merlin-hydra2-forward-normals'` | `adapters/merlin-hydra2/forward-normals-*-install`, `forward-normals-comparison.json` |
| External scene | `-R '^merlin-hydra2-forward-scene'` | `adapters/merlin-hydra2/forward-scene-*-install`, `forward-scene-comparison.json` |

Each host report retains images, per-frame JSON, structural counters and host
traces.

The lighting gate also exercises all eight combinations of usdview's Scene
Lights, Default Camera Light, and Default Dome Light using a red authored
distant light. It checks the Stinson Beach background and verifies that
`domeLightCameraVisibility` leaves the dome contribution on surfaces intact.

### External scene

Configure an existing Hydra build with a local stage, then run its gate:

```powershell
cmake -S . -B build/vulkan-hydra -DMERLIN_FORWARD_SCENE=C:/assets/Kitchen_set/Kitchen_set.usd
ctest --test-dir build/vulkan-hydra -C Release -R '^merlin-hydra2-forward-scene' --output-on-failure
```

An empty path leaves the scene cases unregistered; an explicitly configured
missing file fails configure. The asset is not provisioned by the GPU
capability workflow. On Metal the runner raises `MERLIN_METAL_HEAP_MIB` to 1024
unless it is already set, because Kitchen_set exceeds the 64 MiB default scene
heap.

Supply the checked-in camera recipes to run the Kitchen_set interior views.

```powershell
$views = (Resolve-Path adapters/merlin-hydra2/tests/fixtures/kitchen-interior-views.json).Path
cmake -S . -B build/vulkan-hydra `
  -DMERLIN_FORWARD_SCENE=C:/assets/Kitchen_set/Kitchen_set.usd `
  "-DMERLIN_FORWARD_SCENE_VIEWS=$views"
ctest --test-dir build/vulkan-hydra -C Release -R '^merlin-hydra2-forward-scene' --output-on-failure
```

Each JSON entry defines a unique lowercase `name`, world-space `eye` and
`target` coordinates in stage units, and `vertical_fov_degrees`. The gate runs
the listed views in order. Set `-DMERLIN_FORWARD_SCENE_VIEWS=` to restore the
default frame-all views. Both configurations use the same artifact directories,
so save the first run's artifacts before running the other if you need both.
The same camera file can be supplied to a Metal/Hydra build.

### Open Chess Set

Use an OpenUSD runtime with the `usdMtlx` file-format plugin and the complete
Open Chess Set asset directory. Point the material gate at the original stage.

```sh
cmake -S . -B build/metal-hydra \
  -DMERLIN_OPEN_CHESS_SET=/path/to/OpenChessSet/chess_set.usda
ctest --test-dir build/metal-hydra -R '^merlin-hydra2-forward-chess' --output-on-failure
```

The gate works with Vulkan/Hydra as well. It checks the board and pieces,
including the point-instanced pawns. Session-layer overrides isolate authored
base color, normal mapping, metalness and roughness on the board and each set
of pieces. A separate surface measures reflection and highlight width under
the camera light and a distant light, with and without the dome. It also
compares smooth and rough metal with only the default dome illuminating the
surface and its panorama hidden. With every light disabled, the same surface
checks constant emission, UV0 orientation, sRGB decoding, live intensity
edits, and recovery after unsupported connections or missing image assets.
Images, input counts and probe measurements are written to
`adapters/merlin-hydra2/forward-chess-*-install` under the build directory. When a native Hgi bridge is available, the test compares its
images with Tier 0. For a manual launch on Metal, set
`MERLIN_METAL_HEAP_MIB=1024` before starting usdview. The test runner uses the
same heap size by default. For a close view, use **Camera → Free Camera
Settings... → Override Near** and set it to `0.02` stage units.

## Metal host gates

Metal/Hydra builds register the lighting, first-frame motion, Qt navigation and
surface normals
fixtures for `tier0` and, when the native bridge is built, `hgi-metal`:

```sh
ctest --test-dir build/metal-hydra --output-on-failure \
  -R '^merlin-hydra2-forward-(lighting|motion|navigation|normals)'
```

Tier 0 sets `MERLIN_HYDRA2_DISABLE_HGI_METAL_BRIDGE=1` before delegate
creation; `hgi-metal` enables the bridge. Selecting `merlin-hydra2-forward-lighting`
also selects the native Metal fixture for the center comparison. Host
artifacts live under
`adapters/merlin-hydra2/forward-{lighting,motion,navigation,normals}-{tier0,hgi-metal}-install`,
with comparison JSON files alongside. Configuring `MERLIN_FORWARD_SCENE`
registers the external scene gate for both presentation modes.

## Comparing backends

After independent native and host runs on each backend, compare the saved
reports without requiring either machine to run the other backend:

```console
python tests/compare_forward_backends.py VULKAN_NATIVE_DIR METAL_NATIVE_DIR comparison.json --vulkan-host VULKAN_HOST_JSON --metal-host METAL_HOST_JSON
```

Native directories contain `color/comparison.json` and `white/comparison.json`.
Host inputs are the `usdview-first-frame-forward-lighting.json` files from the
same presentation mode; run the Tier 0 and Hgi comparisons separately. Add
`--vulkan-output` and `--metal-output` with the matching
`usdview-first-frame-forward-output.json` files to compare the 16 selectable
exposure/tone-mapping centers within the host tolerance.

## Capability workflows

The Windows GPU capability workflow selects and archives the native output,
lighting, motion and navigation gates using the pinned OpenUSD 26.08 Vulkan
Lookdev runtime and its matching Python interpreter, plus a MaterialX-enabled
Debug/Release build that executes generated and handwritten Forward. JUnit
checks reject missing or skipped required cases. Dispatch `suite=hydra` to
refresh only the host gates, or `suite=generated` for only the generated
Forward build. The scheduled Debug/Release Metal capability workflow
selects the native Metal gate and archives its images and reports. GPU-free
hosted Hydra validation excludes these cases through the `host-smoke` and `gpu`
labels; SDK-only compilation does not establish device-host execution.
