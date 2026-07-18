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

## Active follow-up

### ⬜ v0.10.x — Development viewport and diagnostic surface

Turn `merlin-viewport` into the primary renderer-development and profiling
tool without turning it into a USD authoring application. Add a thin
immediate-mode UI integration, preferably Dear ImGui, behind a host abstraction;
the UI remains outside Core and renderer backends and consumes host-neutral
settings, diagnostics, capability, and telemetry contracts.

The initial surface covers:

- backend/device capabilities and the selected path or rejected fallback;
- CPU/GPU frame timings and AOV selection;
- geometry, texture, sampler, descriptor, and VRAM residency;
- material/module compilation state and structured diagnostics;
- primitive/instance counts, uploads, allocations, camera, and viewport state;
- screenshot and benchmark controls.

Exit requires the viewport to explain path selection and rejection, make
material/resource fallback visible without log inspection, and support
interactive performance-regression inspection. The Vulkan render loop remains
independent of UI frame rate and widget implementation. A complete stage tree,
property editor, and authoring workflow remain outside this milestone.

## Phase A foundation gates

These cross-cutting items should land alongside v0.10.0 or v0.10.x, before the
implementation becomes substantially more backend-specific.

### ⬜ Evidence-tier separation

- **Tier 1 — required hosted checks:** Core Debug/Release on Windows and Linux,
  shader compilation, SPIR-V validation/reflection, Metal-target compilation,
  MaterialX generation/ABI tests, install-tree consumers, and Hydra adapter
  compilation where a reproducible SDK is available.
- **Tier 2 — required or scheduled capability checks:** Vulkan runtime/image
  tests, Hydra discovery and first frame, native Vulkan viewport smoke tests,
  and stable-update/changed-range tests.
- **Tier 3 — hardware-profile evidence:** timing thresholds, bindless
  selection, transfer-queue behavior, VRAM pressure/exhaustion, and
  vendor-specific capability reports.

GPU timing does not become a universal pull-request gate until runner variance
is controlled; missing hardware evidence remains distinguishable from a product
failure.

### ⬜ Producer-session renderer evidence

`ost 0.18.0` requires a renderer report to record a producer session that ran
to completion and succeeded before it will accept a PASS. The generated
`openstrata.renderer-report/v1alpha1` document emits bare `{id, status}` checks
with no such field, so `ost validate` reports `renderer-evidence: fail` even
after a managed build that OpenStrata performed itself.

The report generator must bind each assertion to the outcome of the session
that produced it, so a failed or partial run cannot leave a stale PASS behind.
This depends on the upstream schema being published; the gap and the
corresponding upstream ask are recorded in
[OST report 7](../reports/ost/07-2026-07-18-v0.18.0-recheck-v0.19.0-asks.md).

### ⬜ Linux Vulkan validation

Add Linux Vulkan configuration and shader builds, useful headless execution
through Mesa lavapipe, optional real-GPU capability execution, and GLFW viewport
smoke coverage for supported window systems.

### ⬜ Versioned renderer settings

Define a host-neutral settings schema before DCC integration expands. The first
version covers backend, presentation mode, Forward/experimental path, AOV,
lighting mode, exposure/tone mapping, alpha policy, debug views, validation,
and telemetry controls.

## Near-term execution order

1. Close v0.10.0 narrowly around the ABI, identity split, diagnostics/fallback,
   Vulkan Forward execution, deterministic cross-target artifacts, and image
   evidence; do not broaden node coverage merely to claim general MaterialX.
2. Add the development viewport surface over existing capability, timing,
   residency, upload, material, AOV, and fallback contracts.
3. Strengthen non-GPU and Linux gates before implementation breadth grows.
4. Begin the native Metal backend and presentation using the v0.9 renderer and
   viewport boundaries plus the v0.10 material ABI.
