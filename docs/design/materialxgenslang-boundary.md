# MaterialXGenSlang material boundary

**Status:** Accepted for v0.10.0; implementation in progress

**Date:** 2026-07-18

## Decision

v0.10.0 proves a durable shader-generation boundary, not production-wide
MaterialX support. The release is complete when an intentionally small
MaterialX subset can generate a deterministic Slang material-evaluation module,
flow through the existing host-neutral material model, and execute in Vulkan
Forward while the same source passes SPIR-V and Metal-target compile and
reflection gates.

The governing rule is: **keep the feature slice small and make the boundary
production-quality**.

MaterialX documents, node graphs, and SDK types never become the persistent
Core scene representation. MaterialXGenSlang never generates or owns a Merlin
render pass.

## Repository mapping

The policy maps onto the current repository as follows:

| Layer | Repository location | Policy ownership |
| --- | --- | --- |
| Host-neutral scene and material semantics | `core/merlin-render-world` | `MaterialIR`, typed parameter state, logical texture/sampler bindings, exact input-space requirements, alpha policy, generated-module identity, and revisions |
| Immutable draw-facing extraction | `core/merlin-render-extraction` | Backend-neutral snapshot records and logical resource identity |
| MaterialX integration | `material/merlin-materialx` / `Merlin::MaterialX` | Document and library handling, validation, canonicalization, MaterialXGenSlang invocation, logical reflection, module keys, and source diagnostics |
| Slang target compilation | Existing Slang build/test pipeline, reusable without MaterialX-specific assumptions | Compiler invocation, target/profile options, target reflection, artifact keys, and compiler diagnostics |
| Vulkan execution | `backend/merlin-vulkan` | SPIR-V modules, descriptors, residency, pipelines, synchronization, Forward pass integration, and fallback rendering |
| Host translation | `adapters/*` | Source identity, MaterialX or Hydra input selection, diagnostic routing, and authored fallback policy |

`Merlin::MaterialX` is optional. A build with `MERLIN_ENABLE_MATERIALX=OFF`
must continue to build and exercise Core and the existing Vulkan material path.
The integration target privately depends on MaterialX; its public API exposes no
MaterialX SDK type. Core does not depend on MaterialX, Slang compiler APIs,
OpenUSD, Hydra, Vulkan, or Metal.

The existing repository does not need a speculative general runtime shader
service for v0.10.0. Target compilation may reuse or extract the current
`slangc` build/test machinery, but its input, reflection, diagnostic, and cache
contracts must also work for handwritten Slang modules and must not be named or
shaped exclusively around MaterialX.

## Data flow and ownership

```text
MaterialX document
    -> material/merlin-materialx
    -> canonical material description
       |-> MaterialIR instance semantics
       `-> generated material module + logical reflection
    -> backend target artifact
    -> renderer-owned pipeline and pass
```

Hydra networks, the existing basic `UsdPreviewSurface` adapter, handwritten
materials, and future material sources normalize at the same `MaterialIR`
boundary. A raw MaterialX graph is input provenance, not a `RenderWorld`
resource.

MaterialXGenSlang owns:

- supported graph evaluation and node implementations;
- texture-sampling expressions and material-specific parameter interpretation;
- generation of a material-evaluation Slang module; and
- source-level logical input and resource reflection.

hdMerlin owns:

- geometry transforms and construction of shading inputs;
- normals, tangent frames, UVs, and other primvars;
- lights and environment lighting;
- alpha, depth, culling, and capability policy;
- AOV and picking output;
- render-pass composition; and
- backend resources, bindings, pipelines, synchronization, and lifetime.

This division lets Forward, a later Visibility resolve, and future passes reuse
the same generated material semantics without asking MaterialX to generate each
pass.

## Material function ABI

The semantic contract is equivalent to:

```slang
MaterialResult evaluateMaterial(
    MaterialInputs inputs,
    MaterialParameters parameters,
    MaterialResources resources);
