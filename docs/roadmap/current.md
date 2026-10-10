# Current

Active and immediately next work lives here. Shipped changes are recorded in
the [changelog](../../CHANGELOG.md) and [v0.16.1 release record](../releases/v0.16.1.md);
current support claims live in the [support matrix](../reference/support-matrix.md).

## Active — Forward lighting quality

The shared directional-input boundary and Hydra radiance conversion are implemented; their
contracts and current image limitations are in the
[Forward lighting design](../design/forward-lighting.md).

- [x] Add focused Vulkan Camera Light ON/OFF, rotated-camera and return-to-view
  fixtures for color/white patches, native handwritten/generated materials,
  and usdview linear/sRGB output. Native/Tier 0 center comparison and optional
  Tier 0/HgiVulkan image comparison are independent CTest gates.
- [ ] Extend the local OpenUSD 26.08 HgiVulkan fixture evidence to 26.05 and
  broaden the bounded first-frame camera sweep to interactive navigation and
  Kitchen coverage before changing ambient/direct balance, exposure or tone
  mapping. The color/white sweep now covers 256 immediate frames per presentation
  path; its [local evidence](../reports/delivery-history.md#forward-first-frame-camera-sweep--2026-10-10)
  does not establish general navigation quality. Generated material host ingestion
  remains outside the current Hydra subset.
- [ ] Validate the shared directional selection on native Metal, then align
  Vulkan/Metal lighting and output transforms with declared image tolerances.

The broader Forward quality exit and dependencies remain in the
[backlog](backlog.md#next-cycle-onward--forward-lighting-and-shading-quality).

Historical producer-session evidence and remaining OpenStrata integration asks
are in the [OST recheck](../reports/ost/13-2026-09-26-v0.23.8-recheck-v0.24.0-asks.md).
