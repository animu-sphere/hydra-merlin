# Current

Active and immediately next work lives here. Shipped changes are recorded in
the [changelog](../../CHANGELOG.md); current support claims live in the
[support matrix](../reference/support-matrix.md).

## v0.16.0 — GPU-driven rendering

### Objective

Complete GPU-driven Mesh submission and Gaussian preparation through
image-producing rasterization. Conventional Forward and the CPU-sorted Gaussian
path remain the correctness references. See the
[GPU-driven design](../design/gpu-driven-rendering.md) and
[Gaussian pipeline design](../design/gaussian-rendering.md) for the
ownership, dependency, and fallback contracts.

### Work

- 🚧 Advance the [Metal Gaussian execution plan](../design/metal-gaussian-execution.md).
  Reference raster math is shared through Slang and an embedded metallib, with
  ABI, artifact identity, install-consumer and local Apple GPU image checks.
  GPU preparation/SH/radix-sort kernels are also shared and checked against the
  CPU reference on Apple GPU. The offscreen harness now connects GPU gather
  and indirect raster with color/depth/ID comparisons and no intermediate CPU
  readback. A private attribute store now supplies that harness with immutable
  resident buffers, range uploads and completion-safe versions. Continuous
  camera/transform/edit images and in-flight byte-budget checks cover the store.
  Renderer integration now selects GPU preparation/sort/gather/indirect raster,
  with completion-safe scratch, partial attributes, bounded allocations,
  post-completion counters and CPU fallback. Local renderer images and
  static/motion/edit scale captures cover the sorted-stream path. Garden GPU
  display was checked in the native viewport and usdview/HgiMetal, including
  preferred tiled fallback. Controlled host/corpus comparisons, per-stage GPU
  timestamps, tile raster and broader hardware performance evidence remain open.

- 🚧 Recheck the Hydra CPU-readback floor in usdview after preferring
  host-cached coherent Vulkan AOV buffers. Local RTX A5000 headless evidence
  at 597x540 with one million Gaussians reduces the four-AOV CPU readback
  median from 19.06 ms to 0.92 ms and GPU sorted-stream frame time from
  26.51 ms to 6.00 ms, without reducing the 5.2 MB payload. Interactive
  usdview/HgiGL display was also checked with a 5.8-million-Gaussian stage
  using the lookdev runtime. Capture a controlled host motion comparison
  and evaluate whether every bound AOV needs a per-frame CPU readback.
- ✅ Complete the supported Vulkan Mesh scaling validation. Eight opt-in
  configurations cover 1k/10k/100k draws, 16 triangle/quad meshes, eight basic
  materials, four textures/two samplers, and 2/4/8/16 production vertex-arena
  blocks. Static and camera-motion GPU phases have zero CPU preparation draw
  visits, uploads, allocations and descriptor writes, with exact four-AOV
  parity against Forward, including an independent renderer without GPU Scene.
  The generated-material ABI fixture validates actual conventional artifact
  execution, explicit required rejection/preferred fallback, and edit/module
  recovery. Unique-variant preflight and completed-context descriptor caches
  remove its steady preparation traversal/descriptor churn. Generated material
  command recording remains conventional and linear in draw count; bindless
  generated execution is not claimed. See the
  [Mesh validation record](../reports/delivery-history.md#v0160-vulkan-mesh-scaling-validation) for the same-device
  30-sample draw and batch sweeps and the actual MaterialX runtime checks.
- ⬜ Capture controlled hardware evidence for Gaussian preparation/sort/raster
  costs, including camera motion and static zero-upload frames. Mesh captures
  above are local headless observations with clocks/power unpinned, not universal
  timing thresholds or controlled host comparisons.

### Exit criteria

- [ ] Camera movement does not require CPU traversal of every instance or
  Gaussian, and Mesh CPU submission does not grow linearly with draw count.
- [ ] Gaussian preparation, sorting, tile work, and rasterization run in a
  bounded number of GPU submissions with deterministic color and ID output.
- [ ] Selected paths, rejection reasons, culling counts, stage costs, and
  explicit fallback behavior are covered by runtime and image evidence.

## Active carry-over — validation gates

- 🚧 Separate required hosted builds, shader/ABI checks, MaterialX generation,
  and install-tree consumers from scheduled runtime/capability checks and
  hardware-specific performance evidence. GPU timing is not a universal PR
  gate until runner variance is controlled.
- ⬜ Collect hosted MaterialX generation evidence and keep SPIR-V validation,
  Metal-target compilation, Hydra compilation, and Linux Vulkan runtime
  coverage as distinct gates.
- ⬜ Add Linux Vulkan configuration and shader builds, headless execution with
  Mesa lavapipe, optional real-GPU evidence, and GLFW viewport smoke coverage
  for supported window systems.

Historical producer-session evidence and remaining OpenStrata integration asks
are in the [OST recheck](../reports/ost/13-2026-09-26-v0.23.8-recheck-v0.24.0-asks.md).
