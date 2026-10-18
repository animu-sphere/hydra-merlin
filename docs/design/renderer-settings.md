# Versioned renderer settings

**Last reviewed:** 2026-10-18

`merlin::render::RendererSettings` is the configuration vocabulary shared by
native viewport, Hydra/DCC adapters, headless tools, and renderer backends. It
defines renderer meanings only: no OpenUSD token, DCC setting object, Vulkan
enum, Metal object, or UI widget crosses this boundary.

## v3 fields

| Field | v3 meaning | Default |
| --- | --- | --- |
| `schema_version` | Exact settings ABI version | `3` |
| `backend` | Automatic, Vulkan, or Metal selection request | `automatic` |
| `presentation_mode` | Automatic, offscreen, native-window, or host delivery | `automatic` |
| `render_path` | Conventional Forward or independently gated experimental Visibility | `forward` |
| `gpu_driven_indexed` | Forward indexed submission policy, visibility mask, and independent visibility-mask/frustum culling controls | `disabled`, all bits visible, both culling stages enabled |
| `gpu_driven_gaussian` | Gaussian execution policy: GPU preparation, sorting, and raster instead of the CPU-sorted stream, and a raster path that draws the sorted stream or composites screen tiles in compute | `disabled`, `sorted-stream` |
| `aov` | Selected renderer output | `color` |
| `lighting_mode` | Diagnostic, environment, or authored-light policy | `diagnostic` |
| `exposure_ev` | Lit Forward energy multiplier `2^EV`, bounded to `[-32, 32]` | `0` |
| `tone_mapping` | None or Reinhard for lit Forward; ACES reserved and rejected | `reinhard` |
| `alpha_policy` | Opaque, mask, or blend policy | `opaque` |
| `debug_view` | None, color, depth, primitive ID, instance ID, normal (final `Ns`), geometric normal, base shading normal, front-facing, tangent, or bitangent | `none` |
| `validation` | Request validation for a backend created with validation support | `false` |
| `telemetry` | Off, basic, or detailed collection policy | `basic` |

Representability is not a capability claim. Version 2 retains the v1
experimental Visibility vocabulary and adds GPU-driven indexed Forward as a
submission policy rather than a separate render path. `prefer` is accepted on
all backends and records an explicit conventional-Forward fallback when the
request cannot select the accelerated path. `require` is accepted only when
the selected backend reports the device features and configured persistent GPU
Scene boundary needed by that path. The visibility mask and culling switches
are forwarded unchanged when GPU-driven execution is selected.

Version 3 adds GPU-driven Gaussian execution as one policy over the whole
Gaussian chain rather than per-stage backend switches. `prefer` records an
explicit fallback count for every stage the backend cannot select and keeps
the CPU-sorted stream; `require` is accepted only when the selected backend
reports `gpu_driven_gaussian`. The `tiled` raster path may still fall back per
frame, to the GPU-sorted draws, when the device cannot use the targets as
storage images or when tile binning overflows.

Every current backend still rejects experimental Visibility until a selectable
capability is added to `RendererCapabilities`. The same rule applies to values
whose execution wiring is still future work: with capabilities supplied, the
current profile accepts Color AOV, Diagnostic lighting, finite EV in `[-32, 32]`,
None or Reinhard tone mapping, Opaque global alpha, no debug view or a Forward
surface debug view, and Basic telemetry. Other named values are
rejected rather than silently stored.

## Validation and application

Hosts call `ValidateRendererSettings` before changing applied state. Validation
first rejects an unknown schema, invalid enum value, or non-finite/out-of-range
exposure. When selected-backend capabilities are supplied, it also rejects an
explicit backend mismatch, unavailable external presentation, an unsupported
experimental path, unavailable required GPU-driven submission, an execution
setting not connected by the current profile, or validation state that differs
from backend creation.

Every rejection has a stable `renderer-settings.*` code plus a human-readable
message. A rejected request leaves the previously applied settings and revision
unchanged. The development viewport uses this common validator before its
viewport-specific clear-color, continuous-readback, and AOV-inspection checks;
the same feedback enters the host-neutral diagnostic history.

