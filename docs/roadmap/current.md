# Current

Only incomplete work for the next release milestone and active carry-over is
listed here. Completed pre-release detail is retained in the
[delivery history](../reports/delivery-history.md); shipped versions will be
recorded in the [changelog](../../CHANGELOG.md).

## Next milestone

### 🚧 v0.7.0 — Persistent RenderWorld and GPU residency

**Objective:** make static and localized Mesh/Gaussian scene work scale with
changed resources rather than total prim count while keeping resource identity
stable across frames.

v0.6.0 established locator-aware Hydra ingestion, resource-granular revisions,
changed ranges, and safe partial upload. v0.7.0 work has since completed
persistent snapshot construction, descriptor-indexing negotiation, and the
Vulkan-facing texture/sampler residency foundation: finite generation-checked
slots, reserved fallback images, dirty-only global descriptor writes,
deduplicated samplers, completion-safe replacement, and benchmark telemetry.
Negotiated devices now activate those tables through a non-uniform-indexed
Forward path with exact conventional-path image parity; warmed static frames do
zero descriptor allocation or update. Persistent vertex/index arenas and the
mapped geometry-upload ring now expose capacity, stable range reuse, growth,
fragmentation, retirement, and resource-class staged-byte evidence. Dedicated
transfer families now run uploads asynchronously with timeline synchronization
and explicit image ownership transitions, while device-local allocations obey
the configured/driver VRAM budget and retain current/peak/exhaustion evidence.
The remaining work completes bindless release and scale evidence on the
supported GPU profiles.

#### 1. Bindless release evidence

- Retain conventional/bindless parity across the release image corpus.
- Record explicit automatic and forced-conventional selection artifacts on the
  supported GPU profiles, including actionable capacity exhaustion behavior.

#### 2. Validation and release evidence

- Retain reserved-image, dirty-write, partially-bound, non-uniform indexing,
  in-flight replacement, conventional/bindless image parity, one-million-prim
  localized-edit scaling, steady-state zero-work, fallback selection,
  asynchronous/single-queue transfer selection, and VRAM evidence.

#### Scope boundary

v0.7.0 owns the common arena, upload, retirement, descriptor, and memory-budget
infrastructure needed by Mesh and future Gaussian resources. Host-neutral
`GaussianResource`, standard Hydra Gaussian ingestion, and native Gaussian
rendering remain v0.14.0 work; this milestone does not introduce a renderer-
specific Gaussian schema or file parser.

#### Exit criteria

- Static snapshot cost no longer scales with total prim count, and changing 100
  resources in a one-million-prim scene scales approximately with those
  changes.
- Unchanged GPU resource addresses, handles, and descriptor indices remain
  stable; stale generations are detectable and texture replacement is safe
  across frames in flight.
- Bindless Forward matches the reference image, while descriptor work scales
  with changed resources rather than materials or draws.
- Steady state performs zero upload, allocation, descriptor allocation/update,
  shader compilation, pipeline creation, and CPU wait for GPU.

## Active carry-over

- Enroll or confirm an organization-scoped Windows x64 runner with the existing
  `vulkan-1.4` label so the manual workflow becomes continuing GPU evidence.
  The repository-scoped runner API reports zero enrolled runners and the
  capability workflow has no prior runs as of 2026-07-16; organization runner
  visibility still requires an administrator confirmation.
- Retain dependency/runtime provenance, validation logs, expected/actual/diff
  images, benchmark output, Hydra discovery, RenderBuffer, and usdview results
  as comparable artifacts rather than reducing capability jobs to a binary
  pass/fail signal.
