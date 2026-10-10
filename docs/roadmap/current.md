# Current

Active and immediately next work lives here. Shipped changes are recorded in
the [changelog](../../CHANGELOG.md) and [v0.16.1 release record](../releases/v0.16.1.md);
current support claims live in the [support matrix](../reference/support-matrix.md).

## Active — Forward lighting quality

The shared directional-input boundary and Hydra radiance conversion are implemented; their
contracts and current image limitations are in the
[Forward lighting design](../design/forward-lighting.md).

- [ ] Establish focused Camera Light ON/OFF and static/moving differential
  fixtures before changing ambient/direct balance, linear/sRGB conversion,
  exposure or tone mapping. Cover native, Tier 0 and Hgi presentation and
  handwritten/generated material behavior.
- [ ] Validate the shared directional selection on native Metal, then align
  Vulkan/Metal lighting and output transforms with declared image tolerances.

The broader Forward quality exit and dependencies remain in the
[backlog](backlog.md#next-cycle-onward--forward-lighting-and-shading-quality).

Historical producer-session evidence and remaining OpenStrata integration asks
are in the [OST recheck](../reports/ost/13-2026-09-26-v0.23.8-recheck-v0.24.0-asks.md).
