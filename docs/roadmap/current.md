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
changed ranges, and safe partial upload. This milestone extends those contracts
through persistent RenderWorld snapshots and bindless GPU residency.

#### Scope

- Add persistent bindless resource tables with generation-safe stable handles
  and descriptor slots, safe fallback texture slots, sampler deduplication, and
  a conventional descriptor fallback when required capabilities are absent.
- Add dirty queues, changed ranges, incremental snapshots, structural sharing,
  and completion-safe delayed retirement.
- Add persistent Mesh and Gaussian arenas, a mapped upload ring, asynchronous
  transfer, and explicit VRAM-budget evidence.
- Probe and report descriptor-indexing features, limits, and selected fallback
  behavior in versioned capability artifacts.

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

- Enroll or confirm a repository- or organization-scoped Windows x64 runner with the existing
  `vulkan-1.4` label so the manual workflow becomes continuing GPU evidence.
- Retain dependency/runtime provenance, validation logs, expected/actual/diff
  images, benchmark output, Hydra discovery, RenderBuffer, and usdview results
  as comparable artifacts rather than reducing capability jobs to a binary
  pass/fail signal.