```

MaterialXGenSlang may emit reflected global parameter or resource blocks, and a
renderer wrapper may adapt those blocks to this contract. The stable part is
the meaning, not whether every argument appears literally in the generated
function signature.

v0.10.0 fixes the following logical ABI:

- the material-evaluation entry contract;
- required geometry inputs and their semantics;
- the minimum material result (`base_color`, `metalness`,
  `specular_roughness`, and a shading normal);
- parameter and logical texture/sampler layout metadata;
- module and target-artifact identity;
- material feature requirements;
- ABI and reflection schema versions; and
- structured diagnostic classes.

Core types use renderer-neutral names such as `MaterialModule`,
`MaterialModuleKey`, `MaterialParameterLayout`, `MaterialResourceLayout`,
`MaterialFeatureRequirements`, and `MaterialDiagnostic`. These are logical
contracts, not a requirement to introduce every example as a separate C++ type.

The following remain backend-owned or deliberately extensible:

- Vulkan descriptor sets and bindings;
- Metal argument-buffer indices and offsets;
- pipeline layouts and native shader handles;
- Forward lighting data or Visibility-specific records;
- the complete Standard Surface field set; and
- MaterialX document paths and node-specific C++ types.

The current compiler returns raw supported graph outputs or a renderer-owned
minimum `MaterialResult` from `evaluateMaterial(MaterialInputs)`. The Standard
Surface adapter evaluates only the accepted upstream graph slice and does not
invoke MaterialX lighting closures. Typed parameter/resource state and the Core
module reference cross the host-neutral boundary separately; target artifacts
and Vulkan execution remain v0.10.0 work.

## v0.10.0 feature slice

Required node and data coverage is intentionally narrow:

- constant and color values;
- image, texcoord, and normal inputs;
- multiply, add, and mix; and
- Standard Surface `base`, `base_color`, `metalness`,
  `specular_roughness`, and `normal`.

`convert` may be supported as an implementation helper without expanding the
public coverage claim. Normal input may initially use an already constructed
shading normal. Complete tangent-space normal mapping is deferred.

The following are not v0.10.0 release requirements:

- complete Standard Surface, arbitrary documents, or broad procedural nodes;
- coat, transmission, subsurface, sheen, anisotropy, displacement, or volume;
- multiple-UV-set completeness, advanced color management, or production IBL;
- tangent-space normal maps, opacity/emissive quality, or UV transforms;
- asynchronous compilation, prewarming, or pipeline libraries;
- Visibility Buffer integration;
- DCC-specific search-path compatibility; or
- faithful rendering of the complete OpenChess Set.

Unsupported features must be diagnosed; they must not silently produce a
plausible but incorrect result.

## Module, artifact, and instance identity

v0.10.0 keeps three identities separate:

```text
canonical graph topology -> material module key
parameter values          -> material instance state
texture assignments       -> resource binding state
```

The target-neutral material module key includes:

- canonical graph topology and compile-time specializations;
- MaterialX and standard-library version or fingerprint;
- MaterialXGenSlang version and pinned compatibility revision;
- generator options;
- material ABI and enabled feature set; and
- transitive include/library dependency fingerprints.

A target-artifact key adds:

- the material module/source identity;
- Slang compiler version;
- backend target, profile, and capability set;
- matrix/scalar layout and optimization/debug policy; and
- any target-specific generation options.

Runtime parameter values do not enter either shader key unless a documented
specialization changes topology, generated source, or the ABI. Texture content
or assignment does not enter the shader key unless it changes the declared
resource interface.

The compiler hashes generated source, its logical interface, ABI/reflection
versions, generator version/revision, fixed generator options, loaded standard-
library documents, and transitive generator-source includes into a target-
neutral module key. Reflected uniform defaults and resource defaults produce
separate instance and resource-state keys, so parameter-only and texture-
assignment edits keep the module key and source unchanged. The separate target-
artifact key remains required before the v0.10.0 identity gate is complete.

Core stores typed generated-parameter values and resolved logical resource
bindings separately from the module definition. MaterialX filename defaults
remain integration-side identifiers until a host adapter resolves them to
RenderWorld texture/sampler handles. Extraction carries both states and their
independent revisions without consulting the source graph.

## Diagnostics and fallback

The MaterialX integration reports structured categories for at least:

- unsupported node, Standard Surface input, or type conversion;
- missing texture, include, or MaterialX library;
- shader-generation or Slang compile failure;
- reflection or ABI mismatch;
- backend target failure; and
- corrupt or incompatible cache data.

Where available, records include material identity, element path, node
category, input name, source document, backend target, generator/compiler
version, and the selected recovery. Integration-local diagnostics bridge into
the existing host-neutral `merlin-diagnostic/v1` sink before reaching Hydra or
another host.

Fallback keeps rendering alive in this order when the declared policy permits:

1. evaluate a diagnosed supported simplification;
2. use the existing constant-base-color basic material; or
3. use the explicit error material.

The chosen fallback is visible in diagnostics, capability evidence, and
telemetry. A fallback is never reported as successful MaterialX coverage.

## Backend integration

Vulkan Forward retains its existing geometry, lighting, attachment, residency,
and synchronization structure. The integration point is limited to composing a
generated material function into the renderer-owned fragment module:

```text
renderer geometry/lighting/pass module
                +
