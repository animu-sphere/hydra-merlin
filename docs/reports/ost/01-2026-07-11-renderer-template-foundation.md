# OST dogfooding report — renderer-template foundation with hdMerlin

- Date: 2026-07-11
- Scope: initial vertical slice; persistent frame/extraction/depth work; Hydra
  2 Sync, RenderBuffer, install-tree usdview, and stable-update regression
- Focus: renderer-platform boundaries suitable for composable OST templates

## Conclusion

The hdMerlin bootstrap validated template boundaries before renderer-specific
features. Separate CMake targets for `RenderWorld`, extraction, Vulkan, headless,
and optional Hydra express three important policies as dependency structure:
the core is host-neutral, the backend is offscreen-first, and Hydra is one scene
ingress rather than the renderer architecture.

A single renderer template would own too much policy. OST should compose at
least a renderer-core template, a Vulkan-backend template, and a
render-validation template, with Hydra and DCC integrations layered explicitly
above them.

The Hydra slice now connects plugin discovery, mesh/transform/visibility/camera
Sync, color/depth RenderBuffers, install staging, and usdview rendering to the
same Core/extraction/Vulkan path used by headless. One install-tree live-edit
sequence exercises points, topology, camera, transform, visibility, resize, and
multiple meshes. Resource revisions and classified `ChangeAspect` values preserve
which resource changed. Instancing, authored materials, format negotiation, and
zero-copy interop are not mature enough to become template policy.

## Validated composition

```text
merlin-headless
  ├─ merlin-render-world
  ├─ merlin-render-extraction
  └─ merlin-vulkan

hdMerlin (opt-in OpenUSD plugin)
  ├─ merlin-render-world
  ├─ merlin-render-extraction
  ├─ merlin-vulkan
  └─ OpenUSD hd/plug + Hdx CPU-to-Hgi presentation
```

Validated properties include:

- Core public headers contain no OpenUSD, Hydra, Vulkan, Qt, or DCC SDK types.
- Typed generation handles turn create/update/remove operations into a
  revisioned, aspect-aware `ChangeSet` and detect stale handles.
- Vulkan renders without a window or swapchain into persistent color/depth
  images and returns Tier 0 CPU readback.
- AOV name, pixel format, origin, color space, row pitch, payload, depth, and
  completion metadata have a common validator.
- Headless owns CLI and output-file policy; Hydra owns only host mapping, dirty
  bit translation, AOV binding, and CPU RenderBuffer presentation.
- `MERLIN_ENABLE_HYDRA2` keeps OpenUSD out of normal Core/Vulkan dependencies.
- Rendering after commit does not re-read Hydra scene state.

## Template candidates

### `ost-template-renderer-core`

Reusable candidates:

- typed generation handles and stale-handle detection;
- minimal scene records and create/update/remove/commit revisions;
- pre-commit change coalescing that preserves aspect classification;
- serialized-handle reconstruction followed by generation validation;
- a backend-facing `ChangeSet` and host-neutral render-product contract;
- a renderer-owned extraction extension point between Core and backend;
- a CMake boundary that can be scanned for forbidden host dependencies.

Template parameters should cover project/namespace/target names, extra record
attributes, public/shared linkage, ABI export macros, exception policy, and
logging hooks. Raster draw construction, MaterialX feature policy, GPU
residency, and pipeline variants remain renderer-owned.

### `ost-template-vulkan-backend`

Reusable candidates:

- Vulkan loader/device/graphics-queue initialization and capability records;
- two-to-eight persistent frame contexts with per-frame command resources;
- timeline/fence completion, frame-slot reuse, and deferred destruction;
- `VK_EXT_debug_utils` validation/performance reporting;
- persistent offscreen color/depth attachments, pipeline, and readback buffers;
- scene-revision upload suppression and build-time SPIR-V compilation;
- GPU smoke and validation tests registered through CTest.

Further dogfooding is required for split queue selection, a device-local staging
ring, descriptor/transient/persistent allocators, multiple AOVs, pipeline-cache
persistence, and device-lost categorization.

### `ost-template-render-validation`

The validated bring-up loop is RenderWorld commit → deterministic extraction →
vertex/index upload → indexed draw → color/depth readback → image sink. It proves
depth numerically, checks top-left/tight-row metadata, verifies that an unchanged
scene uploads once, and supports validation-capability skips.

A reusable validation template should additionally provide PNG/EXR sinks,
golden-image tolerances, expected/actual/diff artifacts, structured validation
messages, device/driver/API metadata, explicit skip reasons, and a deterministic
64×64 bootstrap scene.

