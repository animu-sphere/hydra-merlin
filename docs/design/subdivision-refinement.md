# CPU subdivision refinement

[Mesh surface preparation](surface-geometry.md) consumes triangles. The
optional OpenSubdiv CPU path converts a Catmull–Clark control cage to those
triangles before RenderWorld submission. Hydra and native callers use
`merlin::MeshSubdivision`. The resulting `MeshDescriptor` uses the existing
mesh upload and drawing paths on Vulkan and Metal. The build compiles the
vendored Far, Sdc and Vtr sources into RenderWorld.

## Input and selection

The cage carries point positions, face counts and point indices, holes,
orientation, boundary and face-varying interpolation rules, and crease and
corner sharpness. Creases accept one weight per chain or per edge. Texture
coordinates, display color and opacity accept constant, uniform, vertex,
varying and face-varying data. Indexed face-varying data retains its authored
indices through refinement, including when UV values happen to coincide on
opposite sides of a seam.

Hydra uses the resolved mesh scheme, including the USD schema fallback
`catmullClark`. Refined surface and wire representations use Hydra's display
style level. In usdview, Low uses the cage and Medium through Very High request
levels 1 through 3, as in Storm. The hull and point representations use the
cage. Native callers pass the scheme and level explicitly. Hydra builds enable
OpenSubdiv by default.
[OpenUSD complexity mapping](https://github.com/PixarAnimationStudios/OpenUSD/blob/v26.08/pxr/usdImaging/usdImagingGL/engine.cpp)

## Preparation

OpenSubdiv Far refines the topology uniformly and provides a position stencil
table. Each primitive retains one plan keyed by cage topology, scheme, tags,
point count, level and face-varying topology. A points-only edit can reuse
the plan and position stencils. The adapter schedules evaluation only when a
geometry input changes. The output uses normal mesh revisions and uploads.

Refined faces are triangulated with the authored orientation. Shared surface
preparation computes smooth corner normals from the refined triangles, with
normal groups split at remaining sharp edges and fully sharp corners. It
generates tangent frames using the refined UVs and normals. This yields
normals for the uniform triangle mesh. Evaluating derivatives on limit
patches is a separate operation. Subdivision surfaces derive normals and
tangents from the refined geometry and UVs.

## Limits and recovery

Level zero and scheme `none` select the polygon path. The initial refinement
limits are level 3 and an estimated 128 MiB of CPU working memory per mesh.
The estimate is checked before building the refinement plan. For a Hydra
display level that exceeds the estimate, the renderer reports the highest
lower level that fits and uses it. When even level 1 exceeds the budget, it
uses the cage. Native `MeshSubdivision::Refine` requests retain the exact-level
policy. Unsupported schemes or settings, unavailable OpenSubdiv, malformed
subdivision data and unsupported levels emit a diagnostic and select the cage.
Malformed polygon topology rejects the primitive. Current evidence is tracked in the
[support matrix](../reference/support-matrix.md#subdivision).
