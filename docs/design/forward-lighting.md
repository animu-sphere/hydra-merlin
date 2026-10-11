# Forward lighting boundary

**Last reviewed:** 2026-10-11

Forward remains the image reference for optional shading paths. This document
defines the current directional input contract and the remaining quality work;
it does not claim a complete physically based lighting model.

## Directional selection and coordinates

Vulkan and Metal use `ExtractForwardDirectionalLighting` in
`Merlin::RenderBackend`. The current single-light profile selects the
directional record with the smallest stable light handle. Dense table positions
are storage locations, so deleting an unrelated light or compacting a table
cannot change the selected source. Removing the selected light selects the
remaining smallest handle. This is an explicit single-light limitation, not
additive multi-light evaluation or special priority for host camera lights.

Lights emit along transformed local -Z. The shader receives the normalized
world-space +Z column, pointing from the surface toward the source. Translation
and camera matrices do not participate in this conversion. Normalization uses
double precision so very large or small finite float axes retain their
direction. A zero or non-finite axis explicitly recovers to +Z. Color and
intensity must be finite and nonnegative; invalid selected inputs produce an
`InvalidRequest` renderer error identifying the light. Zero intensity remains
zero. With no directional input, the existing unit white +Z diagnostic light
remains the fallback. Point, Spot and Dome records are not selected by this
profile.

The common conversion runs once per material preparation/render encoding,
not once per draw on Metal. It does not modify snapshots, geometry, material
variants or GPU resource identity.

## Current image behavior and remaining work

Vulkan and Metal now implement the same default diagnostic output profile:
0.15 of the packaged diffuse SH environment plus 0.85 of the selected Lambert
direct term, followed by component-wise Reinhard compression before the linear
UNorm color AOV. Metal no longer uses constant ambient fill or uncompressed
white output. Alpha is preserved until the existing mask/attachment conversion;
host sRGB conversion still happens after the linear AOV.

`Merlin::RenderBackend` owns the GPU-independent Radiance loader, Lambert SH
projection/evaluation and the embedded packaged coefficients. The CPU gate
compares all embedded coefficients with the source HDR within 1e-6. Embedding
the 9 vectors gives Metal SDK consumers the same default without a runtime
asset-path dependency. Vulkan retains its configurable HDR path and native
error boundary; custom Vulkan environments have no Metal parity claim.
This code alignment still requires independent Metal images and backend
comparison evidence before v0.16.2 can close. `RendererSettings` still rejects
nonzero exposure and selectable tone mapping.

