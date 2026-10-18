# Current

Active and immediately next work only. Shipped changes are in the
[changelog](../../CHANGELOG.md); current support is in the
[support matrix](../reference/support-matrix.md).

## v0.16.x — Forward renderer stabilization

v0.16.x establishes a stable architectural, semantic and quality baseline
before v0.17.0 begins the next Gaussian optimization cycle. The
[v0.16.2 release](../releases/v0.16.2.md) is the Forward semantic baseline:
later v0.16.x refactors preserve its rendered meaning unless a change is
explicitly part of an output-transform or quality milestone. Core owns
rendering semantics; backends own execution.

### Active

- **Normal contract release (v0.16.5):** prepare the release record for the
  validated normal part of the
  [surface geometry contract](../design/surface-geometry.md).

### Next

- **Tangent frame and normal mapping (v0.16.6):** authored tangents,
  generated tangents with handedness, derivative-frame fallback and reported
  disablement; mirrored UVs and seams; linear normal-map decode and TBN
  application shared by UsdPreviewSurface and MaterialX; tangent/bitangent
  debug views; Vulkan/Metal agreement on normal-map sign and seams.
- **Representative Forward quality (v0.16.7):** interior Kitchen views,
  bright/dark lighting, textured, rough and metallic materials, Camera Light
  ON/OFF, linear/display output, sustained camera navigation, Vulkan/Metal and
  native/Hydra comparisons. Keep analytic, small-fixture, representative-scene
  and interactive layers distinct; representative scenes are not pixel-exact
  gates. No supported mode needs the camera light disabled to avoid white
  clipping or flicker, navigation exposes no stale-frame or resource-update
  errors, and backend differences are understood and recorded in the support
  matrix.
- **Material and backend parity hardening (v0.16.8):** base color, roughness,
  metallic, textures, vertex color, opacity/alpha mask, double-sided state,
  generated material ingestion, the generated Metal material path and
  material ABI reflection behave consistently across supported paths, on top
  of the shared surface context. Unsupported MaterialX capabilities remain
  explicitly unsupported.

Patch numbers are labels for the phases above and may change; the phase
responsibilities do not.

### Exit

v0.16.x closes when this statement is substantially true:

> Forward rendering has stable cross-backend semantics, an explicit
> output-transform policy, a stable Mesh surface shading contract for normals,
> tangent frames and normal mapping, maintainable backend/shader architecture,
> representative real-scene quality evidence, and no known unexplained
> Vulkan/Metal contract divergence in the supported material subset.

Every desirable shading feature is not required before v0.17.0.

### Out of scope for v0.16.x

Unless required to fix a correctness regression, these belong to the
[backlog](backlog.md): contribution-aware Gaussian culling, hierarchical
Gaussian tiles, temporal Gaussian reuse, Gaussian LOD/compression, streaming
and out-of-core execution, production meshlets, Hi-Z, Mesh Shader, Visibility
Buffer productionization, broad new MaterialX graph coverage, a complete shadow
system, displacement/parallax and other advanced surface detail, and major new
renderer architecture experiments.
