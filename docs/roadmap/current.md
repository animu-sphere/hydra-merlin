# Current

The next release milestone and active carry-over work. Shipped versions will be
recorded in [releases/](../releases/).

## Next milestone: v0.1.0 — reproducible renderer foundation

**Status:** in progress · **Depends on:** the existing RenderWorld → extraction
→ Vulkan → color/depth vertical slice and the Core/Vulkan install package.

Turn the working Hydra/headless foundation into a development baseline that a
new contributor and CI runner can reproduce. The source build, installed
package, runtime loader/device checks, and local validation suite already use
Vulkan 1.4 as their minimum contract.

### Vulkan 1.4 CI evidence

- Add the headless validation loop and install-tree usdview stable-update test to
  a supported Windows Vulkan 1.4 CI runner. The manually dispatched capability
  workflow, digest-pinned Animusphere OpenUSD runtime, and runner label contract
  are present; enrolling the labeled GPU runner is tracked in the
  [backlog](backlog.md).
- Persist loader, device, driver, SDK, and API versions in test or benchmark
  artifacts rather than only printing runtime capabilities to the console.
  Headless now writes this JSON and the capability workflow retains it.

### Reproducible build and distribution baseline

- Add Windows and Linux Core-only build/test CI. Debug and Release Core-only
  source/install-consumer jobs are present, with per-ref concurrency cancelling
  superseded runs. Assertion-based tests keep `assert()` live in Release via a
  scoped `NDEBUG` strip in `tests/CMakeLists.txt`.
- Split Core, Vulkan/headless, and Hydra jobs by required capability and make
  every skip reason explicit. Hosted Core and manually dispatched
  Vulkan/headless and Hydra jobs are now separate; enrolling the labeled GPU
  runner is tracked in the [backlog](backlog.md).
- Add `LICENSE`, `CONTRIBUTING.md`, `SECURITY.md`, `CHANGELOG.md`, architecture,
  build/install, package, and support-matrix documentation.
- Verify a clean checkout in Debug/Release, Vulkan ON/OFF, and Hydra opt-in
  configurations.
- Decide and document whether Hydra runtime and the headless executable become
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

## Exit criteria

- A clean checkout reproduces Core-only, Vulkan/headless, and Hydra builds using
  documented commands.
- CI covers Windows and Linux Core builds; capability runners cover Vulkan and
  OpenUSD/Hydra without turning missing dependencies into false product failures.
- Build-tree and install-tree consumers pass.
- Headless and Hydra validation runs are clean when the capability workflow is
  dispatched on a labeled GPU runner. Enrolling that runner for continuous
  evidence is a [backlog](backlog.md) item and does not gate v0.1.0.
- Benchmark output contains the required machine/build metadata and structural
  counters.
- Public licensing, architecture, build, and support documentation is present.
- The README states which MaterialX, advanced viewport, and GPU interop features
  are intentionally unavailable in v0.1.0.

## Carry-over follow-ups

- Define a host-neutral diagnostic sink instead of writing directly to stderr or
  a DCC-specific logger.
- Add configure-time OpenUSD version, build-configuration, and C++ runtime ABI
  compatibility checks.
- Define a common machine-readable result schema for discovery, delegate
  creation, RenderBuffer, GPU render, and host-presentation assertions.
