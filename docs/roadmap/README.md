# Roadmap

The roadmap contains only **incomplete** work. Shipped work belongs in
[release records](../releases/), completed pre-release roadmap detail belongs in
the [delivery history](../reports/delivery-history.md), and implementation
evidence and design rationale belong in [reports](../reports/) and
[design](../design/).

Legend: 🚧 in progress · ⬜ not started

| Document | Contents |
| --- | --- |
| [current.md](current.md) | The next release milestone and active carry-over work. |
| [backlog.md](backlog.md) | Ordered but unscheduled releases and cross-cutting open work. |

The next milestone is the reproducible development baseline for v0.1.0. Later
releases move from resource-granular GPU scene updates through practical Hydra
mesh support, execution lifetime, materials, and DCC integration.

When a version ships, its completed scope moves to a release record and is
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
