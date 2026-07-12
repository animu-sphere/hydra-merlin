# Current

Only incomplete work for the next release milestone and active carry-over is
listed here. Completed pre-release detail is retained in the
[delivery history](../reports/delivery-history.md); shipped versions will be
recorded in [release records](../releases/).

## Next milestone: v0.1.0 — reproducible renderer foundation

**Status:** in progress · **Depends on:** the existing RenderWorld → extraction
→ Vulkan → color/depth vertical slice and the Core/Vulkan install package.

Turn the working Hydra/headless foundation into a development baseline that a
new contributor and CI runner can reproduce.

### Reproducible build and distribution baseline

- Verify a clean checkout in Debug/Release, Vulkan ON/OFF, and Hydra opt-in
  configurations using the documented commands.
- Decide and document whether the Hydra runtime and headless executable become
  exported CMake targets or remain runtime-only install products.
- Add a release workflow and machine-readable dependency/version metadata.

### Measurement baseline

- Add a `merlin-benchmark` skeleton with deterministic JSON output.
- Record commit, build type, compiler, OS, GPU/driver, Vulkan API, and resolution.
- Add CPU scopes for scene update, extraction, upload, command recording,
  readback, and total frame time.
- Add draw/triangle, upload/readback byte, allocation, pipeline, and cache
  counters.
- Save first-frame, steady-state, and scene-edit baselines for the flattened
  reference path.

## Remaining exit criteria

- A clean checkout reproduces Core-only, Vulkan/headless, and Hydra builds using
  documented commands.
- The headless/Hydra exported-target or runtime-only packaging contract is
  explicit.
- Release automation emits machine-readable dependency/version metadata.
- Benchmark output contains the required machine/build metadata and structural
  counters.

## Carry-over follow-ups

- Define a host-neutral diagnostic sink instead of writing directly to stderr or
  a DCC-specific logger.
- Add configure-time OpenUSD version, build-configuration, and C++ runtime ABI
  compatibility checks.
- Define a common machine-readable result schema for discovery, delegate
  creation, RenderBuffer, GPU render, and host-presentation assertions.
