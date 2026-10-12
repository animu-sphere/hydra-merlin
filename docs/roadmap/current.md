# Current

Active and immediately next work lives here. Shipped changes are recorded in
the [changelog](../../CHANGELOG.md) and [v0.16.2 release record](../releases/v0.16.2.md);
current support claims live in the [support matrix](../reference/support-matrix.md).

## Next — broader Forward lighting and shading quality

v0.16.2 closes stable directional selection, Hydra camera-light energy and the
shared default SH/Reinhard output with independent Vulkan/Metal native and
host center comparisons. The next Forward work extends that bounded evidence:

- [ ] Cover interior Kitchen views and broader scene/material lighting quality
  with native, Tier 0 and Hgi comparisons for Camera Light ON/OFF and linear/sRGB.
- [ ] Validate unrestricted interactive navigation separately from bounded,
  synchronous first-frame sweeps and Qt capture replay.
- [ ] Define exposure/tone-mapping selection and extend lighting beyond the
  current single unshadowed directional source plus packaged SH environment.
- [ ] Extend generated Metal material ingestion and representative hardware/SDK
  evidence before broadening support or establishing controlled timing gates.

The [Forward lighting design](../design/forward-lighting.md) defines current
contracts and limits; the [backlog](backlog.md#next-cycle-onward--forward-lighting-and-shading-quality)
retains the broader quality exit and subsequent milestones.

Historical producer-session evidence and remaining OpenStrata integration asks
are in the [OST recheck](../reports/ost/13-2026-09-26-v0.23.8-recheck-v0.24.0-asks.md).
