# Hydra Merlin

[![Core CI](https://github.com/animu-sphere/hydra-merlin/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/animu-sphere/hydra-merlin/actions/workflows/ci.yml)
[![License: Apache-2.0](https://img.shields.io/github/license/animu-sphere/hydra-merlin)](LICENSE)

hdMerlin is a lightweight, host-neutral raster renderer with independent Vulkan
and Metal backends and a Hydra 2 adapter. It provides a revisioned scene model,
immutable snapshots, explicit submission/completion, native viewports, and
measurable, recoverable fallback paths. It is built as an OpenStrata renderer
project so builds, validation and runtime composition are reproducible.

## Highlights

- **Host-neutral core:** public Core APIs expose no OpenUSD, Hydra, DCC, Qt,
  Vulkan or Metal types; hosts integrate through thin adapters.
- **Vulkan and Metal:** independent native backends implement the same
  renderer semantics, with Forward as the correctness reference.
- **Hydra 2:** a render delegate for usdview and other Hydra hosts, with CPU
  RenderBuffers as the universal path and Hgi GPU-copy presentation.
- **Mesh and Gaussian rendering:** standard OpenUSD meshes and
  `ParticleField3DGaussianSplat` share one persistent scene while keeping
  separate rendering algorithms.
- **Slang and MaterialX:** Slang is the shader source of truth; an optional
  MaterialXGenSlang boundary generates material-evaluation functions while the
  renderer owns lighting, passes and resources.

## Architecture

```text
Hydra / headless / native viewport adapters
            │  normalize host data
            ▼
RenderWorld ──► immutable snapshot ──► draw extraction
            │
            ▼
backend-neutral execution contract
            │
            ▼
Vulkan or Metal backend ──► render products / native presentation
```

Dependencies point from adapters toward Core, never the reverse. See the
[renderer architecture](docs/design/renderer-architecture.md).

## Quick start

With the [OpenStrata CLI](docs/design/openstrata-project.md):

```powershell
ost runtime pull cy2026 --profile core
ost build --jobs auto
ost validate --json
```

Or with CMake presets (Windows / Vulkan shown; use the `metal` preset with
Ninja on macOS):

```powershell
cmake --preset vulkan -G "Visual Studio 17 2022" -A x64 -DMERLIN_BUILD_VIEWPORT=ON
cmake --build --preset vulkan --parallel
ctest --preset vulkan
./build/vulkan/adapters/merlin-viewport/Release/merlin-viewport.exe --backend vulkan
```

A Core-only build needs only CMake and a C++20 compiler:
`cmake --preset core`, then build and test the same preset. See
[build and install](docs/guides/build-and-install.md) for prerequisites,
platform procedures, Hydra builds and MaterialX, and
[running the viewport](docs/guides/viewport.md) for USD stages, usdview and
Gaussian options.

## Supported platforms

Windows x64 (Vulkan), macOS on Apple Silicon (Metal) and Linux x64 (Core, plus
Vulkan on a software driver) are exercised, with Hydra 2 on Windows and macOS.
The [support matrix](docs/reference/support-matrix.md) is the canonical list of
validated platforms, SDK versions, features, evidence levels and known
limitations.

## OpenUSD / Hydra

The Hydra 2 adapter is opt-in, so OpenUSD never becomes a transitive dependency
of Core or the native backends. Build it against a compatible OpenUSD SDK (see
the support matrix for validated versions) using the
[Hydra build procedure](docs/guides/build-and-install.md#hydra-2), then select
**Merlin** in usdview or another Hydra host. hdMerlin consumes standard USD
data and defines no renderer-specific USD schema.

## Documentation

| Topic | Document |
| --- | --- |
| Documentation map and ownership rules | [docs/README.md](docs/README.md) |
| Build, install and run | [Build and install](docs/guides/build-and-install.md), [Viewport and Hydra hosts](docs/guides/viewport.md) |
| Consume the SDK | [Using the CMake package](docs/guides/cmake-package.md) |
| What works now | [Support matrix](docs/reference/support-matrix.md) |
| Architecture and contracts | [Renderer architecture](docs/design/renderer-architecture.md), [design documents](docs/README.md#design) |
| Validation and performance | [Forward validation](docs/guides/forward-validation.md), [Benchmarking](docs/guides/benchmarking.md) |
| What shipped | [Changelog](CHANGELOG.md), [release records](docs/releases/README.md) |
| Contributing | [Contributing](CONTRIBUTING.md), [Code of Conduct](CODE_OF_CONDUCT.md), [Security policy](SECURITY.md) |

## Roadmap

The v0.16.x series stabilizes the Forward renderer before v0.17.0 resumes
Gaussian optimization; see the [current work](docs/roadmap/current.md) and the
[backlog](docs/roadmap/backlog.md).

## License

hdMerlin is licensed under the [Apache License 2.0](LICENSE).
