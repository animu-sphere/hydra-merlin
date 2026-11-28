# Current

Active and immediately next work only. Shipped changes are in the
[changelog](../../CHANGELOG.md). Current support is in the
[support matrix](../reference/support-matrix.md).

Deferred work and future milestones are in the [backlog](backlog.md).

## Forward MaterialX transmission

- Implement Metal opaque scene sampling for pawn-top refraction and verify it
  with the native HDR backdrop and Open Chess Set gates. See the
  [transmission profile](../design/forward-lighting.md#dielectric-transmission).

## v0.18.0 — High-resolution Gaussian tiles

- Evaluate bounded two-level tiling against flat Exact and Adaptive with the
  same scenes and camera sequences. Require safe overflow fallback and a
  repeatable 4K/8K p95/p99 benefit. Profile 1080p and 1440p to check for
  small-viewport regressions. See the
  [Gaussian design](../design/gaussian-rendering.md#high-resolution-hierarchical-tiles).
