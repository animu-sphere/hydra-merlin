# Current

Active and immediately next work only. Shipped changes are in the
[changelog](../../CHANGELOG.md). Current support is in the
[support matrix](../reference/support-matrix.md).

Deferred work and future milestones are in the [backlog](backlog.md).

## v0.18.0 — High-resolution Gaussian tiles

- Resolve the 4K static Vulkan CPU/GPU sorted-stream `instanceId` mismatch
  before using that case as a passing high-resolution reference. See the
  [flat tile measurements](../reports/delivery-history.md#vulkan-flat-gaussian-tiles-at-4k-and-8k--2026-11-28).
- Evaluate bounded two-level tiling against flat Exact and Adaptive with the
  same scenes and camera sequences. Require safe overflow fallback and a
  repeatable 4K/8K p95/p99 benefit. Profile 1080p and 1440p to check for
  small-viewport regressions. See the
  [Gaussian design](../design/gaussian-rendering.md#high-resolution-hierarchical-tiles).
