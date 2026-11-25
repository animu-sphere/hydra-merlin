# Current

Active and immediately next work only. Shipped changes are in the
[changelog](../../CHANGELOG.md). Current support is in the
[support matrix](../reference/support-matrix.md).

## CPU Catmull–Clark — Metal validation

- On Apple M3, run the [subdivision fixtures](../../adapters/merlin-hydra2/tests/fixtures/README.md#subdivision)
  in Hydra/usdview and compare native Mesh output with the Windows Vulkan
  baseline. Include normals, tangent frames and indexed UV seams.
- Measure initial refinement, points-only updates, working memory, uploads
  and steady-state frame time against the polygon `none` path on the
  representative scenes. Add Metal evidence to the
  [support matrix](../reference/support-matrix.md#subdivision).