## Forward output settings

The reserved exposure/tone fields now execute through the shared
[Forward output contract](forward-lighting.md#output-transform). Default
`RendererSettings` explicitly names Reinhard, which was previously embedded in
lit shading even though the stored default said None. The existing v3 field
layout and vocabulary are retained. Exposure/tone mapping apply only to lit
Forward RGB; diagnostic colors, Gaussian and clear color keep their established
policies. `None` still writes a linear UNorm AOV and can clip; display encoding
follows the AOV in presentation. This is not complete color management.

The per-submission `OutputTransformSettings` is populated from the validated
renderer settings. It does not dirty snapshots, geometry or material variants.
Invalid EV and unavailable ACES reject in both host and backend validation;
rejected Hydra edits leave effective values and settings revisions unchanged.
`merlin:exposureEv` accepts float/double EV, while `merlin:toneMapping` accepts
`none`/`reinhard` strings or tokens. Both are listed in host setting descriptors.
The native viewport applies the same fields through its settings controls.

## Forward surface debug views

`normal`, `geometric-normal`, `shading-normal-base`, `front-facing`,
`tangent` and `bitangent` render the shared Forward
[surface context](surface-geometry.md#debug-views) as unlit RGB. They are per-submission `RenderRequest::debug_view` values, like the
output transform, and do not dirty snapshots, geometry or pipelines. Color,
depth and ID debug views remain representable but rejected. Hydra accepts
`merlin:debugView` as a string or token, lists it in the setting descriptors,
rejects other names with `renderer-settings.invalid-debug-view` or
`renderer-settings.debug-view-unsupported`, and keeps the effective view. The
native viewport applies the same field through its settings controls.

## Hydra render settings

The Hydra render delegate translates host render settings into the v3
Gaussian execution policy. Four keys edit the same policy:

| Key | Values | Default | Listed |
| --- | --- | --- | --- |
| `merlin:gpuDrivenGaussian:enabled` | bool: on unless the mode is `disabled`; turning it on selects `prefer` and keeps `require` | `false` | Yes |
| `merlin:gpuDrivenGaussian:tiled` | bool: `tiled` or `sorted-stream` | `false` | Yes |
| `merlin:gpuDrivenGaussian:mode` | `disabled`, `prefer`, `require` | `disabled` | No |
| `merlin:gpuDrivenGaussian:raster` | `sorted-stream`, `tiled` | `sorted-stream` | No |

The Gaussian descriptors list only the two flags, which
usdview shows as Hydra Settings menu checkboxes. usdview's settings dialog,
which it adds for any non-flag setting, re-sends every listed setting as it
was when the dialog opened; listing the names as well would let an untouched,
stale value undo an edited one. The names stay settable, since `require` has
no flag, and are read before the flags when a host passes both at creation.

A policy name outside the vocabulary, a name that is not a string or token,
or a flag that is not a bool is rejected with the same `renderer-settings.*`
code the validator uses. Other
values are bound to the selected backend through `ValidateRendererSettings`
with only the Gaussian fields set: immediately when the backend exists, and
otherwise before the first frame, since the plugin creates its backend
lazily. Every rejection is a Rejected diagnostic whose source is the setting
key and whose recovery names the policy that stays applied, and
`GetRenderSetting` reports that effective policy rather than the rejected
request. An unchanged policy is neither revalidated nor reported again, because
hosts such as usdview re-send every setting when one changes. Settings the host
passes when it creates the delegate go through the same path, and the
development viewport's `SetGpuDrivenGaussianSettings` shares it too.

Settings versioning does not silently coerce an unknown schema to v3. Schema v2
added the GPU-driven indexed submission policy and schema v3 the GPU-driven
Gaussian execution policy; each bumped the backend capability contract.
Backend contract version 4 adds the per-request Forward output transform,
version 5 the per-request Forward surface debug view and version 6 the
tangent views with the tangent-carrying vertex layout;
the renderer-settings field layout remains v3. Adapters may translate a host's own configuration into v3, but
must preserve an explicit rejected/fallback result for values that cannot be represented or
executed.
