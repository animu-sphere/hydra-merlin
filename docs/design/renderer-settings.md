# Versioned renderer settings

**Status:** v1 host-neutral contract

**Last reviewed:** 2026-08-09

`merlin::render::RendererSettings` is the configuration vocabulary shared by
native viewport, Hydra/DCC adapters, headless tools, and renderer backends. It
defines renderer meanings only: no OpenUSD token, DCC setting object, Vulkan
enum, Metal object, or UI widget crosses this boundary.

## v1 fields

| Field | v1 meaning | Default |
| --- | --- | --- |
| `schema_version` | Exact settings ABI version | `1` |
| `backend` | Automatic, Vulkan, or Metal selection request | `automatic` |
| `presentation_mode` | Automatic, offscreen, native-window, or host delivery | `automatic` |
| `render_path` | Conventional Forward or independently gated experimental Visibility | `forward` |
| `aov` | Selected renderer output | `color` |
| `lighting_mode` | Diagnostic, environment, or authored-light policy | `diagnostic` |
| `exposure_ev` | Exposure compensation in EV, bounded to `[-32, 32]` | `0` |
| `tone_mapping` | None, Reinhard, or ACES vocabulary | `none` |
| `alpha_policy` | Opaque, mask, or blend policy | `opaque` |
| `debug_view` | None, color, depth, primitive ID, instance ID, or normal | `none` |
| `validation` | Request validation for a backend created with validation support | `false` |
| `telemetry` | Off, basic, or detailed collection policy | `basic` |

Representability is not a capability claim. For example, v1 names the
experimental Visibility path so settings can remain stable while that path is
developed, but every current backend rejects it until a selectable capability
is added to `RendererCapabilities`.

## Validation and application

Hosts call `ValidateRendererSettings` before changing applied state. Validation
first rejects an unknown schema, invalid enum value, or non-finite/out-of-range
exposure. When selected-backend capabilities are supplied, it also rejects an
explicit backend mismatch, unavailable external presentation, an unsupported
experimental path, or validation that was not enabled at backend creation.

Every rejection has a stable `renderer-settings.*` code plus a human-readable
message. A rejected request leaves the previously applied settings and revision
unchanged. The development viewport uses this common validator before its
viewport-specific clear-color, continuous-readback, and AOV-inspection checks;
the same feedback enters the host-neutral diagnostic history.

Settings versioning does not silently coerce an unknown newer schema to v1.
Adapters may translate a host's own configuration into v1, but must preserve an
explicit rejected/fallback result for values that cannot be represented or
executed.
