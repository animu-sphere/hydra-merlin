# Current

Active and immediately next work only. Shipped changes are in the
[changelog](../../CHANGELOG.md); current support is in the
[support matrix](../reference/support-matrix.md).

## v0.17.0 — Gaussian contribution and adaptive bounds

- Extend paired Exact/adaptive sequences to another Metal GPU and additional
  Vulkan profiles as hardware becomes available. Repeat timing comparisons
  with controlled clocks and power before establishing hardware timing gates.

Keep `Exact` as the default and conservative Adaptive as opt-in. The tested
`2/255` and `4/255` floors exceeded the image and picking contract. See the
[milestone scope](backlog.md#v0170--contribution-aware-gaussian-culling-and-adaptive-bounds)
and [paired sequence evidence](../reports/delivery-history.md#v0170-paired-gaussian-sequences-and-quality-thresholds--2026-11-23).

## v0.16.x — Forward renderer stabilization

v0.16.x establishes a stable architectural, semantic and quality baseline
before v0.17.0 begins the next Gaussian optimization cycle. The
[v0.16.2 release](../releases/v0.16.2.md) is the Forward semantic baseline:
later v0.16.x refactors preserve its rendered meaning unless a change is
explicitly part of an output-transform or quality milestone. Core owns
rendering semantics; backends own execution.

### Exit

v0.16.x closes when this statement is substantially true:

> Forward rendering has stable cross-backend semantics, an explicit
> output-transform policy, a stable Mesh surface shading contract for normals,
> tangent frames and normal mapping, maintainable backend/shader architecture,
> representative real-scene quality evidence, and no known unexplained
> Vulkan/Metal contract divergence in the supported material subset.

Every desirable shading feature is not required before v0.17.0.

### Out of scope for v0.16.x

Unless required to fix a correctness regression, these belong to the
[backlog](backlog.md): contribution-aware Gaussian culling, hierarchical
Gaussian tiles, temporal Gaussian reuse, Gaussian LOD/compression, streaming
and out-of-core execution, production meshlets, Hi-Z, Mesh Shader, Visibility
Buffer productionization, broad new MaterialX graph coverage, a complete shadow
system, displacement/parallax and other advanced surface detail, and major new
renderer architecture experiments.
