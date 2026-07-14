# Roadmap

The roadmap contains only **incomplete** work. Shipped work belongs in the
[changelog](../../CHANGELOG.md) and, when more detail is useful, optional
[release records](../releases/). Completed pre-release roadmap detail belongs in
the [delivery history](../reports/delivery-history.md), and implementation
evidence and design rationale belong in [reports](../reports/) and
[design](../design/).

Legend: 🚧 in progress · ⬜ not started

| Document | Contents |
| --- | --- |
| [current.md](current.md) | The next release milestone and active carry-over work. |
| [backlog.md](backlog.md) | Ordered releases after the active milestone and cross-cutting open work. |

v0.5.0 shipped the host-neutral MaterialIR, basic textured shading, and usdview
slice. v0.6.0 shipped the performance-observability foundation and incremental
Hydra synchronization work, with completed detail retained in the changelog and
delivery history. The active v0.7.0 milestone extends the persistent resource
model to Mesh and Gaussian data. The ordered ladder then establishes a native
Vulkan performance reference and lands Gaussian rendering before GPU-driven
optimization. The Mesh path then advances through persistent
draw identity, GPU-driven indexed Forward, an experimental opaque Visibility
Buffer, MaterialX quality work, and static meshlets. Low-copy presentation can
proceed once evidence justifies it; Mesh Shader, Hi-Z/LOD, and large-scene
streaming follow only after their indexed fallbacks and measurements are
available.

When a version ships, its completed scope is captured in the changelog and
removed from the roadmap. The roadmap is not a second changelog.

## Quality bar

Every release must preserve these properties:

- Core public APIs remain independent of OpenUSD, Hydra, Vulkan, Qt, and DCC SDKs.
- Core-only, Vulkan, and enabled host configurations build without warnings.
- CTest distinguishes a product failure from a missing optional capability.
- Deterministic scene input produces deterministic extraction and image metadata.
- Vulkan validation reports renderer-owned warnings and errors as failures while
  keeping unrelated loader or host diagnostics distinguishable.
- Build-tree and install-tree consumers exercise the same public contracts.
- Resource lifetime is safe across frame latency, resize, and removal.
- New performance-sensitive work adds counters or benchmark evidence; FPS alone
  is not accepted as the performance contract.
- Unsupported inputs return an actionable diagnostic or an explicit fallback.

## Sequencing principles

- **MaterialIR before MaterialX.** Hydra networks, MaterialX documents, and
  future sources normalize into one host-neutral material boundary before
  shader generation.
- **Consume standard USD data.** Gaussian support consumes the existing
  OpenUSD representation exposed through Hydra. hdMerlin defines no custom USD
  schema, prim type, or file-format convention and does not parse PLY/SPLAT
  assets directly.
- **Share resources, not algorithms.** Mesh and Gaussian data share the
  persistent RenderWorld, transforms, visibility, allocation, synchronization,
  render targets, and profiling, while their rasterization, culling, sorting,
  compositing, and LOD pipelines remain distinct.
- **Keep presentation separable.** Native Vulkan viewport, headless rendering,
  and Hgi/DCC presentation use the same renderer core while keeping their
  presentation costs independently measurable.
- **Measure before optimizing.** Hydra Sync, scene normalization, command
  recording, GPU work, readback, host upload, and presentation must be separated
  before selecting an optimization.
- **Establish the shared ABI before specialized paths.** Bindless resource
  tables and a persistent GPU Scene with stable resource/draw identity precede
  GPU-driven submission, Visibility, and meshlets.
- **Prove indexed execution before Mesh Shader execution.** GPU-driven Forward
  and meshlet rendering start with indexed indirect commands. Mesh Shader is an
  optional measured backend, not the definition of a meshlet.
- **Keep Visibility scoped.** The Visibility Buffer initially handles supported
  opaque indexed Mesh only. Forward remains the image reference and fallback;
  transparent Mesh, Gaussian primitives, and overlays keep specialized passes.
- **Keep the reference path.** Tier 0 CPU readback remains available for
  headless execution, image comparison, debugging, and unsupported-host
  fallback even when a lower-copy path exists.
- **Keep integrations outside Core.** DCC packages own discovery, environment,
  settings UI, package metadata, and host smoke tests; Core owns no DCC SDK
  dependency.

## Priority bands

| Priority | Direction |
| --- | --- |
| P0 | v0.7.0 persistent bindless Mesh/Gaussian resources, comparable performance evidence, and GPU capability CI |
| P1 | GPU residency completion and native viewport |
| P2 | Gaussian MVP, persistent Mesh draw identity, and GPU-driven indexed rendering |
| P3 | Opaque Visibility, GPU presentation interop, MaterialX quality, and static meshlets |
| P4 | Optional Mesh Shader/Hi-Z/LOD, large-scene streaming, DCC integration, and v1.0 contracts |
