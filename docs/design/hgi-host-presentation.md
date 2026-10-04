# Hgi host presentation policy

## Purpose and boundary

Hydra presentation currently uses the universal Tier 0 path:

```text
Merlin backend AOV -> GPU readback -> HdRenderBuffer -> CPU upload -> host Hgi texture -> Hydra composite
```

It remains the reference and fallback for every backend and host. A host bridge
reduces the GPU-to-CPU-to-GPU round trip for interactive presentation, but it
does not turn Hgi into the renderer RHI. Core, `merlin-vulkan`, and
`merlin-metal` continue to own their native devices, resources, submissions,
and synchronization. The Hydra adapter owns host integration.

```text
Core and backend-neutral render contract
    ├─ merlin-vulkan
    └─ merlin-metal
             ↓ backend-owned AOV export and completion metadata
Hydra adapter
    ├─ CPU RenderBuffer fallback
    ├─ HgiVulkanBridge
    └─ HgiMetalBridge
```

The bridges share AOV semantics, extent, format category, color space,
transfer-mode selection, completion state, fallback reason, telemetry, and
resize generation. They do not share native handles or synchronization:
`VkImage`, `VkDevice`, semaphores, queue ownership, layouts, `MTLTexture`,
and Metal command completion remain private to their respective implementations.

## Transfer tiers

| Tier | Mode | Policy |
| --- | --- | --- |
| 0 | `CpuReadback` | Permanent universal fallback and image-reference path. |
| 1 | `GpuCopy` | First low-copy path; a backend-local copy into an Hgi-owned destination. |
| 2 | `DirectSharedResource` | Same-logical-device sharing only after public-contract, lifetime, and benchmark validation. |
| 3 | `ExternalInterop` | Reserved for demonstrated multi-device/host demand; not an initial release requirement. |

The bridge transfers only requested AOVs. Color, depth, `primId`, and
`instanceId` retain independent format, clear-value, identity, and color-space
contracts. SDR sRGB is the initial color baseline; unsupported HDR or target
formats produce a structured rejection and select Tier 0 where possible.

