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
- [x] Add an opt-in authored multi-mesh USD navigation gate with Camera Light
  ON/OFF and linear/sRGB output, exact immediate/settled replay, and a separate
  Tier 0/HgiVulkan comparison. The fixture now includes full, quarter and opposite
  starting views with 288 immediate captures per path. Local Kitchen_set evidence
  is recorded in the [Forward lighting design](../design/forward-lighting.md);
  broader quality remains open.
- [ ] Extend navigation evidence beyond bounded mouse routes and three relative
  Kitchen views before changing ambient/direct balance, exposure or tone mapping.
  The color/white sweep now covers 256 immediate frames per presentation
  path; a separate Qt Alt+mouse tumble/truck/zoom fixture now covers 192 immediate
  frames per path, including exact replay/reference and restored-view checks.
  These bounded captures do not establish unrestricted navigation or Kitchen
  quality. The [Forward lighting design](../design/forward-lighting.md) records
  the gates and evidence. Generated material host ingestion remains outside the
  current Hydra subset.
- [ ] Align Vulkan/Metal lighting and output transforms with declared image
  tolerances. Native Metal directional selection and the current diagnostic
  response are validated on Apple M3 in Debug/Release; evidence and remaining
  output differences are in the [Forward lighting design](../design/forward-lighting.md).
  Focused Metal native/Tier 0 center and Tier 0/HgiMetal lighting, sweep and Qt
  navigation gates now cover the existing diagnostic output. White OFF still
  saturates under Metal's uncompressed profile; backend output alignment and
  broader scene quality remain incomplete.

The broader Forward quality exit and dependencies remain in the
[backlog](backlog.md#next-cycle-onward--forward-lighting-and-shading-quality).

Historical producer-session evidence and remaining OpenStrata integration asks
are in the [OST recheck](../reports/ost/13-2026-09-26-v0.23.8-recheck-v0.24.0-asks.md).
