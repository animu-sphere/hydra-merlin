# Forward lighting boundary

**Last reviewed:** 2026-10-10

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

The basic Vulkan shader uses the packaged diffuse SH environment as 0.15
ambient fill plus 0.85 of the selected Lambert direct term, then applies an
embedded Reinhard compression before the UNorm color AOV. Metal currently
uses constant 0.15 ambient fill plus the same 0.85 direct term without that
compression. These are current diagnostic profiles; shared directional input
does not establish cross-backend lighting-image parity. `RendererSettings`
still rejects nonzero exposure and selectable tone mapping.

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

The next slice must establish image/output parity. Differential fixtures must
cover Camera Light ON/OFF,
static/moving cameras, linear/sRGB boundaries, explicit exposure/tone mapping,
native/Tier 0/Hgi presentation and handwritten/generated materials. Broader
lighting and output parity remain gated by those fixtures and backend evidence.

## Regression evidence

The CPU contract test exercises actual RenderWorld/extractor table compaction,
retained snapshots, camera changes, light translation, zero intensity,
extreme/degenerate axes, invalid energy and selected-light removal. The Vulkan
material regression renders differently colored directional inputs, removes an
unselected Point record that moves the last table slot, and requires exact
color/depth/primId/instanceId parity with zero geometry upload and no pipeline
creation. Installed Core consumers compile and link the same helper. Metal
uses the common implementation; native Metal compilation/runtime validation
still requires an Apple host.

The GPU-free Hydra energy regression independently integrates a spherical
light cap at four angles and checks normalized size invariance, exposure and
diffuse scaling, zero/delta lights, overflow, underflow and malformed inputs.
An injected backend captures the actual Sprim-to-snapshot path, including
rotation, parameter-only edits, unchanged revisions, atomic rejection,
initial rejection/retry and removal. These tests verify input energy; they do
not establish the remaining usdview Kitchen image or native Metal claims.
