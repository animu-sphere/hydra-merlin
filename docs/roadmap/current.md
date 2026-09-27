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
  The harness now uses a backend executor with completion-safe scratch reuse,
  zero-allocation camera frames and blocked-submission/budget coverage.
  Controlled static/motion/edit captures, native viewport/HgiMetal rechecks,
  and renderer integration of residency, frame scheduling and telemetry through
  raster remain open.

- 🚧 Recheck the Hydra CPU-readback floor in usdview after preferring
  host-cached coherent Vulkan AOV buffers. Local RTX A5000 headless evidence
  at 597x540 with one million Gaussians reduces the four-AOV CPU readback
  median from 19.06 ms to 0.92 ms and GPU sorted-stream frame time from
  26.51 ms to 6.00 ms, without reducing the 5.2 MB payload. Interactive
  usdview/HgiGL display was also checked with a 5.8-million-Gaussian stage
  using the lookdev runtime. Capture a controlled host motion comparison
  and evaluate whether every bound AOV needs a per-frame CPU readback.
- 🚧 Remove per-draw CPU preparation from the steady-state Mesh submission path
  while preserving the indexed Forward image and fallback contracts. Vulkan
  now reuses draw summaries, GPU Scene slot maps, and indirect batches on
  static/camera-only frames. The 1k/10k/100k shared-geometry fixture verifies
  zero CPU draw visits and uploads with exact Forward color/depth/ID parity.
  The diverse-objects fixture extends those checks to 16 triangle/quad meshes,
  eight basic materials, and multiple batches. Texture/generated-material
  diversity and multiple-arena-block batch scaling remain open.
- ⬜ Capture controlled hardware evidence for Mesh command-recording and
  Gaussian preparation/sort/raster costs, including diverse geometry and
  materials, camera motion, and static zero-upload frames.

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