Hydra ingests `distantLight` color, intensity, exposure, diffuse, angle,
normalize and transform.
OpenUSD's Hdx task controller chooses `distantLight` for camera lights when
`simpleLight` is unavailable, and supplies intensity 15000 with angle 0.53.
See the pinned [OpenUSD 26.08 task controller source](https://github.com/PixarAnimationStudios/OpenUSD/blob/v26.08/pxr/imaging/hdx/taskController.cpp).
The adapter now converts that radiance into the face-on unit-albedo Lambert
response stored as Core intensity. For angular diameter `a` in `(0, 180]`
degrees, unnormalized input yields
`intensity * 2^exposure * diffuse * sin(a*pi/360)^2`.
Normalized input yields `intensity * 2^exposure * diffuse / pi`, independent
of angular size. The zero-angle delta case uses the UsdLux size-factor-1
convention and also divides by pi. These formulas follow the
[UsdLux normalization definition](https://www.forum.openusd.org/release/api/class_usd_lux_light_a_p_i.html).
For Hdx's intensity 15000, angle 0.53, exposure 0 and diffuse 1, the result
is approximately 0.321, rather than a Lambert multiplier of 15000. No
host-name/path heuristic or global exposure override participates.

Forward evaluates that amplitude at the light's center direction, retaining
the existing unshadowed directional approximation; it does not integrate an
extended emitter at every shading normal. Diameters above 180 degrees require
a broader illumination model and are rejected. Missing parameters use UsdLux
defaults: white, intensity/diffuse 1, exposure 0, angle 0.53 and normalize
false. Malformed values, non-finite/negative energy and unrepresentable output
produce `hydra.light.invalid-parameters`; rejection retains the entire previous
light, including its transform, or omits an invalid initial source. Zero
energy stays zero and clean Sync does not revise the light. This conversion
changes images of authored Hydra distant lights too; native Core-authored
intensity remains a directly specified Lambert response.

Focused Vulkan fixtures now cover Camera Light ON/OFF, static and rotated camera
poses, return to the original pose, and color/white patches. Native Forward
compares handwritten and generated MaterialX prototype evaluation; usdview uses
the supported UsdPreviewSurface subset. This does not claim generated-material
Hydra ingestion. The host fixture switches the real Camera Light checkbox and
compares disabled color correction with the host sRGB transfer function.

Camera Light OFF removes the host source. With no authored directional source,
Merlin selects its unit-white +Z diagnostic fallback, so OFF is brighter than
Hdx's converted camera light. It is not an ambient-only mode. The native fixture
also tests an explicit zero-energy source separately, which retains only ambient
fill. Tests record this distinction instead of silently equating OFF with zero.

Repeated native frames and returned poses require exact color/depth/ID equality.
Generated versus handwritten color allows one RGBA8 step, with exact depth/IDs.
Native versus Tier 0 compares interior center RGB with one-step tolerance;
different native/host projections preclude full-image equality. Host sRGB centers
allow two steps after the independently evaluated transfer function. Tier 0/Hgi
full-image comparison retains the existing 0.25% changed-pixel and 0.25 mean-channel
error bounds. Camera/light/output edits require zero geometry fetch/upload and
zero pipeline creation after setup. PPM/PNG images, center samples, structural
counters and host traces accompany the checks.

A separate first-frame motion gate sweeps 17 poses from 0 to 30 degrees of yaw
and back in 1.875-degree steps. Each color/white, linear/sRGB and Camera Light
ON/OFF combination first records converged references, then captures 32
successive camera edits without processing the event loop or waiting for
convergence. The synchronous framebuffer grab must produce exactly one renderer
event per capture, with the camera change present. Every immediate image must
equal its independently captured settled pose exactly; this also checks the
return leg. All 256 moving frames per presentation path retain color headroom,
zero geometry fetch/upload, zero pipeline creation and zero validation messages.
The separate Tier 0/HgiVulkan comparison covers the baseline, 136 references
and 256 moving frames using the existing full-image tolerances. Images,
per-frame counters and host traces are retained independently of the converged
lighting gate.

An independent navigation gate sends Qt Alt+mouse press/move/release events to
usdview's StageView for tumble, truck and zoom. A focused patch camera has a
five-unit focus distance and a wider lens so all three gestures change visible
geometry. Each eight-step excursion/return route is first captured with settled
frames, then replayed from a cloned free-camera state without processing the
event loop or waiting for convergence. All 192 immediate frames per presentation
path require one renderer event carrying the camera edit, the same replayed
camera transform and exact settled-image equality. Camera Light ON/OFF,
color/white, linear/sRGB, center headroom, zero geometry fetch/upload, zero
pipeline creation and clean validation remain gates. Restoring the initial
camera must restore its exact image after each gesture; zoom's multiplicative
out/back steps are not assumed to undo each other. Tier 0/HgiVulkan compares
433 images, including baseline, references, moving frames and restored views,
with the existing full-image tolerances.

An opt-in external scene gate uses `MERLIN_FORWARD_SCENE` to open a multi-mesh
USD stage directly, preserving authored geometry, transforms, materials and up
axis. Three independent starting views derive from usdview's frame-all free camera:
`full` retains it; `quarter` adds yaw +45 degrees, pitch -15 degrees and distance
scale 0.8; `opposite` adds yaw -45 degrees, pitch +15 degrees and distance scale
0.65. Rotations use the free camera's stage-up-axis convention. Each view reuses
the eight-step Qt tumble/truck/zoom routes for Camera Light ON/OFF and linear/sRGB
output, and starting views must have distinct images. Authored scene lights are
disabled to isolate the camera source. All 288 immediate frames per presentation
path must render once, exactly match their settled reference and replayed camera,
preserve draw counts, and avoid geometry fetch/upload or pipeline creation.
Camera restoration is exact; camera-light and output toggles must visibly change
the starting image at every viewpoint. The `merlin-usdview-forward-scene/v2`
report records the view recipe and initial camera matrix, source/root-layer SHA-256,
mesh count, coverage, diagnostics and full-frame stored-PNG RGB saturation fractions
(`clipped_pixel_fraction`). These count any RGB channel equal to 255, including
unassociated translucent samples, and do not establish displayed-color clipping.
They are observations, without a universal threshold for arbitrary assets;
the focused patch headroom gate remains separate. Root-layer identity alone does
not pin referenced layers or textures. Tier 0/HgiVulkan compares 649 images with
the existing tolerances and verifies matching scene metadata. The asset is supplied
locally; it is neither bundled nor automatically downloaded.

These bounded sweeps edit an authored camera or synchronously dispatch Qt mouse
events between captures. The three relative views do not establish interior scene
coverage. Unrestricted interactive navigation and broader Kitchen quality,
explicit exposure/tone-mapping selection and cross-backend output alignment
remain incomplete. Focused Metal native and host diagnostic evidence is recorded below.
Framebuffer capture adds readback and disk I/O, so the sweep is not a timing gate
or evidence for the
absence of flicker at an unrestricted navigation rate. Cross-backend
lighting/output parity is also still unverified.

## Regression evidence

### Comparing default backend output

After independent native and host runs, compare the saved reports without
requiring either machine to run the other backend:

```console
python tests/compare_forward_backends.py VULKAN_NATIVE_DIR METAL_NATIVE_DIR comparison.json --vulkan-host VULKAN_HOST_JSON --metal-host METAL_HOST_JSON
```

Native directories contain `color/comparison.json` and `white/comparison.json`;
host inputs are the `usdview-first-frame-forward-lighting.json` files from the
same presentation mode (run Tier 0 and Hgi comparisons separately).
The gate requires 20 native center pairs covering the seven light/camera phases
and three normal-only environment phases, within one RGBA8 step. The 24 host
center pairs cover linear/sRGB, color/white, ON/OFF and static/moving/restored
poses within two steps. It rejects stale output profiles, duplicate/missing
phases, saturation, uploads and pipeline creation. Full-image Tier 0/Hgi,
exact replay/AOV checks and validation remain independent existing gates;
center parity does not establish full-image backend rasterization parity.
Dark normal-only environment channels may quantize to zero; they still require
the declared image tolerance and reject saturated channels. The focused
Camera Light ON/OFF centers retain their positive, unsaturated bounds.

The comparator's GPU-free boundary tests do not count as Metal execution or
backend image evidence. Updated Metal compilation, Debug/Release native images,
Tier 0/HgiMetal host images and actual backend report comparison are still
pending; v0.16.2 remains open until these pass.

Windows validation of the shared environment and refreshed Vulkan native,
generated-material and host fixtures is recorded in the
[output-alignment delivery record](../reports/delivery-history.md#forward-default-output-alignment--2026-10-11).

The CPU contract test exercises actual RenderWorld/extractor table compaction,
retained snapshots, camera changes, light translation, zero intensity,
extreme/degenerate axes, invalid energy and selected-light removal. The Vulkan
material regression renders differently colored directional inputs, removes an
unselected Point record that moves the last table slot, and requires exact
color/depth/primId/instanceId parity with zero geometry upload and no pipeline
creation. Installed Core consumers compile and link the same helper. Metal
uses the common implementation; the independent native Metal fixture exercises
it separately. Fresh Apple GPU execution remains required for the changed output
profile.

The native `merlin-metal-forward-lighting` gate covers the same color/white
patches and converted Hdx camera-light amplitude through Core-authored inputs,
without a Hydra host or OpenUSD SDK. Twenty-two phases per patch include ON/OFF,
camera-only motion with a world-space source, rotated and restored camera/light
poses, explicit zero energy, colored stable-handle selection through table
compaction, retained snapshots, light translation, extreme/degenerate axes,
selected-source removal and no-source recovery. Six invalid energy/color inputs
per patch must reject and recover to an exact valid image. Stable-handle values
include the generation, so creation order alone does not determine selection.

An independent analytic center oracle checks Metal's current
`linear = albedo * (0.15 * SH(N) + light_color * 0.85 * max(N dot L, 0) * intensity)`
and `linear / (1 + linear)` profile
within one RGBA8 step. Three repeats per phase and returned views require exact
color/depth/primId/instanceId equality. The fixture warms all frame contexts
before requiring zero uploads, allocations and pipeline creation for edits and
static frames. All focused centers, including white OFF, now require headroom.
Native Vulkan and Metal fixtures also isolate SH fill with +Y, -Y and +X shading
normals, without a direct contribution. The prior Metal profile's OFF/ON/rotated/
zero white centers of 255/108/98/38 are historical evidence for the pre-alignment
shader and do not validate the updated output.

Run this native gate in a Metal build:

```sh
ctest --test-dir build/metal -R '^merlin-metal-forward-lighting$' --output-on-failure
```

CTest enables Metal
API and shader validation; unavailable devices return an explicit capability
skip. PPM images, expected/actual center RGB, resource counters and rejection
counts live under `tests/forward-images-metal/{color,white}`. The scheduled
Debug/Release Metal capability workflow selects the gate and archives its images
and reports. Before output alignment, local Apple M3/macOS 15.7.9/AppleClang 17/Slang 2026.8 validation on
2026-10-11 passes the full Release Metal selection 31/31 and the focused Debug
selection 3/3 without skips. All 38 center comparisons have zero observed error,
all 38 PPM images and both reports match exactly between Debug and Release, and
validation messages remain zero. Commands and scope are in the
[Metal delivery record](../reports/delivery-history.md#native-metal-forward-lighting--2026-10-11).

The GPU-free Hydra energy regression independently integrates a spherical
light cap at four angles and checks normalized size invariance, exposure and
diffuse scaling, zero/delta lights, overflow, underflow and malformed inputs.
An injected backend captures the actual Sprim-to-snapshot path, including
rotation, parameter-only edits, unchanged revisions, atomic rejection,
initial rejection/retry and removal. These tests verify input energy; broader
scene and output quality require independent image evidence.

### Metal host presentation gates

Metal/Hydra builds now register the same focused lighting, first-frame sweep
and Qt navigation fixtures for `tier0` and, when the native bridge is built,
`hgi-metal`. Tier 0 sets `MERLIN_HYDRA2_DISABLE_HGI_METAL_BRIDGE=1` before
delegate creation; HgiMetal enables the bridge and requires GPU-copy telemetry
instead of accepting fallback. Both use the same installed plugin and enable
Metal API validation. Native Metal gates separately enable shader validation;
host shader instrumentation fails OpenUSD display pipeline creation on the
validated runtime and is explicitly disabled in the host runner.
Each host report retains images, resource
counters and Chrome traces; comparison checks baseline transfer evidence as
well as image tolerances.

The native/host lighting comparison selects the Metal analytic fixture through
a separate CTest fixture dependency, checks all 12 linear center pairs within
one RGBA8 step, and rejects mismatched backend reports. Tier 0/HgiMetal keeps
the existing full-image tolerances and requires all lighting phases in order.
Linear/sRGB centers retain the independent two-step transfer-function check.
White OFF now requires headroom in both output modes, as on Vulkan.
Motion/navigation use the same condition while retaining exact first-frame/
reference and restored-image equality. Fresh Metal native and host runs remain
required to validate the changed profile.

Run the independent gates in a Metal/Hydra build:

```sh
ctest --test-dir build/metal-hydra --output-on-failure \
  -R '^merlin-hydra2-forward-(lighting|motion|navigation)'
```

Host artifacts live under
`adapters/merlin-hydra2/forward-{lighting,motion,navigation}-{tier0,hgi-metal}-install`;
comparison JSON files live alongside those directories. Optional external
scene fixtures also register both presentation modes when configured; this
does not establish Metal Kitchen coverage. Generated Metal materials,
cross-backend output alignment, unrestricted navigation and timing remain
separate gates.

Before output alignment, local Apple M3/OpenUSD 26.08 Release validation on 2026-10-11 passes the final
16/16 selection, including installed-package and native AOV dependencies.
All 12 native/host center pairs and 851 Tier 0/HgiMetal images match exactly;
each presentation path passes 256 immediate sweep and 192 immediate Qt
navigation captures. Geometry fetch/upload, pipeline creation and validation
messages remain zero. Commands, the host shader-validation limitation and
environment are in the [Metal host delivery record](../reports/delivery-history.md#metal-forward-host-lighting-and-navigation--2026-10-11).

### Vulkan host presentation gates

Run the focused fixtures in a Vulkan/Hydra build with testusdview available:

```powershell
ctest --test-dir build/vulkan-hydra -C Release `
  -R '^merlin-hydra2-forward-lighting' --output-on-failure
```

CTest also selects the native material fixture needed by the center comparison.
An SDK exporting `hgiVulkan` adds the separate Hgi host and full-image comparison
cases. Native MaterialX comparison is part of `merlin-vulkan-generated-material`
when that optional compiler and its Forward artifacts are enabled. Native output
lives under `tests/forward-images-{handwritten,generated}/{color,white}`; host
output lives under `adapters/merlin-hydra2/forward-lighting-*-install`.

Run the independent first-frame motion cases with
`-R '^merlin-hydra2-forward-motion'`. Their evidence lives under
`adapters/merlin-hydra2/forward-motion-*-install`, with the separate
`forward-motion-comparison.json` report. The Windows GPU capability workflow
selects and archives these cases alongside the existing lighting fixtures;
GPU-free hosted Hydra validation still excludes them through `host-smoke`/`gpu`.

Run the separate Qt navigation cases with
`-R '^merlin-hydra2-forward-navigation'`. Images, per-frame JSON and host traces
live under `adapters/merlin-hydra2/forward-navigation-*-install`, with a separate
`forward-navigation-comparison.json`. The Windows GPU capability workflow runs
and archives these gates for both OpenUSD versions; CI selection alone does not
establish successful 26.05 execution.

Configure an existing Hydra/host build with an external stage and run its
independent scene gate:

```powershell
cmake -S . -B build/vulkan-hydra -DMERLIN_FORWARD_SCENE=C:/assets/Kitchen_set/Kitchen_set.usd
ctest --test-dir build/vulkan-hydra -C Release -R '^merlin-hydra2-forward-scene' --output-on-failure
```

An empty path leaves the scene cases unregistered; an explicitly configured
missing file fails configure. Images, per-frame JSON, structural counters and
host traces live under `adapters/merlin-hydra2/forward-scene-*-install`, with
`forward-scene-comparison.json` alongside them. These optional local asset cases
are not provisioned by the GPU capability workflow.

Local 2026-10-10 Release validation on RTX A5000/OpenUSD 26.08 passes 8/8
lighting/motion cases including the native fixture dependency. Both presentation
paths pass all 256 one-render captures; all 393 motion-comparison images have
zero observed channel error. Environment and limits are recorded in the
[delivery history](../reports/delivery-history.md#forward-first-frame-camera-sweep--2026-10-10).

Initial 2026-10-11 Release validation of the single-view external scene gates
on the same host
passes 3/3 for the supplied Kitchen_set stage (1,788 meshes, Z up). Both paths
pass all 96 immediate captures with exact settled-pose equality. All 217
presentation comparisons are within the existing tolerances; the final maximum
changed-pixel fraction is about 0.005%, not zero. At most three stored-PNG pixels
have saturated channels; inspected samples have alpha 178/255, so this is not
proof of displayed-color clipping. The full final selection passes 18/18,
including existing Forward and installed-package/runtime checks.
Environment, scene identity and limits are in the
[Kitchen delivery record](../reports/delivery-history.md#forward-external-kitchen-navigation--2026-10-11).

The three-view expansion on the same host passes all 288 immediate captures per
path with exact settled-reference images. The 649-image Tier 0/HgiVulkan comparison
passes the unchanged aggregate tolerances; the final full selection passes 19/19,
including existing Forward and installed-package/runtime gates. Captured draws remain 1,788 and
geometry fetch/upload, pipeline creation, diagnostics and validation messages
remain zero. Stored-PNG saturation reaches 17 pixels in a closer-view OFF/sRGB
capture; those inspected samples all have alpha 178/255. This remains an
observation rather than proof of displayed-color clipping. Validation commands
and scope are in the [viewpoint delivery record](../reports/delivery-history.md#forward-kitchen-starting-viewpoints--2026-10-11).

Local 2026-10-11 Release validation on the same RTX A5000/assembled OpenUSD 26.08
host passes 11/11 lighting/motion/navigation cases, including the native fixture
dependency. Both navigation paths pass all 192 one-render captures; all 433
navigation comparison images have zero observed channel error. The installed
consumer/runtime and Core/Hydra lighting selection passes 4/4 after a successful
incremental build. Scope and commands are recorded in the
[navigation delivery record](../reports/delivery-history.md#forward-qt-mouse-navigation--2026-10-11).

Local 2026-10-10 Release validation on RTX A5000/OpenUSD 26.08 passes all five
native/Tier 0/HgiVulkan cases. The 25 presented images (baseline plus 24 fixture
phases) match exactly across Tier 0 and HgiVulkan; all 12 native/host center
comparisons and both color/white generated-material comparisons have zero
observed channel error. The native material run plus its three compilation
fixtures passes 5/5, and the Core lighting/Hydra energy/install-consumer/runtime
selection passes 4/4. These are local fixture results, not Kitchen, OpenUSD 26.05,
Metal, continuous-navigation, or timing guarantees. The separate lookdev SDK's
existing comprehensive smoke fails its authored-normal handedness image check
before reaching any lighting fixture; the independent gate remains usable.
