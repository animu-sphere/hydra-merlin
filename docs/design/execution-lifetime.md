# Execution and render-product lifetime

**Status:** v0.9.0 implementation · **Last reviewed:** 2026-07-17

The backend-neutral boundary separates immutable input, GPU submission,
completion, optional presentation, and CPU product resolution:

```text
RenderRequest(shared FrameSnapshot, extent, products, presentation target)
    │ Submit: validate, synchronize resources, record, queue
    ▼
CompletionToken(renderer identity, monotonic submission value)
    │ IsComplete or Resolve(timeout)
    ▼
RenderResult(selected CPU products, timings, common counters)
```

`Renderer::Render` remains a synchronous compatibility wrapper. It requests all
four implemented AOVs and immediately resolves the returned token. New callers
use the explicit API so production and CPU transfer are visible in the request.

## Ownership

| Resource | Owner and lifetime |
| --- | --- |
| `FrameSnapshot` | Shared by `RenderRequest`; required through `Submit`, after which recorded commands no longer refer to snapshot CPU storage |
| Frame command pool/buffer | One backend `FrameContext`; unavailable from successful `Submit` until its token is resolved |
| Attachment images/framebuffer | Owned by that frame context and retained across matching requests; never reused while its token is unresolved |
| Host-visible readback buffers | Owned by the same frame target; mapped only after completion and retained until resolve copies into caller-owned vectors |
| Logical presentation target | Backend-specific and owner-checked; contains no native handle in Core |
| Vulkan surface/swapchain | Owned by the Vulkan backend; the GLFW adapter only supplies required instance extensions and a surface-creation callback |
| Acquired swapchain image | Retained from acquire through the graphics submission and present; its per-image completion semaphore is not reused until the image is acquired again |
| Geometry ranges | Owned by the persistent GPU scene; an edit versions a range while older submissions may read it, then retires the old range at its last completion value |
| Staging regions | Owned by the staging ring through the completion value of the submission that consumes them |
| `RenderResult` payloads | Owned entirely by the caller and independent of renderer/frame lifetime |

The renderer destructor waits for device idle before destroying outstanding
backend resources. A token is renderer-specific and single-use. A timeout does
not consume it; successful resolve does. If every configured frame context has
an unresolved token, another submit returns `resource-busy` rather than
silently invalidating a target. Renderer calls are externally synchronized;
the API does not claim concurrent calls from multiple CPU threads.

## Products and readback

Each `RenderProductRequest` selects one of color, depth, primId, or instanceId
and independently enables Tier 0 CPU readback. The current fixed shader pass may
write attachments that are not requested, but it records image-to-buffer copies,
maps memory, allocates result payloads, and counts readback bytes only for the
requested CPU products. Unsupported and duplicate AOV requests are rejected.

Hydra derives CPU readback requests from host AOV bindings. Native viewport
frames request a GPU-only color product and add CPU color/ID products only for
screenshots or picking. usdview retains Tier 0 RenderBuffer readback. Headless
requests color/depth, adding IDs when comparison artifacts are requested.

## Error classification

`RendererError` exposes a stable `RendererErrorCode`, operation name, and native
backend code where present:

- `invalid-request` and `invalid-token` identify caller contract failures;
- `resource-busy` means every frame target is retained by an unresolved token;
- `timeout` leaves the completion token valid for a later resolve;
- `device-lost` identifies `VK_ERROR_DEVICE_LOST`;
- `unsupported` covers missing Vulkan/format capabilities and unsupported AOVs;
- `backend-unavailable` identifies failed explicit or automatic backend selection;
- `backend-failure` covers other Vulkan or backend operation failures.

## Regression artifacts

`SaveComparisonArtifacts` requires color, depth, primId, and instanceId CPU
products at one extent and writes twelve deterministic paths:

- `color-{expected,actual,diff}.png` as RGBA8 PNG;
- `depth-{expected,actual,diff}.exr` as float OpenEXR;
- `primId-{expected,actual,diff}.exr` as unsigned-integer OpenEXR;
- `instanceId-{expected,actual,diff}.exr` as unsigned-integer OpenEXR.

The PNG and uncompressed scanline EXR sinks have no optional image-library
dependency. The headless `--artifact-dir` path compares its first and final
unchanged frames, while tests also exercise intentionally different values so
the diff files and mismatch result remain covered.
