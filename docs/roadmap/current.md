# Current

Only incomplete work for the next release milestone and active carry-over is
listed here. Completed pre-release detail is retained in the
[delivery history](../reports/delivery-history.md); shipped versions will be
recorded in the [changelog](../../CHANGELOG.md).

## Next milestone

### 🚧 v0.6.0 — Incremental Hydra sync

**Objective:** make camera and localized scene edits scale with the changed
data instead of the total Hydra scene while preserving the v0.5.1 performance
evidence and host-neutral Core boundary.

v0.5.1 separates delegate, scene-index, RenderWorld, extraction, Vulkan,
readback, host upload, composite, and presentation costs. This milestone uses
that evidence to remove redundant fetch, normalization, snapshot, and upload
work from common Hydra changes.

#### Scope

- Separate dirty locator/bit processing and retain persistent
  USD-path-to-Merlin state across Sync calls.
- Add transform-only, visibility-only, and material-parameter-only fast paths.
- Cache primvar descriptors, normalized/indexed primvars, and triangulation;
  track changed ranges so unchanged mesh payload is neither fetched nor
  uploaded.
- Preserve distinct topology, points, primvar, material-partition, instance,
  transform, and visibility revisions so later GPU Scene and derived-meshlet
  invalidation does not require Hydra state inside the renderer backend.
- Emit actionable diagnostics for unsupported or lossy Hydra data instead of
  silently rebuilding or dropping it.
- Study ingestion of the existing OpenUSD Gaussian representation through
  Hydra without adding a renderer-specific USD schema, prim type, attribute
  contract, or direct PLY/SPLAT parser.
- Preserve the v0.5.1 stage reports, structural gates, and capability artifacts
  as the optimization evidence.

#### Exit criteria

- Camera-only updates perform zero geometry, topology, primvar, and
  Gaussian-attribute fetch or upload and zero pipeline creation.
- Transform-only, visibility-only, and material-parameter-only changes avoid
  unrelated mesh fetch, normalization, triangulation, and upload.
- Cached primvar/topology state is invalidated by the relevant dirty locators or
  bits and remains correct across removal and re-addition.
- Unsupported data produces versioned, actionable diagnostics with a named
  fallback or rejection.
- The Gaussian study records a supported integration boundary and evidence;
  any adapter remains outside the renderer-neutral Core model.

## Active carry-over

- Enroll or confirm a repository- or organization-scoped Windows x64 runner with the existing
  `vulkan-1.4` label so the manual workflow becomes continuing GPU evidence.
- Retain dependency/runtime provenance, validation logs, expected/actual/diff
  images, benchmark output, Hydra discovery, RenderBuffer, and usdview results
  as comparable artifacts rather than reducing capability jobs to a binary
  pass/fail signal.
- Add a host-neutral diagnostic sink and actionable OpenUSD build/runtime
  compatibility checks before the first DCC integration milestone.
