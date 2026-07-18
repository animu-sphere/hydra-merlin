# Roadmap

The roadmap contains only **incomplete** work. Shipped work belongs in the
[changelog](../../CHANGELOG.md) and, when more detail is useful, optional
[release records](../releases/). Completed pre-release roadmap detail belongs in
the [delivery history](../reports/delivery-history.md), and implementation
evidence and design rationale belong in [reports](../reports/) and
[design](../design/).

> hdMerlin is a lightweight, host-neutral, multi-backend Hydra raster renderer
> whose behavior is explicit, measurable, and recoverable through well-defined
> fallback paths.

The roadmap prioritizes durable ownership, identity, lifetime, diagnostic, and
measurement contracts before feature breadth. Version numbers are release
labels, not permission to violate a dependency gate: scope may move when
benchmark, capability, or host-integration evidence changes the justified
order.

Legend: 🚧 in progress · ⬜ not started

| Document | Contents |
| --- | --- |
| [current.md](current.md) | The next release milestone and active carry-over work. |
| [backlog.md](backlog.md) | Ordered releases after the active milestone and cross-cutting open work. |

v0.5.0 shipped the host-neutral MaterialIR, basic textured shading, and usdview
slice. v0.6.0 shipped the performance-observability foundation and incremental
Hydra synchronization work. v0.7.0 shipped the persistent resource model,
bindless Forward path, transfer and memory-budget infrastructure, and scale
evidence for Mesh and future Gaussian data. v0.8.0 moved the Vulkan Forward
shader source of truth to Slang, established reflected shader ABI validation,
and added the Metal compile gate. v0.9.0 extracts the minimum backend contract
and delivers the dedicated backend-neutral `merlin-viewport` with Vulkan
presentation; its completed pre-release detail is retained in the changelog and
delivery history. The active v0.10.0 milestone proves a MaterialXGenSlang
material-function slice, followed by a renderer-development diagnostic surface,
before Metal residency, native presentation, and an HgiMetal host bridge come
online. The later Mesh and Gaussian paths advance through persistent draw
identity, GPU-driven execution, an experimental opaque Visibility Buffer,
production MaterialX and lighting quality, and static meshlets. Optional Mesh
Shader, Hi-Z/LOD, and large-scene streaming remain measurement-gated. The
architecture behind this order is recorded in the [multi-backend shader and
presentation strategy](../design/multibackend-slang-materialx.md); the exact
v0.10.0 contract is the
[MaterialXGenSlang material boundary](../design/materialxgenslang-boundary.md).

When a version ships, its completed scope is captured in the changelog and
removed from the roadmap. The roadmap is not a second changelog.

## Product direction

hdMerlin is both a lightweight interactive Hydra renderer and a fast native
viewport renderer for USD scenes. Its reusable rendering core is independent of
usdview, Qt, DCC SDKs, Vulkan, and Metal, and the repository serves as an
OpenStrata renderer unit with reproducible build, validation, packaging, and
runtime-composition evidence.

The project differentiates itself through persistent GPU resources,
revision-based updates, explicit submission/completion/readback contracts,
deterministic images and IDs, Forward as the correctness reference, a
renderer-owned MaterialX function boundary, standard Hydra Mesh/Gaussian
ingestion, separate native and host-presentation paths, and capability-selected
acceleration with reliable fallbacks.

It is deliberately not a USD authoring application, UI framework, asset
importer, replacement for USD composition or FileFormat plugins, Hgi-based
internal renderer RHI, or renderer whose correctness depends on Visibility or
Mesh Shader support. Compilation alone is not accepted as runtime support
evidence.

The long-term ownership invariants are:

- Core owns host-neutral scene, resource, material, snapshot, renderer,
  completion, diagnostic, and telemetry contracts and exposes no host, GPU API,
  window-system, DCC, or MaterialX graph types.
- Vulkan and Metal implement the same renderer meanings while retaining
  independent native resource management, synchronization, and presentation.
- Hydra synchronizes scene data, maps paths to Merlin handles, converts inputs,
  integrates host RenderBuffers/presentation, and forwards diagnostics; it does
  not own renderer scheduling, residency, shader ABI, or material evaluation.
- Conventional Forward remains the universal correctness reference and
  fallback.
- Every optional path declares required features/limits, validation evidence,
  selection policy, fallback, rejection diagnostics, and selected-path
  telemetry before automatic use.

## Product phases

| Phase | Objective | Planned scope |
| --- | --- | --- |
| A — Renderer foundation | Complete material, diagnostics, and renderer-development foundations. | v0.10.0, v0.10.x, and foundation gates |
| B — Backend parity | Establish cross-platform backend and presentation parity. | v0.11.0–v0.13.0 |
| C — Scene and lighting breadth | Broaden scene representation and establish the lighting ladder. | v0.14.0–v0.15.0 and lighting tiers |
| D — GPU scalability | Scale submission and shading through measured GPU-driven paths. | v0.16.0–v0.20.0 |
| E — Production readiness | Productize large scenes, DCC hosts, runtime composition, and v1.0 contracts. | v0.21.0–v1.0.0 |

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

Every milestone defines its release gates in six categories:

1. **Correctness:** reference images, AOV/ID semantics, deterministic identity,
   update and lifetime behavior, and unsupported-input fallback.
2. **Performance:** first-frame and steady-state cost, camera and scene edits,
   CPU/GPU stage distributions, resource activity, and feature-relevant scale
   fixtures.
3. **Compatibility:** compiler/SDK versions, OpenUSD runtime, device features
   and limits, host presentation, and install-tree consumption.
4. **Diagnostics:** stable codes, source context, named recovery, selected path,
   and an explanation visible through logs or the development viewport.
5. **Packaging:** versioned installed targets and artifacts, dependency and
   OpenStrata metadata, and host discovery metadata where applicable.
6. **Documentation:** support matrix and milestone updates, completed-work
   archival, explicit unsupported behavior, and benchmark interpretation.

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
- **Broaden materials and lighting together.** Material evaluation produces
  renderer-consumable surface properties; the renderer owns illumination,
  visibility, integration, and presentation. A MaterialX input is not supported
  until its lighting behavior, fixture, diagnostic, and fallback are defined.
- **Separate shader identity from instance state.** Canonical graph topology
  identifies a generated material module, compiler/target policy identifies a
  backend artifact, parameter values identify material instance state, and
  texture assignments identify resource binding state. Value-only edits do not
  create new shader variants.
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

The final ordering rule is: ownership boundaries before feature breadth; stable
identity and lifetime before GPU-driven execution; Forward correctness before
alternate shading; material ABI before broad MaterialX; standard Hydra data
before renderer-specific formats; native presentation before host-specific
low-copy optimization; and diagnostics, fallbacks, and measured evidence before
DCC expansion or v1.0 support claims.
