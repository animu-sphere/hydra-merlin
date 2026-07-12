# Backlog

Ordered but unscheduled work. The next release and active carry-over work are in
[current.md](current.md); completed scope will move to [releases/](../releases/).

Legend: ⬜ not started

## Milestone ladder beyond v0.1.0

- ⬜ **v0.2.0 — resource-granular GPU scene.** Define an immutable
  `FrameSnapshot`; split mesh geometry, material, instance, and draw records;
  key extraction and GPU caches by handle/generation/revision; implement
  transform-, visibility-, and material-only updates; share geometry across
  instances; validate removal across frame latency; and add a device-local
  staging ring, dirty-range transfer, and buffer suballocation. Static scenes
  must produce zero upload bytes and zero pipeline creation after warm-up.
- ⬜ **v0.3.0 — practical Hydra mesh input.** Normalize normals, display color,
  opacity, UVs, indexed and face-varying primvars; replace convex fan assumptions
  with deterministic robust triangulation and malformed-topology diagnostics;
  add native instancing, authored material binding, primId/instanceId AOVs, and
  large-mesh/many-small-mesh/repeated-edit regression coverage.
- ⬜ **v0.4.0 — execution and render-product lifetime.** Separate render request,
  submit, completion token, and resolve; document ownership of frame contexts,
  targets, and readback buffers; classify timeout/device-lost/unsupported errors;
  select AOVs and CPU readback per request; and save color/depth/primId
  expected/actual/diff artifacts with PNG/EXR sinks.
- ⬜ **v0.5.0 — MaterialIR and basic shading.** Introduce host-neutral graph
  identity, parameter blocks, texture/sampler bindings, alpha mode, and
  double-sided state; add texture, descriptor, shader, and pipeline caches; render
  normals, UVs, textures, vertex color, and a directional light through the same
  headless and Hydra path; return structured fallback diagnostics.
- ⬜ **v0.6.0 — MaterialX MVP.** Load and validate documents, normalize graphs,
  translate Standard Surface base color/metallic/roughness plus image textures,
  UV transforms, and normal maps into MaterialIR, and emit versioned SPIR-V,
  reflection, and metadata artifacts with compiler/source hashes in cache keys.

## Future phases

- ⬜ **Viewport fundamentals.** Alpha mask/blend, double-sided rendering, dome
  light, shadows, selection foundations, and measured culling improvements.
- ⬜ **Low-copy host presentation.** Negotiate RenderBuffer formats and evaluate
  HgiVulkan/external-memory paths while preserving Tier 0 CPU readback.
- ⬜ **Measured GPU optimization.** Consider bindless tables, indirect draws,
  mesh shaders, or meshlets only after counters and fixed-GPU baselines show a
  concrete need.
- ⬜ **DCC integration.** Add separately packaged Houdini Solaris, Husk, Hydra 1,
  and Maya Hydra integrations in that order. Each package owns only environment
  setup, package metadata, settings UI, and host smoke tests.

## Cross-cutting open items

- ⬜ **Diagnostics and capabilities.** Define a Core callback/diagnostic sink, a
  host-neutral capability schema, and backend-specific extensions.
- ⬜ **Resource ownership.** Specify external render-product lifetime, async
  ownership, device-local upload ownership, and frame-in-flight removal.
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
