# Current

Active and immediately next work lives here. Shipped changes are recorded in
the [changelog](../../CHANGELOG.md) and [v0.16.0 release record](../releases/v0.16.0.md);
current support claims live in the [support matrix](../reference/support-matrix.md).

## v0.16.x follow-up — remaining hardware and host coverage

The v0.16.0 GPU-driven foundation is shipped within its documented validated
scope. Complete the remaining quality and coverage work before expanding the
claims. See the [GPU-driven design](../design/gpu-driven-rendering.md),
[Gaussian pipeline design](../design/gaussian-rendering.md), and
[Metal execution plan](../design/metal-gaussian-execution.md).

- [ ] Broaden Metal Gaussian execution evidence beyond Apple M3 and tune tile
  work on representative scenes. Record total GPU cost as well as raster savings;
  current 65K/1M binning and sorting can outweigh the raster improvement.
- [ ] Extend same-device/host measurements to other supported GPUs with
  controlled clocks/power before establishing hardware timing gates.
- [ ] Run the new Qt mouse-click/axis-overlay regressions on HgiVulkan with
  OpenUSD 26.05 and 26.08. Apple M3/OpenUSD 26.08 Tier 0 and HgiMetal evidence
  is archived in the [delivery history](../reports/delivery-history.md#v016x-click-picking-overlay-parity-and-ci-separation).
  The Windows GPU runner was offline during this follow-up.

## Active carry-over — validation gates

- [ ] Keep SPIR-V validation, Metal-target compilation, Hydra compilation, and
  runtime coverage as distinct gates. Hosted GPU-free MaterialX
  generation and consumers now have passing Windows/Linux Debug/Release evidence.
  Explicit SPIR-V validation and independent hosted Hydra build coverage remain
  to be expanded; GPU timing is not a universal PR gate.

Linux Vulkan work is deferred to the [backlog](backlog.md#cross-cutting-planned-work).

Historical producer-session evidence and remaining OpenStrata integration asks
are in the [OST recheck](../reports/ost/13-2026-09-26-v0.23.8-recheck-v0.24.0-asks.md).
