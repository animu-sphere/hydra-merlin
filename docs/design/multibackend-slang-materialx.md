# Multi-backend, Slang, MaterialX, and HgiMetal strategy

**Status:** v0.9 backend/viewport boundary implemented; later stages accepted

**Date:** 2026-07-16

hdMerlin evolves from its Vulkan first implementation into a backend-neutral
renderer with independently optimized Vulkan and Metal execution. The shared
boundary is the renderer's meaning and lifetime model, not a lowest-common-
denominator wrapper around GPU API commands.

The intended dependency direction is:

```text
Hydra / USD / native viewport / headless
                    |
                    v
              RenderWorld
                    |
                    v
              FrameSnapshot
                    |
                    v
       backend-neutral render contract
             /                 \
            v                   v
     Vulkan backend        Metal backend
```

Windows and Linux use Vulkan as the primary backend. macOS and Apple Silicon
use Metal. Direct3D 12, WebGPU, and a limited CPU/reference implementation are
possible later backends, but they do not contribute requirements to the first
Vulkan/Metal contract.

## Architectural decisions

- `RenderWorld`, `FrameSnapshot`, `MaterialIR`, `RenderRequest`,
  `RenderResult`, AOV semantics, resource identity, revisions, generation-
  checked handles, completion lifetime, capability meanings, and common
  telemetry remain backend-neutral.
- GPU allocation, command encoding, barriers/transitions, descriptor sets,
  argument buffers, pipeline-cache internals, presentation objects, queue
  ownership, and native synchronization remain backend-owned.
- Core public APIs expose neither Vulkan nor Metal handles or terminology.
  Logical handles and resource slots carry stable index plus generation; each
  backend decides how those slots are implemented and retired.
- The backend interface is expressed in renderer operations such as capability
  discovery, frame submission, resolve/readback, and presentation. It does not
  reproduce Vulkan command-buffer methods in a virtual RHI.
- The initial minimum contract is extracted from the working Vulkan path and
  kept Metal-neutral; Metal extends renderer meanings only when its bootstrap
  proves they are required. A general-purpose RHI is not designed in advance.
- The dedicated `merlin-viewport` application owns windows, input, camera
  control, UI, resize, picking, and an abstract presentation target. It is a
  permanent renderer product and performance reference. Vulkan owns its
  surface/swapchain adapter and Metal owns its `CAMetalLayer` and drawables;
  both plug into the same application host.
- The v0.9 Vulkan host uses GLFW behind a private adapter. GLFW types do not
  enter Core or Hydra public APIs, and Vulkan owns the created surface and
  swapchain. Presentation uses a GPU blit from the reference color attachment,
  retaining exact offscreen output and zero CPU readback for normal frames.
- Hydra ingestion mutates `RenderWorld` and never selects or calls a GPU API.
  Backend selection is a build, plugin, environment, command-line, or automatic
  platform decision.

Backend capabilities use renderer meanings such as bindless textures,
indirect draw/dispatch, asynchronous upload, timestamp queries, subgroup
operations, external presentation, and resource-table limits. API-specific
facts remain available as backend diagnostics rather than leaking into Core
capability names.

## Slang shader foundation

Slang becomes the source of truth for new shader work and progressively
replaces the current GLSL sources before Gaussian, GPU-driven, Visibility, and
meshlet shader families expand. The same module set produces Vulkan SPIR-V,
Metal-target output, and reflection metadata.

Normal distribution builds use precompiled shader artifacts. Development may
also support runtime compilation and hot reload, but runtime compilation is not
a required deployed dependency. Artifact and cache keys include at least:

- compiler version, target/profile, capability set, optimization/debug policy;
- matrix and scalar layout policy, entry point, and specialization parameters;
- source/module dependency hash; and
- generated MaterialX graph and generator version information where relevant.

Reflection is initially a contract test rather than an unchecked code-
generation mechanism. CI compares C++ and Slang sizes, offsets, alignment,
resource classes and arrays, binding spaces, stage visibility, entry points,
specialization parameters, and target capabilities. Stable areas may adopt
generated bindings later.

Shaders are organized as reusable common, geometry, material, pass, and small
platform-compatibility modules. Target-specific code is isolated and must have
an explicit capability diagnostic or fallback. All common shader modules pass
both Vulkan and Metal compile gates before the Metal renderer is considered
complete.

## MaterialX boundary

MaterialX uses the official Slang Shader Generator. A MaterialX document or
node graph is translated outside Core into a generated Slang material module,
which then produces both SPIR-V and Metal-target artifacts through the common
compiler pipeline.

MaterialX owns node-graph evaluation, node implementations, parameter
expressions, logical texture references, material models, and the generated
material function. hdMerlin owns geometry/camera/light inputs, transforms,
tangent frames, primvars, render passes, AOVs, picking, shadows, alpha policy,
resource tables, pipeline construction, and capability selection.

Generated code is consumed as a material-evaluation function rather than as a
complete fragment pass. Conceptually:

