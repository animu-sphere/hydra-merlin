# Forward lighting

**Last reviewed:** 2026-10-12

Forward is the image reference for optional shading paths. This document
defines the Forward lighting contract: directional-light selection, Hydra light
energy conversion, the default output profile, and the validation rules that
compare backends and host presentation paths. It does not define a complete
physically based lighting model. Current implementation limits are listed in
the [support matrix](../reference/support-matrix.md#known-limitations); run
procedures are in the [Forward validation guide](../guides/forward-validation.md).

## Directional selection and coordinates

Vulkan and Metal use `ExtractForwardDirectionalLighting` in
`Merlin::RenderBackend`. The single-light profile selects the directional
record with the smallest stable light handle. Dense table positions are storage
locations, so deleting an unrelated light or compacting a table cannot change
the selected source. Removing the selected light selects the remaining smallest
handle. Stable-handle values include the generation, so creation order alone
does not determine selection. This is an explicit single-light profile, not
additive multi-light evaluation or special priority for host camera lights.

Lights emit along transformed local -Z. The shader receives the normalized
world-space +Z column, pointing from the surface toward the source. Translation
and camera matrices do not participate in this conversion. Normalization uses
double precision so very large or small finite float axes retain their
direction. A zero or non-finite axis explicitly recovers to +Z. Color and
intensity must be finite and nonnegative; invalid selected inputs produce an
`InvalidRequest` renderer error identifying the light. Zero intensity remains
zero. With no directional input, a unit white +Z diagnostic light is the
fallback. Point, Spot and Dome records are not selected by this profile.

The common conversion runs once per material preparation/render encoding, not
once per draw on Metal. It does not modify snapshots, geometry, material
variants or GPU resource identity.

## Shared implementation boundary

`Merlin::RenderBackend` owns draw-matrix composition, inverse-transpose normal
matrices (including the singular-transform identity fallback), material-value
packing and environment/light inputs. Backends supply their native upload ABI
and selected environment to the common packing function.

`core/merlin-render-backend/shaders/forward-policy.slang` owns vertex evaluation,
SH lighting, the fixed output transform, alpha-mask rejection and color/ID
outputs. `gpu-scene-forward.slang` owns table-backed material/geometry feature
selection, transforms and material overrides. Conventional, bindless and
GPU-driven Vulkan entries and native Metal entries call these modules.
Bindings, argument-buffer capacity, sampler transport and material placement
across stages remain backend adapters. Metal retains fragment-stage material
multiplication; the Vulkan vertex path retains its existing material placement.
Sharing policy does not change stage interpolation or extend generated-material
support.

## Default output profile

Vulkan and Metal implement one default output profile:

```text
linear = albedo * (0.15 * SH(N) + light_color * 0.85 * max(N · L, 0) * intensity)
output = linear / (1 + linear)          (component-wise Reinhard)
```

`SH(N)` is the packaged diffuse environment evaluated at the shading normal and
`L` the selected directional source. The Reinhard result is written to the
linear UNorm color AOV. Alpha is preserved until the existing mask/attachment
conversion.

`Merlin::RenderBackend` owns the GPU-independent Radiance loader, Lambert SH
projection/evaluation and the embedded packaged coefficients. A CPU gate
compares all embedded coefficients with the source HDR within 1e-6. Embedding
the nine vectors gives Metal SDK consumers the same default without a runtime
asset-path dependency. Vulkan additionally accepts a configurable HDR
environment through its native error boundary; custom environments are outside
the shared profile and carry no cross-backend parity claim.

## Output transform

The output transform is fixed:

```text
scene-linear lighting
  -> component-wise Reinhard
  -> linear UNorm color AOV
  -> host/display color encoding (for example sRGB)
```

`RendererSettings` rejects nonzero exposure and selectable tone mapping, so no
renderer-side exposure precedes Reinhard. Hydra light `exposure` scales the
input energy of an individual light (see below) and is not a global exposure
control. Host sRGB conversion happens after the linear AOV and is owned by the
host or presentation path.

## Hydra distant-light energy

Hydra ingests `distantLight` color, intensity, exposure, diffuse, angle,
normalize and transform. OpenUSD's Hdx task controller chooses `distantLight`
for camera lights when `simpleLight` is unavailable, and supplies intensity
15000 with angle 0.53. See the pinned
[OpenUSD 26.08 task controller source](https://github.com/PixarAnimationStudios/OpenUSD/blob/v26.08/pxr/imaging/hdx/taskController.cpp).

The adapter converts that radiance into the face-on unit-albedo Lambert
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
the unshadowed directional approximation; it does not integrate an extended
emitter at every shading normal. Diameters above 180 degrees require a broader
illumination model and are rejected. Missing parameters use UsdLux defaults:
white, intensity/diffuse 1, exposure 0, angle 0.53 and normalize false.
Malformed values, non-finite/negative energy and unrepresentable output produce
`hydra.light.invalid-parameters`; rejection retains the entire previous light,
including its transform, or omits an invalid initial source. Zero energy stays
zero and clean Sync does not revise the light. The conversion applies equally
to authored Hydra distant lights; native Core-authored intensity remains a
directly specified Lambert response.

### Camera Light OFF

Camera Light OFF removes the host source. With no authored directional source,
Merlin selects its unit-white +Z diagnostic fallback, so OFF is brighter than
Hdx's converted camera light. It is not an ambient-only mode. An explicit
zero-energy source is a separate case that retains only the environment term.
Validation records this distinction instead of equating OFF with zero.

## Validation contract

These rules define what the Forward gates accept. They apply to each backend
and presentation path that registers the gate.

### Image and resource invariants

- Repeated native frames and returned camera poses require exact
  color/depth/primId/instanceId equality.
- Generated versus handwritten material color allows one RGBA8 step, with
  exact depth/IDs.
- Native versus Tier 0 compares interior center RGB within one step; different
  native/host projections preclude full-image equality. Host sRGB centers allow
  two steps after the independently evaluated transfer function.
- Tier 0/Hgi full-image comparison allows at most 0.25% changed pixels and 0.25
  mean-channel error.
- Camera, light and output edits require zero geometry fetch/upload and zero
  pipeline creation after setup; static frames additionally require zero
  uploads and allocations after all frame contexts are warmed.
- All focused color/white centers, including white OFF, require headroom in
  linear and sRGB output.
- Renderer validation messages must be zero. Native Metal gates enable API and
  shader validation; Metal host gates enable API validation only, because host
  shader instrumentation fails OpenUSD display pipeline creation.

### Native fixtures

The native fixtures render color and white patches through Core-authored
inputs, including the converted Hdx camera-light amplitude, without a Hydra
host. Phases cover Camera Light ON/OFF, camera-only motion with a world-space
source, rotated and restored camera/light poses, explicit zero energy, colored
stable-handle selection through table compaction, retained snapshots, light
translation, extreme/degenerate axes, selected-source removal and no-source
recovery. Invalid energy/color inputs must reject and recover to an exact valid
image. Three normal-only phases (+Y, -Y, +X shading normals, no direct term)
isolate the SH environment.

An independent analytic oracle checks each center against the default output
profile within one RGBA8 step. Dark normal-only channels may quantize to zero;
they still require the declared tolerance and reject saturation. Each phase
repeats three times and returned views must match exactly.

The Vulkan material regression additionally renders differently colored
directional inputs, removes an unselected Point record that moves the last table
slot, and requires exact four-AOV parity with zero geometry upload and no
pipeline creation. Native Forward compares handwritten and generated MaterialX
prototype evaluation; this does not cover generated-material Hydra ingestion.

The CPU contract test exercises RenderWorld/extractor table compaction,
retained snapshots, camera changes, light translation, zero intensity,
extreme/degenerate axes, invalid energy and selected-light removal; installed
Core consumers compile and link the same helper. The GPU-free Hydra energy
regression integrates a spherical light cap at four angles and checks
normalized size invariance, exposure and diffuse scaling, zero/delta lights,
overflow, underflow and malformed inputs. An injected backend captures the
Sprim-to-snapshot path, including rotation, parameter-only edits, unchanged
revisions, atomic rejection, initial rejection/retry and removal. These tests
verify input energy; output quality requires image evidence.

### Host lighting fixtures

usdview host fixtures use the supported UsdPreviewSurface subset, switch the
real Camera Light checkbox, and compare disabled color correction with the host
sRGB transfer function. Each fixture runs in `tier0` and, when the native bridge
is built and the host provides the matching Hgi driver, in the Hgi mode
(`hgi-vulkan` or `hgi-metal`). The Hgi mode requires GPU-copy telemetry rather
than accepting fallback. Native/host comparison checks linear center pairs
within one step and rejects mismatched backend reports; Tier 0/Hgi comparison
requires every lighting phase in order and checks baseline transfer evidence as
well as image tolerances.

### First-frame motion gate

A sweep moves through 17 poses from 0 to 30 degrees of yaw and back in
1.875-degree steps. Each color/white, linear/sRGB and Camera Light ON/OFF
combination first records converged references, then captures 32 successive
camera edits without processing the event loop or waiting for convergence. The
synchronous framebuffer grab must produce exactly one renderer event per
capture, with the camera change present. Every immediate image must equal its
independently captured settled pose exactly, including the return leg. Moving
frames retain color headroom, zero geometry fetch/upload, zero pipeline
creation and zero validation messages. Tier 0/Hgi comparison covers the
baseline, references and moving frames with the full-image tolerances.

### Qt navigation gate

Qt Alt+mouse press/move/release events drive usdview's StageView through
tumble, truck and zoom. A focused patch camera has a five-unit focus distance
and a wider lens so all three gestures change visible geometry. Each eight-step
excursion/return route is first captured with settled frames, then replayed
from a cloned free-camera state without processing the event loop or waiting
for convergence. Every immediate frame requires one renderer event carrying the
camera edit, the same replayed camera transform and exact settled-image
equality. Restoring the initial camera must restore its exact image after each
gesture; zoom's multiplicative out/back steps are not assumed to undo each
other. Camera Light ON/OFF, color/white, linear/sRGB, center headroom and the
resource invariants remain gates.

### External scene gate

An opt-in gate opens a local multi-mesh USD stage directly, preserving authored
geometry, transforms, materials and up axis. Three starting views derive from
usdview's frame-all free camera: `full` retains it; `quarter` adds yaw +45
degrees, pitch -15 degrees and distance scale 0.8; `opposite` adds yaw -45
degrees, pitch +15 degrees and distance scale 0.65. Rotations use the free
camera's stage-up-axis convention. Each view reuses the Qt navigation routes for
Camera Light ON/OFF and linear/sRGB output, and starting views must have
distinct images. Authored scene lights are disabled to isolate the camera
source. Immediate frames must render once, match their settled reference and
replayed camera, preserve draw counts, and avoid geometry fetch/upload or
pipeline creation. Camera-light and output toggles must visibly change the
starting image at every viewpoint.

The `merlin-usdview-forward-scene/v2` report records the view recipe and
initial camera matrix, source/root-layer SHA-256, mesh count, coverage,
diagnostics and full-frame stored-PNG RGB saturation fractions
(`clipped_pixel_fraction`). These count any RGB channel equal to 255, including
unassociated translucent samples, and do not establish displayed-color
clipping. They are observations without a universal threshold for arbitrary
assets. Root-layer identity alone does not pin referenced layers or textures.
The asset is supplied locally; it is neither bundled nor downloaded.

### Scope of the motion gates

The motion, navigation and external scene gates edit an authored camera or
synchronously dispatch Qt events between captures. Framebuffer capture adds
readback and disk I/O, so these gates are neither timing gates nor evidence for
the absence of flicker at an unrestricted navigation rate. Three relative
views do not establish interior scene coverage.

### Cross-backend comparison

An offline comparator checks independently captured Vulkan and Metal reports
without requiring either machine to run the other backend. It requires 20
native center pairs covering the seven light/camera phases and three normal-only
environment phases within one RGBA8 step, and 24 host center pairs covering
linear/sRGB, color/white, ON/OFF and static/moving/restored poses within two
steps, per presentation mode. It rejects stale output profiles,
duplicate/missing phases, saturation, uploads and pipeline creation. Center
parity does not establish full-image backend rasterization parity, broader
scene or material parity, or custom-environment parity. The comparator's
GPU-free boundary tests are not backend execution evidence.

## Evidence

Evidence captured for these gates is available in the
[v0.16.2 release record](../releases/v0.16.2.md) and in the delivery history:
[independent backend validation and closure](../reports/delivery-history.md#v0162-independent-backend-validation-and-closure--2026-10-12),
[refreshed Metal default output](../reports/delivery-history.md#refreshed-metal-default-output-validation--2026-10-11),
[default output alignment](../reports/delivery-history.md#forward-default-output-alignment--2026-10-11),
[Metal host lighting and navigation](../reports/delivery-history.md#metal-forward-host-lighting-and-navigation--2026-10-11),
[native Metal lighting](../reports/delivery-history.md#native-metal-forward-lighting--2026-10-11),
[Kitchen starting viewpoints](../reports/delivery-history.md#forward-kitchen-starting-viewpoints--2026-10-11),
[external Kitchen navigation](../reports/delivery-history.md#forward-external-kitchen-navigation--2026-10-11),
[Qt mouse navigation](../reports/delivery-history.md#forward-qt-mouse-navigation--2026-10-11),
[first-frame camera sweep](../reports/delivery-history.md#forward-first-frame-camera-sweep--2026-10-10),
[Camera Light fixtures](../reports/delivery-history.md#forward-camera-light-fixtures--2026-10-10) and
[directional identity and Hydra light energy](../reports/delivery-history.md#forward-directional-identity-and-hydra-light-energy--2026-10-10).
