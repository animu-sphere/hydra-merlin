# Current

Active and immediately next work lives here. Shipped changes are recorded in
the [changelog](../../CHANGELOG.md) and [v0.16.0 release record](../releases/v0.16.0.md);
current support claims live in the [support matrix](../reference/support-matrix.md).

## v0.16.x follow-up — Gaussian quality and coverage

The v0.16.0 GPU-driven foundation is shipped within its documented validated
scope. Complete the remaining quality and coverage work before expanding the
claims. See the [GPU-driven design](../design/gpu-driven-rendering.md),
[Gaussian pipeline design](../design/gaussian-rendering.md), and
[Metal execution plan](../design/metal-gaussian-execution.md).

- [x] Investigate the sparse 15/255 HgiMetal tiled color tail and capture larger
  host/native viewport tile output. Float accumulation and pixel-center
  evaluation reduce the 8,192-particle host tail to 6/255; framed 1M host images
  match exactly and native viewport images differ by at most 1/255. The distinct
  offscreen and host policies remain unchanged. See the
  [2026-10-05 delivery evidence](../reports/delivery-history.md#v016x-metal-quality-host-aovs-and-native-viewport).
- [ ] Broaden Metal Gaussian execution evidence beyond Apple M3 and tune tile
  work on representative scenes. Record total GPU cost as well as raster savings;
  current 65K/1M binning and sorting can outweigh the raster improvement.
- [x] Resolve the macOS Mesh usdview movement assertion failure. HgiMetal Y
  reflection now corrects front-face winding; all existing phases and the
  selection phase pass with validation on Apple M3/OpenUSD 26.08.
- [ ] Extend same-device/host measurements to other supported GPUs with
  controlled clocks/power before establishing hardware timing gates.
- [x] Extend demand-driven Hydra depth/ID readback to HgiMetal, preserving
  Tier 0 and picking AOV semantics. Exact current-frame AOV and usdview
  selection checks pass; normal unselected frames read back no image AOVs.
- [ ] Broaden actual host click picking and overlay parity evidence beyond
  the automated Hdx selection and demand-driven ID Map checks.

## Active carry-over — validation gates

- [ ] Separate required hosted builds, shader/ABI checks, MaterialX generation,
  and install-tree consumers from scheduled runtime/capability checks and
  hardware-specific performance evidence. GPU timing is not a universal PR
  gate until runner variance is controlled.
- [ ] Keep SPIR-V validation, Metal-target compilation, Hydra compilation, and
  Linux Vulkan runtime coverage as distinct gates. Hosted GPU-free MaterialX
  generation and consumers now have passing Windows/Linux Debug/Release evidence.
- [ ] Add Linux Vulkan configuration and shader builds, headless execution with
  Mesa lavapipe, optional real-GPU evidence, and GLFW viewport smoke coverage
  for supported window systems.

Historical producer-session evidence and remaining OpenStrata integration asks
are in the [OST recheck](../reports/ost/13-2026-09-26-v0.23.8-recheck-v0.24.0-asks.md).