```slang
MaterialResult evaluateMaterial(MaterialInputs input);
```

hdMerlin pass modules build `MaterialInputs`, invoke that function, shade, and
write AOVs. This keeps one generated material usable by Forward, Visibility or
deferred resolve, shadow/picking variants, Vulkan, and Metal. Raw MaterialX
graphs never enter the Core scene model, and Gaussian appearance is not forced
into a MaterialX mesh BSDF.

The first prototype is a diagnosed Standard Surface subset with constants,
colors, images, texcoords, normals, arithmetic/mix operations, deterministic
generated-code caching, and Vulkan Forward integration. It must generate both
SPIR-V and Metal-target artifacts before production MaterialX quality work.

## Metal and HgiMetal presentation

`merlin-metal` renders directly with Metal. HgiMetal is not the rendering RHI;
it is a Hydra-host presentation and interop boundary for passing AOVs to usdview
or a DCC host and connecting them to final composition.

```text
FrameSnapshot -> merlin-metal
                   |-- native viewport -> CAMetalLayer / drawable
                   |-- headless        -> offscreen texture / readback
                   `-- Hydra bridge    -> HgiMetal / host composite
```

The presentation layer is separable from rendering. Its host bridge owns host
device/context discovery, AOV texture sharing or copying, completion sync,
format and color-space conversion, resize, frames-in-flight lifetime, composite
notification, fallback selection, and presentation-only telemetry. Backend
surface handles do not escape into Core.

For each supported OpenUSD and host version, the HgiMetal bridge validates
device access/injection, external texture import or native-handle access,
command queue and completion contracts, required usages and formats for color,
depth, `primId`, and `instanceId`, multisampling, resize, and HgiMetal selection
conditions. It relies only on public contracts.

The Metal host bridge selects, in order:

1. direct sharing on a compatible Metal device;
2. a Metal-local texture copy into a host-owned texture; or
3. CPU readback as the universal compatibility and debug fallback.

The first complete bridge displays Metal AOVs in a Hydra host, supports GPU
copy and CPU fallback, uses direct sharing when the public host contract permits
it, preserves AOV identity and resize/lifetime safety, shares one
`FrameSnapshot` path with the native viewport, and reports presentation cost
separately. Immediate zero-copy support in every DCC is not a requirement.

The same presentation-adapter boundary can contain an independently measured
HgiVulkan bridge. Vulkan and Metal interop details remain separate even when
their fallback and telemetry meanings are shared.

## Validation and telemetry

Common CI covers Core and snapshot contracts, resource identity, shader module
parsing, reflection/layout contracts, deterministic shader keys, MaterialX
translation, and backend-neutral lifetime behavior. Vulkan CI additionally
compiles and validates SPIR-V, creates pipelines, renders offscreen references,
compares bindless and fallback paths, and records GPU telemetry. Metal compile
and MSL compiler gates begin before the Metal backend; Metal execution CI later
adds pipeline, offscreen image, argument-buffer, lifetime, and Apple Silicon
performance tests.

The same `FrameSnapshot` is used for Vulkan/Metal AOV differential tests.
Color comparisons declare color space and numerical tolerance; depth, normal,
picking, and ID outputs use exactness appropriate to their contract, with ID
AOVs exact by default.

Common telemetry includes extraction and snapshot time, uploaded bytes,
allocation and residency changes, resource-table updates, compilation and
pipeline creation, GPU upload/render duration, completion wait, readback,
presentation, frame percentiles, and cache hits. Vulkan retains descriptor,
barrier, queue-transfer, timeline, validation, and memory-budget diagnostics;
Metal retains argument-buffer, encoder, heap-residency, drawable-wait,
command-buffer, and counter-sample diagnostics.

## Sequencing and non-goals

The Vulkan persistent-residency milestone, Slang migration/Metal compile gate,
and backend contract/native Vulkan viewport boundary are complete. The ordered
delivery continues with the MaterialXGenSlang prototype; Metal residency, native presentation,
and HgiMetal presentation; then add Gaussian and the later GPU-driven,
Visibility, production MaterialX, and meshlet work. The detailed release gates
are in the [roadmap](../roadmap/backlog.md).

The initial effort does not attempt to support every GPU API, build a universal
RHI, make Vulkan and Metal internals identical, require a runtime compiler in
shipping packages, implement all MaterialX nodes, build a shader-graph editor,
guarantee bit-exact floating-point color across backends, or make HgiMetal the
renderer implementation.

New work must preserve these checks:

1. Is this a `RenderWorld`/`FrameSnapshot` meaning or a backend execution detail?
2. Does it have the same semantic meaning on Vulkan and Metal?
3. Can a mismatch remain backend-owned or capability-selected?
4. Can target-specific shader code be avoided or isolated?
5. Can the behavior be covered by differential and lifetime tests?
6. Does it preserve zero steady-state allocation, upload, descriptor/resource-
   table update, shader compilation, pipeline creation, and CPU wait where the
   scene is unchanged?
7. Does it preserve the generated-material/renderer-pass boundary?
