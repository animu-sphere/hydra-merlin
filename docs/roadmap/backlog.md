# Backlog

Ordered work after the active v0.5.0 milestone in [current.md](current.md).
Shipped scope moves to the [changelog](../../CHANGELOG.md).

Legend: ⬜ not started

## Milestone ladder

- ⬜ **v0.5.1 — Hydra performance observability.** Break usdview frame cost into
  Hydra Sync, snapshot extraction, GPU scene update, command recording, queue
  submit, GPU execution, GPU-to-CPU readback, RenderBuffer resolve/map,
  CPU-to-Hgi upload, and host composite/present. Record requested/generated
  products, transfer bytes, waits, maps, resolves, host upload, and host timing.
  Add comparable static, camera, edit, 1M-triangle, 10k-mesh, 1k-instance, and
  AOV-combination fixtures. Color-only requests must perform zero depth/ID CPU
  readback. Exit requires stage-level bottleneck attribution, comparable
  benchmark artifacts, and CI detection of steady-state regression.
- ⬜ **v0.6.0 — MaterialX MVP.** Validate and normalize a deliberate Standard
  Surface subset: base color, metallic, roughness, normal, opacity, image,
  texcoord, UV transform, normal map, constant, multiply, and add. Translate the
  supported subgraph into MaterialIR, then generate deterministic SPIR-V,
  reflection, and metadata. Cache keys include graph, MaterialX/compiler/target/
  generator versions, source-template hash, and render feature mask. Unsupported
  nodes produce a structured fallback; raw MaterialX graphs never enter the Core
  scene model. Exit requires deterministic shader output, version-aware cache
  behavior, explicit unsupported-node diagnostics, and stable reference-scene
  image comparisons.
- ⬜ **v0.7.0 — Viewport essentials.** Deliver selection/picking first, then
  viewport and dome lighting, a single-light shadow MVP, measured frustum
  culling, and alpha blend. Region picking returns
  prim/instance/depth without reading the entire ID buffer back to the CPU. Exit
  requires prim/instance selection and highlighting plus a deterministic
  directional-shadow reference test.
- ⬜ **v0.8.0 — Low-copy presentation experiment.** Retain Tier 0 Vulkan-to-CPU
  RenderBuffer readback as the universal reference. If v0.5.1 shows readback and
  upload are dominant, evaluate HgiVulkan shared/external GPU resources before
  any host-specific Vulkan/OpenGL bridge. Document device ownership and
  synchronization, fall back safely, isolate host code in a presentation
  adapter, and require a measured win over Tier 0.

## DCC integration order

1. usdview and `testusdview` remain the reference host.
2. Houdini Solaris viewport integration.
3. Husk batch integration.
4. Hydra 1 compatibility.
5. Maya Hydra integration.

Before Solaris work begins, the renderer must have structured diagnostics,
OpenUSD compatibility checks, MaterialIR basic shading, deterministic
install-tree tests, usdview frame-time breakdown, a renderer-settings schema,
and versioned capability reporting. Integration packages own environment setup,
plugin discovery metadata, host settings UI, package metadata, and host smoke
tests; Core remains independent of every DCC SDK.

## Evidence-gated optimization

- ⬜ Consider bindless tables, indirect draws, GPU-driven rendering, meshlets,
  mesh shaders, and advanced transparency only after fixed-scene, fixed-GPU
  benchmark evidence demonstrates a need.
- ⬜ Preserve CPU readback for reference rendering, CI image comparison,
  headless testing, debugging, and fallback regardless of which presentation
  optimization is selected.

## Cross-cutting open items

- ⬜ **GPU capability matrix.** After the active Windows runner work, add durable
  Linux Vulkan/Hydra evidence and cover NVIDIA and AMD, with Intel when
  practical. Keep PR jobs portable; run rendering, image comparison, Hydra, and
  benchmark regression nightly or manually when hardware is scarce; require
  multi-vendor usdview/install validation for releases.
- ⬜ **Capabilities.** Define a versioned host-neutral capability schema with
  backend-specific extensions and explicit unsupported/fallback reporting.
- ⬜ **External presentation ownership.** Specify external-image lifetime,
  device ownership, synchronization, adapter negotiation, and safe fallback
  before implementing any low-copy bridge.
- ⬜ **Shader artifacts.** Define a generated shader manifest, versioning, and
  cache-compatibility contract.
- ⬜ **OpenUSD compatibility.** Detect shared/static mode, Debug/Release mismatch,
  MSVC runtime mismatch, runtime bin/lib locations, and plugin ABI compatibility.
- ⬜ **Build options.** Add independent headless, Hydra-test, example, validation,
  sanitizer, and Tracy options where separate control is justified.
- ⬜ **Exported products.** Resolve `Merlin::Hydra2` and `Merlin::Headless`
  packaging without making OpenUSD a transitive dependency of Core/Vulkan.
- ⬜ **OST template extraction.** Extract only boundaries proven by Merlin and a
  second consumer; keep renderer-specific extraction, materials, upload policy,
  and zero-copy policy out of templates until separately demonstrated.
