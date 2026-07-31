# Hgi host presentation policy

**Status:** accepted implementation policy
**Applies from:** v0.13.0
**Primary implementation order:** HgiVulkan, then HgiMetal

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

## HgiVulkan delivery plan

### v0.13.0: Hgi-owned targets and GPU copy

The Vulkan bridge is implemented first because the Vulkan backend, offscreen
AOVs, completion model, viewport comparison path, and Hydra validation assets
are already mature on Windows and Linux.

1. Establish Hgi-owned destination textures and their format, usage, extent,
   sample-count, color-space, destruction, and frames-in-flight contracts.
2. Keep Tier 0 operating through those targets as the parity reference.
3. Add selected-AOV Vulkan GPU copy from Merlin-owned images to HgiVulkan
   destinations, with explicit barriers and bridge completion. No CPU readback
   or CPU upload occurs on this path.
4. Report capability selection, source/destination metadata, bytes, encode and
   wait cost, fallbacks, target recreations, CPU transfers, and GPU-copy time.

The implementation establishes the public host boundary used by both validated
OpenUSD lines. `HdRenderDelegate::SetDrivers` discovers the application-owned
Hgi render driver and `HdRenderBuffer::GetResource` publishes an Hgi-owned
destination texture. Hgi blit commands provide the Tier 0 CPU-to-Hgi fallback
without a queue- or device-idle wait. When the package also exports the
validated native `hgiVulkan` target, the adapter borrows its Vulkan 1.3 device
and graphics queue for Merlin's conventional renderer path and records a
same-device color-image copy instead. A missing driver, non-Vulkan driver,
disabled bridge, missing native package target, or operational failure retains
the CPU RenderBuffer path with a structured rejection. Merlin-owned Vulkan
contexts remain on the Vulkan 1.4 product baseline.

The slice publishes a target for the 8-bit color AOV alone. Color is the AOV a
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

The OpenUSD 26.05 and 26.08 runtime smokes exercise this native path across the
full regression and resize sequence. Each baseline reports one exported color
AOV and three CPU-readback AOVs, host trace evidence for GPU copy, no color
RenderBuffer Map or CPU upload, non-zero bridge completion, and zero coarse
waits. Unsupported package compositions continue to compile the public Tier 0
fallback without a native HgiVulkan dependency.

The paired comparison fixture executes the same 13 phases through Tier 0 and
HgiVulkan, records bounded image differences plus performance evidence in
`merlin-hydra-presentation-comparison/v1`, and verifies every named GPU-copy
phase avoids color Map/upload. The OpenUSD 26.08 Windows evidence reduced
baseline CPU readback by 1,289,520 bytes per frame and measured representative
median readback-plus-transfer time from about 19.1 ms to 14.2 ms while the
maximum changed pixel fraction stayed at 0.1862% on rasterized triangle edges.

The release exits only when color, depth, `primId`, and `instanceId` match Tier
0 semantics; resize and target retirement are completion-safe; camera-only
frames avoid CPU readback/upload; unsupported configurations retain Tier 0; and
the bridge has measured Tier 0 comparison evidence without coarse device waits.

### v0.13.1: optional direct-path hardening

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

The implemented capability evaluator keeps direct-share rejection independent
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

The color AOV now declares sampled usage and exports usage, optimal tiling,
device-local memory, exclusive sharing, queue family, and the existing
format/layout/access/completion metadata. GPU copy validates this expanded
source contract as well, so the hardening does not create a less-checked
fallback. External memory/semaphores remain unjustified: GPU copy is available,
and the missing public host import/consumption contract would not be repaired by
adding cross-device handle transport.

Self-hosted GPU capability
[run 30655056809](https://github.com/animu-sphere/hydra-merlin/actions/runs/30655056809)
validated this decision at commit `8a2b4a4`. The OpenUSD 26.05 and 26.08 jobs
both passed bridge/RenderBuffer tests and the paired 13-phase Tier 0/HgiVulkan
usdview regressions. Vulkan 1.4 Debug and Release also passed the headless,
viewport, lifetime, validation, descriptor, packaging, and evidence gates.
Both HgiVulkan jobs retained GPU copy with the direct-share rejection present;
no direct-path performance claim is made because no public consumable resource
can be constructed.

## HgiMetal follow-up

v0.14.0 implements the same logical contract for Metal: Tier 0 CPU fallback,
Metal-local texture copy, then optionally same-`MTLDevice` texture sharing.
Metal device identity, texture storage/usage/pixel format, command queue and
buffer completion, target lifetime, resize generation, SDR sRGB/Display P3,
and HDR rejection are validated independently. Native Metal viewport and Hydra
presentation remain separate consumers of the same renderer output.

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