generated material-evaluation module
                =
Vulkan Forward fragment pipeline
```

`backend/merlin-vulkan` owns every Vulkan handle and concrete layout. Neither
Core nor `material/merlin-materialx` stores a Vulkan object or binding number.

Metal execution is not required in v0.10.0. The compile gate does require the
same canonical module and ABI version to produce SPIR-V and Metal-target output,
with semantically matching parameter/resource reflection and explicit
diagnostics for unsupported target features. Each backend derives its own
native layout from the common logical reflection.

## Validation

Unit and generation tests cover:

- deterministic graph canonicalization and keys;
- topology versus instance-parameter separation;
- include dependency tracking;
- supported and unsupported node diagnostics;
- logical reflection serialization and ABI mismatch detection;
- identical generated Slang for identical canonical input;
- SPIR-V and Metal-target compilation from the same module; and
- absence of renderer entry points, lighting, AOV, or pass-owned declarations
  in generated material source.

Small image fixtures cover constant dielectric, constant metal, textured
dielectric, add/multiply, mix, normal input, multiple material assignments, and
unsupported-node fallback. OpenChess Set may provide representative integration
smoke materials; complete visual fidelity is not a release gate.

## Release acceptance

v0.10.0 is complete only when all of the following hold:

1. A MaterialX document deterministically produces a material-evaluation Slang
   module.
2. Vulkan Forward invokes the generated function.
3. The required small node/Standard Surface slice passes image tests.
4. The same module source produces SPIR-V and Metal-target artifacts.
5. Logical and target reflection agree with the versioned material ABI.
6. No MaterialX SDK type appears in a Core public API.
7. No Vulkan type or handle appears in the MaterialX integration boundary.
8. Graph topology, instance parameters, and resource binding state are
   separately identified.
9. Unsupported inputs produce a structured diagnostic and explicit fallback.
10. Core and the existing Vulkan material path build and run with MaterialX
    disabled.
11. Reuse of an identical graph performs no unnecessary regeneration.
12. Complete OpenChess Set reproduction remains outside the release claim.

## Later releases

v0.18.0 builds quality and operational behavior on this accepted boundary:
broader Standard Surface, UV transforms, tangent-space normal maps, opacity and
emissive behavior, environment lighting, asynchronous compilation, prewarming,
texture/sampler residency, runtime parameter-only updates, shader sharing, mip
and gradient quality, and Forward/Visibility parity.

Metal consumes the common module/reflection contract but owns Metal residency
and pipeline layout. Persistent draw records carry material and module IDs, not
MaterialX graphs. Visibility resolve reuses the material-evaluation contract.
Gaussian appearance may share resource infrastructure but is not forced into a
MaterialX mesh BSDF.
