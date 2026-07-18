# Contributing to hdMerlin

Thank you for helping improve hdMerlin. The project is still establishing its
foundation contracts, so changes should keep the renderer small, testable, and
host-neutral.

## Before you start

- Read the [renderer architecture](docs/design/renderer-architecture.md).
- Check the [current milestone](docs/roadmap/current.md) and
  [backlog](docs/roadmap/backlog.md) before starting substantial work.
- For a larger design or a change to a public API, open an issue or draft pull
  request before investing in the full implementation.

## Development setup

Follow the [build and install guide](docs/guides/build-and-install.md). A
Core-only build requires CMake 3.24 and a C++20 compiler. Vulkan and Hydra are
optional capability layers with additional dependencies.

The smallest portable validation loop is:

```powershell
cmake -S . -B build-core -DMERLIN_ENABLE_VULKAN=OFF
cmake --build build-core --config Debug --parallel
ctest --test-dir build-core -C Debug --output-on-failure
```

## Change guidelines

- Keep Core public APIs independent of OpenUSD, Hydra, MaterialX, Slang compiler
  APIs, Vulkan, Metal, Qt, and DCC SDK types.
- Keep host translation in adapters and GPU execution in the Vulkan backend.
- Preserve deterministic extraction and render-product metadata.
- Return an actionable diagnostic or explicit fallback for unsupported input.
- Add or update tests for observable behavior.
- Add counters or benchmark evidence for performance-sensitive changes; FPS by
  itself is not a performance contract.
- Normalize Hydra, MaterialX, and future material sources into host-neutral
  `MaterialIR` rather than coupling a source graph directly to GPU code.
- Keep generated material modules limited to material evaluation. Geometry,
  lighting, alpha/depth policy, render passes, resources, and AOVs remain
  renderer-owned, and graph topology keys remain separate from instance values
  and texture assignments. See the
  [MaterialXGenSlang boundary](docs/design/materialxgenslang-boundary.md).
- Preserve Tier 0 CPU readback as the correctness, headless, CI, and fallback
  path when experimenting with lower-copy presentation.
- Require measurements that identify the limiting stage before adopting
  bindless, indirect-draw, meshlet, mesh-shader, or host-interop complexity.
- Update documentation when a command, dependency, public target, compatibility
  promise, or limitation changes.

The build enables `/W4` on MSVC and `-Wall -Wextra -Wpedantic` elsewhere. New
code must build without warnings in every configuration it affects.

## Pull requests

Keep pull requests focused and describe:

- the problem and chosen approach;
- affected configurations (Core, Vulkan/headless, Hydra, MaterialX);
- commands used to verify the change;
- any capability that could not be exercised locally;
- public API, package, performance, or compatibility implications.

Run the relevant source tests and install-tree consumer before requesting
review. Missing optional GPU, validation, or OpenUSD capabilities should be
reported explicitly rather than presented as passing coverage.

## Reporting security issues

Do not open a public issue for a suspected vulnerability. Follow the private
reporting process in [SECURITY.md](SECURITY.md).

## License

Unless explicitly stated otherwise, contributions submitted to this repository
are licensed under the [Apache License 2.0](LICENSE).
