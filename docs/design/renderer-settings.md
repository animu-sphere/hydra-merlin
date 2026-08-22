# Versioned renderer settings

**Status:** v2 host-neutral contract

**Last reviewed:** 2026-08-22

`merlin::render::RendererSettings` is the configuration vocabulary shared by
native viewport, Hydra/DCC adapters, headless tools, and renderer backends. It
defines renderer meanings only: no OpenUSD token, DCC setting object, Vulkan
enum, Metal object, or UI widget crosses this boundary.

## v2 fields

| Field | v2 meaning | Default |
| --- | --- | --- |
| `schema_version` | Exact settings ABI version | `2` |
| `backend` | Automatic, Vulkan, or Metal selection request | `automatic` |
| `presentation_mode` | Automatic, offscreen, native-window, or host delivery | `automatic` |
| `render_path` | Conventional Forward or independently gated experimental Visibility | `forward` |
| `gpu_driven_indexed` | Forward indexed submission policy, visibility mask, and independent visibility-mask/frustum culling controls | `disabled`, all bits visible, both culling stages enabled |
| `aov` | Selected renderer output | `color` |
| `lighting_mode` | Diagnostic, environment, or authored-light policy | `diagnostic` |
| `exposure_ev` | Exposure compensation in EV, bounded to `[-32, 32]` | `0` |
| `tone_mapping` | None, Reinhard, or ACES vocabulary | `none` |
| `alpha_policy` | Opaque, mask, or blend policy | `opaque` |
| `debug_view` | None, color, depth, primitive ID, instance ID, or normal | `none` |
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

Every current backend still rejects experimental Visibility until a selectable
capability is added to `RendererCapabilities`. The same rule applies to values
whose execution wiring is still future work: with capabilities supplied, the
current profile accepts Color AOV, Diagnostic lighting, 0 EV, no tone mapping,
Opaque global alpha, no debug view, and Basic telemetry. Other named values are
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

Settings versioning does not silently coerce an unknown schema to v2. Schema v2
adds the GPU-driven indexed submission policy and bumps the backend capability
contract to version 2. Adapters may translate a host's own configuration into
v2, but must preserve an
explicit rejected/fallback result for values that cannot be represented or
executed.
