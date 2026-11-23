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

- **Open Chess Set opaque materials and backend parity (v0.16.8).** Use the
  unmodified Open Chess Set stage as the Forward material acceptance scene.
  - Support authored MaterialX Standard Surface `metalness` and
    `specular_roughness` constants and direct linear UV0 image textures.
    Evaluate both through shared Forward specular lighting for the camera
    light and distant scene lights. Test the result with the diffuse dome
    enabled and disabled.
  - Run the [Open Chess Set gate](../guides/forward-validation.md#open-chess-set)
    on Vulkan and Metal. Verify authored base color, tangent-space normals,
    metalness and roughness on the board and pieces. Use controlled probes to
    verify that metalness changes the reflection response and roughness changes
    the highlight width. Exercise Tier 0 and native Hgi presentation.
  - Run regression checks for vertex color, alpha masks, double-sided shading,
    depth and picking, and the existing generated-material ABI and reflection
    contracts.

Track alpha blending and Standard Surface transmission as separate follow-up
work. Alpha blending needs a compositing order, depth and picking policy.
The pawn tops use tinted transmission, which needs its own lighting and
refraction contract.

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