Vulkan Tier 0 AOV buffers require host-visible, host-coherent memory and prefer
host-cached memory within the resource's compatible memory-type mask. When no
compatible cached coherent type exists, allocation retains the first compatible
coherent type. Cached noncoherent memory is not selected: the existing CPU read
path waits for renderer completion and relies on coherence without explicit
cache invalidation. The preference does not change requested AOVs, payloads,
target lifetime, or upload-buffer placement. Vulkan defines host caching and
coherence as separate properties; see the
[memory property reference](https://docs.vulkan.org/refpages/latest/refpages/source/VkMemoryPropertyFlagBits.html).

## Common contract

Each bridge exposes a host-neutral result model equivalent to:

```text
capabilities: supported, device relationship, available transfer modes,
              selected mode/path, rejection reason
request:      AOV set, source/destination metadata, extent, color spaces,
              renderer completion, frame index, resize generation
result:       submitted/completed state, selected mode, transferred bytes,
              encode/wait time, fallback reason, bridge completion
```

Capability discovery is cached and reevaluated only when the host or backend
device is created or replaced, the Hgi/OpenUSD runtime changes, a target is
recreated, or a resize changes target format or usage. It is never a
per-frame probe.

Completion has three distinct stages:

```text
Merlin render completion -> bridge transfer completion -> host composite consumption -> safe reuse/retirement
```

The bridge must not equate renderer completion with host consumption. It must
retain source and destination resources until their respective completion rules
allow reuse. Per-frame `vkDeviceWaitIdle`, queue-wide idle waits, speculative
host-target lifetimes, implicit layout transitions, and immediate destruction
of in-flight resize targets are prohibited.

## HgiVulkan transfer contract

### Hgi-owned targets and GPU copy

The Vulkan bridge uses the backend's offscreen AOV and completion model while
keeping the host destination and lifetime separately owned.

1. Establish Hgi-owned destination textures and their format, usage, extent,
   sample-count, color-space, destruction, and frames-in-flight contracts.
2. Keep Tier 0 operating through those targets as the parity reference.
3. Add selected-AOV Vulkan GPU copy from Merlin-owned images to HgiVulkan
   destinations, with explicit barriers and bridge completion. No CPU readback
   or CPU upload occurs on this path.
4. Report capability selection, source/destination metadata, bytes, encode and
   wait cost, fallbacks, target recreations, CPU transfers, and GPU-copy time.

The public host boundary uses `HdRenderDelegate::SetDrivers` to discover the application-owned
Hgi render driver and `HdRenderBuffer::GetResource` publishes an Hgi-owned
destination texture. Hgi blit commands provide the Tier 0 CPU-to-Hgi fallback
without a queue- or device-idle wait. When the package also exports the
validated native `hgiVulkan` target, the adapter borrows its Vulkan 1.3 device
and graphics queue for Merlin's conventional renderer path and records a
same-device color-image copy instead. A missing driver, non-Vulkan driver,
disabled bridge, missing native package target, or operational failure retains
the CPU RenderBuffer path with a structured rejection. Merlin-owned Vulkan
contexts remain on the Vulkan 1.4 product baseline.

The bridge publishes a target for the 8-bit color AOV alone. Color is the AOV a
host present task consumes as a texture, while depth, `primId`, and
`instanceId` are read through `HdRenderBuffer::Map`; uploading those would
spend bandwidth no consumer collects. A published target is bound to the Hgi
that created it: a driver declaration that would swap that Hgi while targets
are outstanding is rejected as `driver-swap-rejected` rather than retiring
those textures through a different device. Re-declaring the same driver is the
one point at which an operational rejection is re-evaluated; otherwise a
rejection holds for the delegate's lifetime instead of being retried per
frame.

Hgi backend availability remains a package-composition capability, not a
version guarantee: a package can provide public Hgi without shipping
`hgiVulkan`. The Tier 0 boundary therefore checks the runtime driver token and
API name, while the native copy is compiled only when CMake can consume the
package's `hgiVulkan` imported target. Merlin exposes selected color, depth,
`primId`, and `instanceId` images through a Vulkan-only optional backend
interface. Each export carries explicit native format, transfer-source layout,
stage/access, aspect, queue family, extent, and renderer completion; its
move-only lease prevents frame-target reuse after Resolve. The native bridge
validates physical/logical device, queue family, format, extent, layout,
aspect, and sample count, records `vkCmdCopyImage`, restores the Hgi target
layout, and returns the lease only from the Hgi command-buffer completion
callback.

Runtime validation covers the regression and resize sequence: exported color,
CPU-readback depth/ID AOVs, GPU-copy host traces, no color RenderBuffer Map or
upload, bridge completion, and no coarse waits. Unsupported package
compositions retain the public Tier 0 fallback without a native HgiVulkan
dependency. The [release record](../releases/v0.13.0.md) retains the measured
evidence.

The paired comparison fixture runs Tier 0 and HgiVulkan through the same
phases and verifies bounded image differences, transfer costs, and absence of
color Map/upload in GPU-copy phases. Measured release evidence belongs in the
[HgiVulkan release record](../releases/v0.13.0.md).

Native HgiVulkan now copies bound depth/ID products as well as color. A D32
depth target supplies host depth composition; R32 ID targets preserve the
renderer bits. Selection and CPU consumers call Map to download the current
non-color target once. The target owns its lifetime through the submission
wait; staging writes are ordered on reuse. A new GPU copy invalidates the CPU
cache. Mixed/multiple consumers and bridge failures retain eager Tier 0.
`hgi_cpu_download_count`, `hgi_cpu_download_bytes` and `hgi_cpu_download_ns`
are cumulative bridge telemetry, separate from renderer image readback.
HgiMetal follows the same demand-driven non-color contract. Native Hgi color
targets declare attachment usage because Hdx selection composites into them.

The bridge requires color, depth, `primId`, and `instanceId` to match Tier
0 semantics; resize and target retirement are completion-safe; camera-only
frames avoid CPU readback/upload; unsupported configurations retain Tier 0; and
the bridge has measured Tier 0 comparison evidence without coarse device waits.

### Optional direct-path gates

Direct sharing is a separately gated capability, not an automatic optimization.
Before enabling it, the bridge verifies physical-device UUID, logical-device
identity, queues and ownership, API and extension compatibility, format/usage,
sample count, tiling, memory constraints, public host consumption guarantees,
and completion-safe resize/destruction. The same physical GPU is insufficient:
two logical Vulkan devices must never exchange native handles as though they
were one device. GPU copy remains the fallback.

External memory and semaphores are considered only when GPU copy is unavailable
or materially slower, a supported host requires them, public APIs make the path
maintainable, and recurring hardware evidence exists.

The capability evaluator keeps direct-share rejection independent
from the selected transfer fallback. It requires every gate affirmatively:
physical and logical device identity, compatible queue ownership, API and
extensions, format/usage, single sampling, tiling and memory constraints,
public texture import, retained host consumption, completion retention,
resize-safe retirement, and an available direct implementation. Default or
partially populated requirements reject the path, and every rejection has a
stable diagnostic name.

On the validated OpenUSD 26.05 and 26.08 packages, the evaluation stops at
`public-texture-import-unavailable`. Public `Hgi::CreateTexture` allocates an
Hgi-owned texture, and `CreateTextureView` aliases another Hgi texture; neither
imports a renderer-owned `VkImage`. `GetRawResource` exposes an existing
resource in the opposite direction and does not transfer Hgi ownership or
provide a host-consumption completion contract. Constructing or mutating a
private `HgiVulkanTexture` cannot be a maintained adapter contract. Merlin
therefore keeps GPU copy selected and records this rejection rather than
publishing a handle with ambiguous destruction or frame-target reuse.

The color AOV declares sampled usage and exports usage, optimal tiling,
device-local memory, exclusive sharing, queue family, and the existing
format/layout/access/completion metadata. GPU copy validates this expanded
source contract as well, so the hardening does not create a less-checked
fallback. External memory/semaphores remain unjustified: GPU copy is available,
and the missing public host import/consumption contract would not be repaired by
adding cross-device handle transport.

Release-time validation evidence for direct-share rejection is in the
[HgiVulkan direct-path release record](../releases/v0.13.1.md).

## HgiMetal follow-up

The same logical contract applies to Metal: Tier 0 CPU fallback,
Metal-local texture copy, then optionally same-`MTLDevice` texture sharing.
The adapter publishes Hgi-owned color, depth and ID targets, while the Metal
renderer exports a leased AOV texture plus an `MTLSharedEvent`; the Hgi command buffer
waits on that event before copying and releases the lease only from its own
completion callback. This keeps renderer completion, bridge completion, and
host consumption distinct without a per-frame queue/device idle wait.

Color and Depth32Float use texture blits. Renderer R32Uint ID bits pass through
a retained private GPU buffer into the Hgi R32Sint texture. Non-color Map uses
an owned, aligned Metal shared buffer and waits for that command's completion;
the generic Hgi download cannot wrap unaligned RenderBuffer vector storage.
Repeated Map reuses the CPU version until the next copy or Tier 0 write.
Bridge download telemetry is distinct from renderer readback. Completion-handler
lease release is serialized with backend frame state; native command retention
keeps targets and private transfer buffers alive during retirement. HgiMetal's
projection Y reflection also flips Mesh winding when the reflection changes.

Metal device identity, texture storage/usage/pixel format, command queue and
buffer completion, target lifetime, resize generation, SDR sRGB/Display P3,
and HDR rejection are validated independently. Public HgiMetal exposes no
maintained renderer-texture import contract, so direct sharing remains a
diagnosed rejection and Metal-local copy remains the selected native path.
Native Metal viewport and Hydra presentation remain separate consumers of the
same renderer output.

## Verification and release evidence

Every bridge path requires Tier 0 image parity for all supported AOVs, resize,
hidden/minimized hosts, repeated target recreation, frames in flight, backend
selection, unsupported fallback, host shutdown, and explicit device-loss-style
failure. Debug and Release validation are required where the host supports
them.

Benchmarks record first frame, steady state, camera-only frames, resize, color
only, color plus depth, color plus ID AOVs, 4K, static, large-mesh, and
many-instance fixtures. They separate renderer cost from presentation cost and
compare Tier 0, GPU copy, and direct sharing when available. A low-copy path is
only claimed when it has a measurable benefit; the term “zero copy” is not a
success criterion by itself.

## Build and dependency rules

`MERLIN_ENABLE_HGI_VULKAN_BRIDGE` requires Hydra 2 and Vulkan;
`MERLIN_ENABLE_HGI_METAL_BRIDGE` requires Hydra 2 and Metal. Disabling either
bridge preserves CPU RenderBuffers. OpenUSD/Hgi remains outside Core and
backend-only targets, public backend APIs contain no Hgi type, and install-tree
Hydra consumers continue to validate the selected and fallback paths.
