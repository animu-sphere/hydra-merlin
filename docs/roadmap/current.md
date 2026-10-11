# Current

Active and immediately next work lives here. Shipped changes are recorded in
the [changelog](../../CHANGELOG.md) and [v0.16.1 release record](../releases/v0.16.1.md);
current support claims live in the [support matrix](../reference/support-matrix.md).

## Active — v0.16.2 Forward output alignment

The shared directional-input boundary, Hydra radiance conversion and common
default SH/Reinhard output profile are implemented. Their contracts and remaining
validation requirements are in the
[Forward lighting design](../design/forward-lighting.md).

- [ ] Validate the updated Metal output in Debug/Release with native analytic
  centers, three normal-only environment samples, repeated/restored AOVs,
  resource counters and API/shader validation. Earlier Apple M3 captures used
  constant ambient and uncompressed output; they do not validate the new profile.
- [ ] Run the updated Metal native/Tier 0 and Tier 0/HgiMetal lighting, camera
  sweep and Qt navigation gates. White OFF must retain headroom in linear/sRGB;
  saturation is no longer an accepted diagnostic result.
- [ ] Compare independently captured Vulkan/Metal default-output reports using
  `tests/compare_forward_backends.py`: 20 native center pairs within 1/255 and
  24 host center pairs within 2/255 for each presentation mode. Preserve the
  independent full-image host, exact replay/AOV and installed-runtime gates.
- [ ] Finish release preparation and hosted checks, then publish v0.16.2 only
  after the changed Metal profile and actual backend comparisons pass.

Unrestricted navigation, interior Kitchen views, generated host material
ingestion and the broader Forward quality exit remain in the
[backlog](backlog.md#next-cycle-onward--forward-lighting-and-shading-quality).

Historical producer-session evidence and remaining OpenStrata integration asks
are in the [OST recheck](../reports/ost/13-2026-09-26-v0.23.8-recheck-v0.24.0-asks.md).
