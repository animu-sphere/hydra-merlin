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

- 🚧 Connect GPU-sorted Gaussian records to tile pairing/ranges and indirect
  raster without losing stable IDs, deterministic order, or bounded overflow
  behavior. Validate against CPU-sorted reference images.
- ⬜ Remove per-draw CPU preparation from the steady-state Mesh submission path
  while preserving the indexed Forward image and fallback contracts.
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
