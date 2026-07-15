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
model for Mesh and future Gaussian data. The ordered ladder then moves the
shader source of truth from GLSL to Slang while preserving Vulkan output and
establishing a Metal compile gate. It extracts the minimum backend contract and
delivers the dedicated backend-neutral `merlin-viewport` with Vulkan
presentation, proves a MaterialXGenSlang material-function slice, and brings up
Metal residency, native presentation, and an
HgiMetal host bridge before Gaussian shader work expands. The later Mesh and
Gaussian path advances through persistent draw identity, GPU-driven execution,
an experimental opaque Visibility Buffer, production MaterialX quality, and
static meshlets. Optional Mesh Shader, Hi-Z/LOD, and large-scene streaming
remain measurement-gated. The architecture behind this order is recorded in
the [multi-backend shader and presentation strategy](../design/multibackend-slang-materialx.md).

When a version ships, its completed scope is captured in the changelog and
removed from the roadmap. The roadmap is not a second changelog.

## Quality bar

Every release must preserve these properties:

- Core public APIs remain independent of OpenUSD, Hydra, Vulkan, Metal, Qt, and
  DCC SDKs.
- Core-only, Vulkan, Metal, and enabled host configurations build without
  warnings when their corresponding backend is enabled.
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

- **Share renderer semantics, not GPU commands.** RenderWorld, FrameSnapshot,
  MaterialIR, AOV meanings, logical resource identity, completion lifetime, and
  common telemetry stay backend-neutral. Allocation, command encoding,
  synchronization, descriptors/argument buffers, and native presentation stay
  backend-owned; hdMerlin does not pre-design a Vulkan-shaped general RHI.
- **Move the shader source of truth before shader families expand.** Existing
  Vulkan Forward output migrates to Slang and passes a Metal compile/reflection
  gate before Gaussian, GPU-driven, Visibility, MaterialX production, or
  meshlet shader work grows.
- **MaterialIR before MaterialX.** Hydra networks, MaterialX documents, and
  future sources normalize into one host-neutral material boundary before
  shader generation. MaterialXGenSlang produces a material-evaluation function;
  hdMerlin still owns geometry, lighting, render passes, resources, and AOVs.
- **Consume standard USD data.** Gaussian support consumes the existing
  OpenUSD representation exposed through Hydra. hdMerlin defines no custom USD
  schema, prim type, or file-format convention and does not parse PLY/SPLAT
  assets directly.
- **Share resources, not algorithms.** Mesh and Gaussian data share the
  persistent RenderWorld, transforms, visibility, allocation, synchronization,
  render targets, and profiling, while their rasterization, culling, sorting,
  compositing, and LOD pipelines remain distinct.
- **Keep presentation separable.** Native Vulkan viewport, headless rendering,
  native Metal viewport, headless rendering, and Hgi/DCC presentation use the
  same renderer core while keeping their presentation costs independently
  measurable. HgiMetal is a host AOV bridge, not the Metal rendering RHI.
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
| P1 | Slang migration, Vulkan parity, shader ABI validation, and the Metal compile gate |
| P2 | Backend contract, dedicated cross-backend `merlin-viewport`, MaterialXGenSlang prototype, Metal execution/residency, and Metal/Hydra presentation |
| P3 | Gaussian MVP, persistent draw identity, GPU-driven Mesh/Gaussian execution, opaque Visibility, and MaterialX quality |
| P4 | Static meshlets, optional Mesh Shader/Hi-Z/LOD, large-scene streaming, DCC integration, and v1.0 contracts |
