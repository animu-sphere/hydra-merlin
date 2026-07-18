# Current

Only incomplete work for the next release milestone and active carry-over is
listed here. Completed pre-release detail is retained in the
[delivery history](../reports/delivery-history.md); shipped versions are
recorded in the [changelog](../../CHANGELOG.md).

## Next milestone

### 🚧 v0.10.0 — MaterialX shader-generation boundary

Use the official MaterialX Slang Shader Generator to establish a durable,
optional material-function boundary. v0.10.0 is not a production MaterialX
support claim: it proves that a small diagnosed graph and Standard Surface
slice can cross the existing host-neutral `MaterialIR` boundary, execute in
Vulkan Forward, and compile for SPIR-V and Metal without giving MaterialX
ownership of the render pass.

The authoritative scope, ownership, ABI, cache, diagnostic, and fallback rules
are in the [MaterialXGenSlang material boundary](../design/materialxgenslang-boundary.md).

#### Release acceptance criteria

- A MaterialX document deterministically produces an
  `evaluateMaterial`-style Slang module for constant/color values, image,
  texcoord, normal, add, multiply, mix, and the minimum Standard Surface
  `base`, `base_color`, `metalness`, `specular_roughness`, and `normal` slice.
- Vulkan Forward invokes the generated material function while geometry,
  lighting, alpha/depth policy, resources, render passes, picking, and AOV
  writes remain renderer-owned.
- Constant dielectric, constant metal, textured dielectric, arithmetic/mix,
  normal, multiple-material, and unsupported-node fallback fixtures meet their
  declared exact or tolerance-based image contracts.
- The same generated module source produces SPIR-V and Metal-target artifacts;
  target reflection agrees semantically with the common versioned material
  ABI.
- Core public APIs expose no MaterialX SDK types, and the MaterialX integration
  exposes no Vulkan types, native handles, descriptor bindings, or pipeline
  layout.
- Canonical graph topology, runtime parameter values, and texture/resource
  assignments have separate identities. Identical graphs share modules and do
  not regenerate; parameter-only edits do not change a shader key.
- Module and artifact keys cover MaterialX/library/generator/compiler versions,
  include fingerprints, target/profile/options, ABI version, and enabled
  features at the appropriate layer.
- Unsupported nodes, inputs, conversions, dependencies, generation, compile,
  reflection, ABI, target, and cache failures produce structured diagnostics
  with an explicit simplification, basic-material, or error-material fallback.
- `MERLIN_ENABLE_MATERIALX=OFF` retains the existing Core and Vulkan build,
  tests, basic material path, and install contract.
- Complete Standard Surface, arbitrary MaterialX documents, production IBL,
  asynchronous compilation, prewarming, Visibility integration, and complete
  OpenChess Set fidelity are not claimed by this release.

#### Remaining implementation focus

- Extend the host-neutral material/module contract so `MaterialIR` can refer to
  generated-module identity, logical parameter/resource layouts, feature
  requirements, ABI version, and revision without adding MaterialX-specific
  names to Core.
- Add image/texcoord/normal generation coverage and adapt the minimum Standard
  Surface outputs into the common material result. Full tangent-space normal
  mapping remains later quality work.
- Replace or layer the current document-and-source SHA-256 identity. The
  prototype key is deterministic but still changes when a uniform value in the
  document changes, so it is not yet the required topology-only module key.
- Track standard-library/includes and generator options in the module key, then
  compose target/compiler/profile/layout/capability policy into a separate
  artifact key reusable by MaterialX-generated and handwritten Slang.
- Bridge `Merlin::MaterialX` diagnostics into `merlin-diagnostic/v1`, add the
  required failure categories and context, and record the selected fallback in
  capability and telemetry evidence.
- Compose the generated function with the existing Vulkan Forward shader and
  resource paths; add pipeline/module reuse and image/fallback validation.
- Retain SPIR-V and Metal-target outputs plus logical/target reflection as
  deterministic build and test evidence, including ABI mismatch and
  render-pass-contamination checks.

The already merged graph-only compiler, package target, deterministic
prototype generation, logical reflection, diagnostics, and direct SPIR-V/Metal
compile wrappers are recorded in the
[delivery history](../reports/delivery-history.md) and summarized as current
capability in the [support matrix](../reference/support-matrix.md). They are
foundation evidence, not completion of the gates above.
