# Changelog

All notable changes to hdMerlin will be documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project intends to follow [Semantic Versioning](https://semver.org/)
after its public API and release process are established.

## [Unreleased]

### Added

- Host-neutral `RenderWorld` and deterministic draw extraction libraries.
- Persistent Vulkan 1.4 offscreen rendering with color/depth CPU readback.
- Opt-in Hydra 2 adapter and install-tree usdview stable-update validation.
- Versioned CMake package exports for Core and optional Vulkan consumers.
- Windows/Linux Core CI and manually dispatched Vulkan/Hydra capability CI.
- Machine-readable Vulkan runtime provenance and retained validation artifacts.
- Deterministic `merlin-benchmark` JSON reports with environment metadata,
  per-stage CPU timings, structural counters, and first-frame, steady-state,
  and scene-edit baselines.
- Public contribution, security, build/install, package, support, architecture,
  roadmap, and delivery-history documentation.
- Versioned installed JSON metadata describing dependencies, configured layers,
  exported targets, and runtime-only products.
- Stable SemVer tag-driven Windows/Linux Core SDK releases with project-version
  validation and SHA-256 checksum assets.

No hdMerlin version has been formally released. Granular pre-release progress is
retained in the [delivery history](docs/reports/delivery-history.md).

[Unreleased]: https://github.com/animu-sphere/hydra-merlin/commits/main
