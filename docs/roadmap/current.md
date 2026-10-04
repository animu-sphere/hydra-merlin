# Current

Active and immediately next work lives here. Shipped changes are recorded in
the [changelog](../../CHANGELOG.md) and [v0.16.0 release record](../releases/v0.16.0.md);
current support claims live in the [support matrix](../reference/support-matrix.md).

## Next — validation gates

- [ ] Separate required hosted builds, shader/ABI checks, MaterialX generation,
  and install-tree consumers from scheduled runtime/capability checks and
  hardware-specific performance evidence. GPU timing is not a universal PR
  gate until runner variance is controlled.
- [ ] Add independent Metal-target and Hydra compilation gates alongside the
  Core, MaterialX generation and Linux Vulkan validation workflows, preserving
  the distinction between compilation and actual host/device execution.

Historical producer-session evidence and remaining OpenStrata integration asks
are in the [OST recheck](../reports/ost/13-2026-09-26-v0.23.8-recheck-v0.24.0-asks.md).