### `ost-template-hydra-adapter`

The OpenUSD 26.05 slice directly exercised `HdRendererPlugin`,
`HdRenderDelegate`, `HdRenderPass`, mesh/camera prims, color/depth RenderBuffers,
`plugInfo.json`, install staging, and a `testusdview` host regression.

Important reusable findings:

- Keep Hydra paths and dirty bits out of Core; the adapter owns handle mappings
  and the synchronization boundary protecting a non-thread-safe scene model.
- Normalize points/topology/transform/visibility in the adapter. Fan
  triangulation and fallback material are bootstrap policies, not OST policy.
- Select the active camera and AOV bindings explicitly at the render-pass
  boundary because they are frame/host state.
- Gf row-vector and renderer column-vector conventions require a semantic
  transpose; storage-order conversion alone can move translation into clip-space
  `w` and produce zero coverage despite valid draw packets.
- CPU `HdFormatUNorm8Vec4` color and `HdFormatFloat32` depth buffers must validate
  resize, map count, resize-while-mapped rejection, convergence, and same-frame
  multi-AOV writes.
- Keep Hydra 1 and Hydra 2 in separate binaries sharing only the Core ABI.
- Test discovery, delegate creation, RenderBuffer lifetime, GPU rendering, and
  host presentation as separate assertions and artifacts.
- OpenUSD build configuration and C++ runtime ABI must match the plugin. A
  Release-only SDK loaded by a Debug `/MDd` consumer can corrupt STL values such
  as plugin paths.
- Windows builds need `NOMINMAX`; runtime helpers must consider SDKs that place
  OpenUSD DLLs in either `bin` or `lib`.
- Multi-config resources belong beside `$<TARGET_FILE_DIR:...>`; validate actual
  delegate creation because a discoverable `plugInfo.json` can still contain an
  invalid module path.
- Install smoke tests stage the plugin, resources, shaders, and USD scene without
  adding the build tree to plugin search paths.
- Process success from `usdview --quitAfterStartup` does not prove rendering.
  Use an event-loop-aware harness plus completion logs, depth coverage, and a
  viewport capture.

Valuable OST helpers include OpenUSD configuration/ABI checks, plugin resource
layout, discovery-only CTest, runtime-path composition, first-frame reporting,
matrix-convention conversion tests, Tier 0 RenderBuffer creation, and matching
multi-config build/install layouts.

## Gaps found by template dogfooding

1. Core/Vulkan package export and minimal OpenUSD runtime layout are validated;
   Hydra/headless exported-target policy and a common generated layout are not.
2. Core lacks a host-neutral diagnostic/logging sink.
3. Persistent frame contexts exist, but upload-ring and async render-product
   ownership are undefined.
4. A common capability schema and backend extensions are undefined.
5. RenderBuffer resize/map lifetime is covered, but external lifetime and format
   negotiation are not.
6. Installed SPIR-V has no generic artifact manifest/versioning convention.
7. OpenUSD configuration/ABI compatibility is not checked at configure time.
8. Discovery/delegate/RenderBuffer/GPU/host assertions have no common
   machine-readable result schema.

These gaps are tracked as incomplete work in the
[roadmap](../../roadmap/current.md) and [backlog](../../roadmap/backlog.md).

## Recommended composition

```text
ost-template-renderer-core
        ↑
ost-template-vulkan-backend
        ↑
ost-template-render-validation
        ↑
ost-template-hydra-adapter
        ↑
ost-template-dcc-integration
```

The renderer-owned extraction/build layer sits between Core and backend. A
template should provide an extension point and target placeholder, not freeze a
specific renderer's draw policy.

## Assessment

- `ost-template-renderer-core`: ready to begin extraction for handles, resource
  revisions, aspect-aware coalescing, render settings, AOV contracts, and the
  extraction seam.
- `ost-template-vulkan-backend`: persistent contexts, completion, and validation
  callback are candidates; wait for upload-ring and async-ownership evidence.
- `ost-template-render-validation`: color/depth smoke, CPU image validation,
  capability skips, and long validation loops are candidates; golden/diff
  artifacts remain open.
- `ost-template-hydra-adapter`: discovery, delegate creation, basic mesh/camera
  Sync, Tier 0 RenderBuffer, install-tree smoke, and stable-update regression are
  candidates; do not freeze instancing, materials, format negotiation, or
  zero-copy policy yet.
- `ost-template-dcc-integration`: wait for additional adapter/package evidence.
- `ost-template-material-compiler`: outside this slice; validate it separately
  with the MaterialX MVP.
