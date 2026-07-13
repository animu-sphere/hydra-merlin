# Current

Only incomplete work for the next release milestone and active carry-over is
listed here. Completed pre-release detail is retained in the
[delivery history](../reports/delivery-history.md); shipped versions will be
recorded in the [changelog](../../CHANGELOG.md).

## Next milestone

### 🚧 v0.5.1 — Performance foundation

**Objective:** make first-frame, steady-state, and edit latency attributable
across Hydra, Merlin Core, Vulkan execution, readback, host upload, and
presentation before choosing the next optimization.

v0.5.0 already provides deterministic reference-path benchmark JSON, CPU
scopes, structural counters, and first-frame/steady/edit fixtures. This
milestone extends that foundation through the Hydra presentation path and makes
the resulting artifacts comparable between runs.

#### Scope

- Record Hydra Sync and scene-index processing, RenderWorld update, snapshot
  extraction, GPU scene update, command recording, queue submission, GPU
  execution, readback, RenderBuffer resolve/map, CPU-to-Hgi upload, host
  composite, and presentation time as separately attributable stages.
- Record upload/readback bytes, requested and generated AOVs, descriptor and
  pipeline work, waits, maps, resolves, host uploads, draw count, visible
  primitive count, and GPU memory where the owning layer exposes them.
- Add fixed static, camera-only, transform, visibility, material, topology,
  1M-triangle, 10k-mesh, 1k-instance, AOV-combination, and 4K fixtures.
- Export versioned JSON or CSV with build/machine metadata and first-frame,
  median, p95, p99, maximum, and frame-hitch summaries.
- Retain dependency/runtime provenance, validation logs, images, benchmark
  output, plugin discovery, RenderBuffer, and usdview results as comparable
  capability artifacts.
- Detect stable structural performance regressions in CI; timing thresholds are
  enabled only on controlled hardware.

#### Exit criteria

- First frame and steady state are reported separately and CPU/GPU timelines can
  be correlated.
- A camera-only run proves zero geometry/topology/primvar fetch and upload.
- A static steady-state run proves zero resource allocation, shader compile,
  pipeline creation, and geometry upload.
- Color-only requests prove zero depth/ID CPU readback.
- A regression report identifies the limiting stage rather than reporting only
  aggregate FPS.
- The Windows Vulkan/Hydra capability workflow retains comparable evidence; a
  missing runner remains distinguishable from a renderer failure.

## Active carry-over

- Enroll or confirm a repository- or organization-scoped Windows x64 runner with the existing
  `vulkan-1.4` label so the manual workflow becomes continuing GPU evidence.
- Retain dependency/runtime provenance, validation logs, expected/actual/diff
  images, benchmark output, Hydra discovery, RenderBuffer, and usdview results
  as comparable artifacts rather than reducing capability jobs to a binary
  pass/fail signal.
- Add a host-neutral diagnostic sink and actionable OpenUSD build/runtime
  compatibility checks before the first DCC integration milestone.
