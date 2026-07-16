# Current

Only incomplete work for the next release milestone and active carry-over is
listed here. Completed pre-release detail is retained in the
[delivery history](../reports/delivery-history.md); shipped versions will be
recorded in the [changelog](../../CHANGELOG.md).

## Next milestone

### 🚧 v0.8.0 — Slang foundation and Vulkan parity

**Objective:** move the existing Vulkan Forward shader source of truth from
GLSL to Slang while preserving output, performance, packaging, and fallback
behavior, and establish the shared ABI and Metal compile gates needed before
new shader families expand.

#### 1. Boundary and artifact audit

- Audit Core, `FrameSnapshot`, viewport, presentation, and telemetry boundaries
  for leaked Vulkan execution concepts before freezing a shared shader-facing
  contract.
- Pin the supported Slang toolchain and produce reproducible, versioned,
  installable shader artifacts with dependency, compiler, and generator
  provenance.
- Define deterministic shader/permutation keys and reflection metadata with
  explicit cache-compatibility rules.

#### 2. Vulkan Forward migration

- Migrate the existing Forward vertex and fragment paths from GLSL to Slang,
  keeping conventional and bindless execution selectable through the existing
  capability contract.
- Preserve color, depth, `primId`, and `instanceId` reference output and avoid a
  material performance regression.
- Make clean and incremental builds rebuild only affected shader artifacts and
  package the complete runtime shader set for build-tree and install-tree use.

#### 3. ABI and cross-target gates

- Add reflected C++/Slang layout and resource-binding contract tests so layout,
  binding, permutation, and feature mismatches fail with actionable diagnostics.
- Compile the common shader set to SPIR-V and a Metal target, declaring explicit
  unsupported features and fallbacks rather than weakening Vulkan fast paths.
- Retain shader capability declarations that later backend and MaterialX work
  can extend without exposing API-specific command or resource objects through
  Core.

#### Scope boundary

v0.8.0 does not add a Metal renderer, native viewport, MaterialX translation,
Gaussian shaders, GPU-driven submission, Visibility, or meshlets. It establishes
their reproducible shader source, reflection, ABI, and compile foundations while
the working Vulkan renderer remains the output reference.

#### Exit criteria

- Slang-generated Vulkan Forward preserves color, depth, `primId`, and
  `instanceId` output under the declared exact/tolerance rules and shows no
  material performance regression.
- Clean and incremental builds produce complete installable versioned artifacts
  with deterministic keys, provenance, and dependency tracking.
- Reflection tests detect C++/shader layout and resource-binding mismatches.
- Common shaders compile for Vulkan and Metal targets with explicit diagnostics
  and fallback declarations for unsupported features.
- The superseded GLSL runtime path can be removed without losing a supported
  renderer configuration or evidence path.

## Active carry-over

- Keep the enrolled repository-scoped Windows x64 `vulkan-1.4` runner available
  for release-candidate evidence. The Debug/Release Vulkan and OpenUSD 26.05
  Hydra jobs completed in
  [capability run 29508228337](https://github.com/animu-sphere/hydra-merlin/actions/runs/29508228337),
  including a same-GPU `v0.7.0` benchmark baseline.
- Retain dependency/runtime provenance, validation logs, expected/actual/diff
  images, benchmark output, Hydra discovery, RenderBuffer, and usdview results
  as comparable artifacts rather than reducing capability jobs to a binary
  pass/fail signal.
