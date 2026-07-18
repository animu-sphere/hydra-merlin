# Current

Only incomplete work for the next release milestone and active carry-over is
listed here. Completed pre-release detail is retained in the
[delivery history](../reports/delivery-history.md); shipped versions will be
recorded in the [changelog](../../CHANGELOG.md).

## Next milestone

### 🚧 v0.10.0 — MaterialXGenSlang prototype

Use the official MaterialX Slang Shader Generator for a deliberate prototype
covering constants, colors, images, texcoords, normals, multiply/add/mix, and a
small Standard Surface subset. Generate a cached Slang material-evaluation
module rather than a complete render pass, connect it to Vulkan Forward through
the host-neutral `MaterialIR` boundary, diagnose unsupported nodes, and keep
geometry, lights, alpha policy, resources, and AOV writes renderer-owned.

#### Exit criteria

- A MaterialX document produces a deterministic `evaluateMaterial`-style
  module and cache key.
- Vulkan Forward calls the generated function and matches reference images
  within the declared tolerance.
- SPIR-V and Metal-target artifacts plus reflection metadata come from the same
  generated module.
- Raw MaterialX graphs do not enter Core; this milestone does not claim
  production-wide node coverage.

#### In-progress foundation

- `Merlin::MaterialX` is an optional compiler layer outside Core and exposes no
  MaterialX SDK types through its public API.
- Constants and the deliberate add/multiply/mix graph slice generate a
  deterministic `evaluateMaterial(MaterialInputs)` Slang function, SHA-256
  cache key, logical input/uniform metadata, and structured unsupported-node
  diagnostics.
- The generated function is compiled through SPIR-V and Metal test wrappers
  from the same source, with Slang reflection JSON retained as test evidence.
- Image/texcoord/normal coverage, the Standard Surface parameter adapter,
  Vulkan Forward integration, and image-tolerance evidence remain incomplete.
