# Current

Only incomplete work for the next release milestone and active carry-over is
listed here. Completed pre-release detail is retained in the
[delivery history](../reports/delivery-history.md); shipped versions will be
recorded in the [changelog](../../CHANGELOG.md).

## Next milestone

### 🚧 v0.4.1 — Release integrity and diagnostics

**Objective:** stabilize the v0.4.0 execution contract, make failures
host-observable without stderr parsing, and keep release claims synchronized
with implementation and exercised capability evidence.

#### Scope

- Extend release-integrity checks so `VERSION`, the CMake/OpenStrata project
  versions, release tag, latest changelog heading, support status, and roadmap
  state cannot contradict one another.
- Introduce a host-neutral diagnostic sink with severity, stable code, message,
  source, and optional prim path; adapters translate it into host-native
  reporting.
- Preserve the implemented Vulkan `RendererError` classification while defining
  portable mappings for invalid requests, unsupported products/formats,
  allocation or submission failures, timeout, device loss, premature resolve,
  unrequested products, and internal failure.
- Broaden negative lifetime tests for invalid requests/tokens, timeout retry,
  unresolved frame targets, completion ordering, and product selection.
- Add configure-time OpenUSD version, shared/static mode, build-configuration,
  C++ runtime, and plugin ABI compatibility checks with actionable failures.
- Keep install-tree plugin discovery and first-frame host execution as release
  evidence, and merge optional Hydra results into the root OpenStrata renderer
  report.
- Define a tiered capability cadence: portable Core, headless, and install checks
  on pull requests; Vulkan rendering, image comparison, Hydra integration, and
  benchmark regression on equipped recurring jobs; multi-vendor usdview and
  install-tree validation for releases. Unequipped jobs are not passing
  evidence.

#### Exit criteria

- Release identity and documentation consistency checks fail deterministically
  on a stale version, changelog, support statement, or roadmap entry.
- Invalid lifetime operations fail deterministically; timeout, device loss, and
  unsupported capability remain distinguishable.
- Diagnostics can be collected through a host-neutral interface rather than
  only stderr.
- A mismatched OpenUSD build is rejected during configure with an actionable
  explanation.
- An isolated install tree discovers the plugin and renders its first host
  frame on an equipped capability runner.

## Active carry-over

- Enroll a repository-scoped Windows x64 runner with the existing
  `vulkan-1.4` label so the manual workflow produces continuing GPU evidence.
- Retain dependency/runtime provenance, validation logs, images, benchmark
  output, Hydra discovery, RenderBuffer, and usdview results as comparable
  artifacts rather than reducing capability jobs to a binary pass/fail signal.
