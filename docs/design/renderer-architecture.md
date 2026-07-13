# Renderer architecture

**Status:** v0.5.0 implementation candidate · **Last reviewed:** 2026-07-14

hdMerlin is a lightweight, host-neutral Vulkan raster renderer. It is not a DCC
plugin by itself: the product is the composition of a renderer core, an
offscreen-first Vulkan backend, a renderer-owned extraction layer, an independent
material compiler, and thin Hydra/DCC adapters.

## Dependency boundary

```text
DCC / usdview / headless source
            │
            ▼
Hydra or headless adapter
            │  normalize host data
            ▼
Scene model: RenderWorld
            │  commit / immutable snapshot
            ▼
Draw model: GPU Scene + DrawItem / DrawPacket
            │  frame plan
            ▼
Execution model: Vulkan backend
            │
            ▼
Offscreen RenderProduct
```

Required rules:

- Core public APIs do not expose `pxr::`, Vulkan, Qt, Houdini, Maya, or other DCC
  SDK types.
- Hydra dirty bits, topology, and primvar interpolation are normalized in the
  adapter before entering the scene model.
- The adapter owns the Hydra path-to-Merlin handle map.
- DCC discovery, UI, environment, and package metadata belong to integration
  packages rather than Core.
- The Vulkan backend owns no native window or swapchain.
- Tier 0 CPU readback remains the correctness reference when lower-copy host
  presentation is added.
- Dependencies point from adapters/integrations toward Core and backend, never
  from Core toward a host SDK.

## Four models

| Model | Responsibility | Must not own |
| --- | --- | --- |
| Scene | `RenderWorld`, handles, resource revisions, change contract | GPU allocations or Hydra state |
| Draw | Immutable frame snapshot, draw items, batches, GPU Scene | Host callbacks or DCC types |
| Material | Host-neutral `MaterialIR`, parameters, texture bindings | Raw MaterialX/Hydra networks |
| Execution | Frame plan, submission, completion, resource lifetime | Mutations to the scene model |

The target scene-to-GPU path is:

```text
mutable RenderWorld
    ↓ Commit()
immutable FrameSnapshot
    ↓ revision comparison / compilation
persistent GPU Scene + DrawPackets
    ↓ execution
Vulkan commands
```

The snapshot is the ownership boundary between host updates, renderer work, and
GPU frame latency. Request, submission, completion token, and resolve are
separate backend operations; frame targets and readback buffers remain owned by
their frame context until the single-use token resolves. See the
[execution lifetime contract](execution-lifetime.md).

## Vulkan baseline

- Vulkan 1.4 loader, headers, and a Vulkan 1.4 physical device with a graphics
  queue are the minimum backend requirements.
- Shader compilation uses `glslc` from a compatible Vulkan SDK.
- The backend uses persistent frame contexts and completion-based resource
  retirement.
- Validation/performance messages are renderer quality signals. Unrelated
  general loader/host diagnostics remain observable but are not counted as
  renderer validation failures.
- Missing devices, validation layers, or optional capabilities have explicit
  capability/skip semantics in tests.

## Material and shader variants

The implemented host-neutral material boundary is:

```text
MaterialX document ─────┐
Hydra material network ┼─> MaterialIR ─> shader variant ─> SPIR-V
Future material source ┘
```

- `MaterialIR` contains host-neutral parameters, texture/sampler bindings,
  alpha/double-sided state, and feature flags; resource handles supply stable
  identity and independent revisions.
- Shader variant keys contain vertex-color, texture, directional-light,
  alpha-mask, and double-sided feature classes. Per-material values use
  per-frame uniform buffers while per-draw transforms, normal matrices, feature
  masks, and IDs use push constants, so a value-only edit does not rebuild the
  pipeline.
- Textures and samplers are revisioned scene resources. The Vulkan backend
  uploads changed images, caches sampler objects, updates per-frame descriptors,
  and retires replaced GPU objects only after their last submission completes.
- The Hydra adapter translates a deliberate `UsdPreviewSurface`/`UsdUVTexture`
  subset into `MaterialIR`; headless scenes author the same Core resources
  directly.
- MaterialX compilation happens before draw time and produces version-aware
  SPIR-V, reflection, and cache metadata; raw source graphs do not enter Core.
- Missing/stale texture or sampler bindings and unsupported alpha blending
  produce structured extraction fallbacks rather than silent corruption.

## Performance contract

The representative user-facing measures are:

1. time to first frame;
2. steady-state frame time;
3. scene edit to visible pixel latency.

CPU and GPU timelines are recorded separately. Baselines include median, P95,
P99, maximum, and frame-spike count where timing is stable enough to compare.
Structural counters are preferred in normal CI: draw and triangle count,
upload/readback bytes, allocation/buffer/image/descriptor/pipeline count, cache
hit rate, CPU/VRAM use, and temporary allocations.

Versioned benchmark fixtures should cover empty, triangle, indexed cube, 10,000
small objects, a 1,000,000-triangle mesh, resource-specific edits, many
instances/materials/textures, repeated resize, 4K offscreen rendering, a
long-running static scene, and usdview first frame/navigation.

Optimization follows measurement. In particular, lower-copy presentation is
not selected until usdview timings separate Hydra Sync, snapshot and GPU scene
work, command recording/submission, GPU execution, readback, RenderBuffer
resolve/map, Hgi upload, and host presentation. Tier 0 CPU readback remains the
reference path after any faster presentation adapter is added.

## Permanent validation gates

- Debug/Release, Vulkan ON/OFF, and Core-only builds.
- Forbidden-dependency scan and warning-free compilation.
- Deterministic extraction and headless rendering.
- Clean Vulkan validation on a supported GPU runner.
- Empty scene, indexed mesh, multiple frames, resize, and resource removal.
- NaN/Inf, row pitch, origin, color space, depth, and completion validation.
- Detection of unnecessary static-scene upload/allocation/pipeline creation.
- Build-tree and install-tree execution.
- Hydra discovery, delegate/RenderBuffer lifetime, first frame, camera,
  transform, visibility, resize, multiple mesh, and repeated-edit coverage.

## Non-goals for the foundation releases

- CPU renderer fallback or OpenGL/Metal backends.
- Full USD Imaging parity.
- Arbitrary OSL/custom MaterialX code nodes.
- Complete volume, hair, subdivision, motion-blur, or advanced-transparency
  support.
- Early bindless, multi-draw indirect, meshlet, or path-tracing work without
  benchmark evidence.
- DCC-specific workarounds in Core.
