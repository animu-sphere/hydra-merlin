# Current

Only incomplete work for the next release milestone and active carry-over is
listed here. Completed pre-release detail is retained in the
[delivery history](../reports/delivery-history.md); shipped versions will be
recorded in the [changelog](../../CHANGELOG.md).

## Next milestone

### 🚧 v0.5.0 — MaterialIR and basic shading

**Objective:** release the implemented host-neutral MaterialIR and basic
textured shading path with durable GPU/Hydra evidence and consistent release
claims.

The feature slice is complete locally: Core owns revisioned material,
texture, and sampler resources; extraction emits deterministic records and
structured fallbacks; Vulkan owns completion-safe residency and feature-keyed
caches; headless and Hydra render through the same basic shading path.

#### Remaining release work

- Run the full Debug/Release Vulkan and Release Hydra capability matrix on the
  maintained Windows GPU runner and retain validation, image, benchmark, plugin,
  and textured usdview artifacts.
- Fold the unfinished v0.4.1 release-integrity checks into the release gate so
  `VERSION`, CMake/OpenStrata metadata, changelog, support status, roadmap, and
  tag cannot contradict one another.
- Add the planned host-neutral diagnostic sink and actionable OpenUSD
  build/runtime compatibility rejection, preserving current structured Vulkan
  errors and material fallback records.
- Perform release preparation only after those gates pass; until then the
  project version remains the last shipped version.

#### Exit criteria

- Core-only, Vulkan/headless, and Hydra builds are warning-free in their claimed
  configurations, with clean validation on the equipped capability runner.
- Revisioned texture/sampler behavior, value-only pipeline reuse, feature
  variants, structured fallback, expected/actual/diff artifacts, and textured
  usdview rendering remain covered by automated tests.
- Release identity and documentation consistency checks reject stale metadata.
- Invalid lifetime operations and unsupported inputs remain distinguishable and
  host-observable without relying only on stderr parsing.
- An isolated install tree discovers the plugin and renders the textured host
  fixture using the declared OpenUSD ABI/configuration.

## Active carry-over

- Enroll or confirm a repository-scoped Windows x64 runner with the existing
  `vulkan-1.4` label so the manual workflow becomes continuing GPU evidence.
- Retain dependency/runtime provenance, validation logs, expected/actual/diff
  images, benchmark output, Hydra discovery, RenderBuffer, and usdview results
  as comparable artifacts rather than reducing capability jobs to a binary
  pass/fail signal.
