# Roadmap

The roadmap contains only **incomplete** work. Shipped work belongs in the
[changelog](../../CHANGELOG.md) and, when more detail is useful, optional
[release records](../releases/). Completed pre-release roadmap detail belongs in
the [delivery history](../reports/delivery-history.md), and implementation
evidence and design rationale belong in [reports](../reports/) and
[design](../design/).

Legend: 🚧 in progress · ⬜ not started

| Document | Contents |
| --- | --- |
| [current.md](current.md) | The next release milestone and active carry-over work. |
| [backlog.md](backlog.md) | Ordered releases after the active milestone and cross-cutting open work. |

The active milestone is v0.5.0 release hardening: its host-neutral MaterialIR,
basic textured shading, and usdview slice are implemented, while final
capability evidence and unfinished release-integrity/diagnostic work remain.
The ordered ladder next measures Hydra presentation costs, adds MaterialX,
delivers viewport essentials, and evaluates lower-copy presentation only when
the measurements justify it.

When a version ships, its completed scope is captured in the changelog and
removed from the roadmap. The roadmap is not a second changelog.

## Quality bar

Every release must preserve these properties:

- Core public APIs remain independent of OpenUSD, Hydra, Vulkan, Qt, and DCC SDKs.
- Core-only, Vulkan, and enabled host configurations build without warnings.
- CTest distinguishes a product failure from a missing optional capability.
- Deterministic scene input produces deterministic extraction and image metadata.
- Vulkan validation reports renderer-owned warnings and errors as failures while
  keeping unrelated loader or host diagnostics distinguishable.
- Build-tree and install-tree consumers exercise the same public contracts.
- Resource lifetime is safe across frame latency, resize, and removal.
- New performance-sensitive work adds counters or benchmark evidence; FPS alone
  is not accepted as the performance contract.
- Unsupported inputs return an actionable diagnostic or an explicit fallback.

## Sequencing principles

- **MaterialIR before MaterialX.** Hydra networks, MaterialX documents, and
  future sources normalize into one host-neutral material boundary before
  shader generation.
- **Measure before optimizing.** Hydra Sync, scene normalization, command
  recording, GPU work, readback, host upload, and presentation must be separated
  before selecting an optimization.
- **Keep the reference path.** Tier 0 CPU readback remains available for
  headless execution, image comparison, debugging, and unsupported-host
  fallback even when a lower-copy path exists.
- **Keep integrations outside Core.** DCC packages own discovery, environment,
  settings UI, package metadata, and host smoke tests; Core owns no DCC SDK
  dependency.

## Priority bands

| Priority | Direction |
| --- | --- |
| P0 | v0.5.0 release integrity, structured diagnostics/errors, GPU capability CI, and OpenUSD compatibility checks |
| P1 | Hydra performance observability and comparable host-stage evidence |
| P2 | MaterialX MVP and selection/picking |
| P3 | Shadows, dome light, measured culling, DCC integration, and justified low-copy presentation |
| P4 | Bindless, indirect draw, GPU-driven rendering, meshlets, mesh shaders, and advanced transparency only with benchmark evidence |
