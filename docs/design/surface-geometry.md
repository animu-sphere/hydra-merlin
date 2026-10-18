# Surface geometry, normals and tangent frames

This document defines the Mesh surface shading contract: what geometric and
shading normals mean, how host normals and tangents become renderer inputs,
how they are transformed and oriented, and how normal maps perturb them. Forward
lighting, UsdPreviewSurface, MaterialX and future material sources consume one
surface model; Vulkan and Metal execute it without defining their own normal
semantics. The [support matrix](../reference/support-matrix.md#surface-geometry-and-normals)
records which parts are currently implemented and validated.

Gaussian orientation is a separate contract owned by the
[Gaussian rendering design](gaussian-rendering.md).

## Responsibilities

| Owner | Responsibility |
| --- | --- |
| Adapter (Hydra, native) | Read authored normals, tangents, UVs, `orientation`, `doubleSided` and interpolation; triangulate; expand primvars to renderer elements without inventing semantics |
| Core mesh attribute preparation | Normal and tangent fallbacks, normalization, degenerate rejection, orientation records |
| `Merlin::RenderBackend` | Object-to-world and normal matrices, transform handedness, packing of surface inputs |
| Shared Slang policy | Construct `Ng`, `Ns`, the tangent frame and facing; decode and apply normal maps; supply lighting inputs |
| Material sources (MaterialX, UsdPreviewSurface) | Material graph evaluation, texture sampling and normal-map node values |
| Backends | Binding, compilation, pipelines, rasterizer state and dispatch |

MaterialX owns material semantics; Merlin owns surface geometry semantics. A
material function may return a perturbed tangent-space or world-space normal,
but it does not build the geometric normal, tangent frame, transforms or facing
state. UsdPreviewSurface uses the same surface context; there is no separate
PreviewSurface normal path.

## Normals

### Geometric normal `Ng`

`Ng` is the oriented face normal of the rasterized triangle in world space. It
is derived from triangle positions after the topology orientation and transform
handedness rules below, never from authored normals. It defines front/back
classification, double-sided flipping, fallback shading and validation of
`Ns`, and is the future ray-offset normal.

### Shading normal `Ns`

`Ns` is the normal supplied to BRDF and lighting evaluation. It is constructed
in stages and each stage is a distinct value:

```text
positions + orientation ─► Ng
authored / generated normal ─► interpolate ─► transform ─► Ns_base
Ns_base + tangent frame + normal map ─► Ns ─► lighting
```

`Ns_base` is never overwritten by normal mapping, and `Ng` is never replaced by
`Ns`. Debug views can therefore show each stage independently.

### Source priority

1. Authored normals.
2. Renderer-generated normals: flat (triangle `Ng`) or smooth (area-weighted
   average over shared positions within one primitive).
3. `Ng` when neither is available or the authored value is degenerate.

Authored normals are renormalized. A zero-length or non-finite authored normal
falls back to `Ng` for that element and is counted in diagnostics; malformed
primvar sizes or indices reject the primitive, as for other primvars. The
generation mode is a host input to the shared preparation: Hydra keeps flat
generation, and native indexed meshes, which store one normal per shared
vertex, generate smooth normals and repair a degenerate vertex normal with the
area-weighted `Ng` of its triangles. Smoothing
groups, crease inference and angle thresholds are not inferred; a host that
needs hard edges authors `faceVarying` or `uniform` normals.

### Interpolation

| Interpolation | Element |
| --- | --- |
| `constant` | One value for the primitive |
| `uniform` | One value per authored face, shared by its triangles |
| `vertex`, `varying` | One value per point, interpolated across faces |
| `faceVarying` | One value per authored face corner; preserves hard edges and seams |

Indexed primvars resolve through their indices before expansion. Expansion,
triangulation and corner remapping happen once in shared preparation rather
than in each backend. Triangulation preserves the authored corner identity of
every element so `faceVarying` values follow their corners through orientation
changes.

## Transforms

Positions use the object-to-world matrix `M`. Normals use the inverse-transpose
of its upper 3×3:

```text
N_world = normalize(transpose(inverse(M3)) * N_object)
```

Core computes this matrix once per draw or instance (cofactor/determinant form,
which preserves sign under reflection). A singular transform uses the identity
normal matrix and its geometry is reported, not silently re-oriented. The
vertex stage transforms; the fragment stage renormalizes after interpolation.

World space is the canonical shading space. Lights, environment lookup, `Ng`,
`Ns` and the tangent frame are evaluated in world space; view space is used only
for projection.

## Orientation and facing

Front-facing is the product of three independent terms:

```text
front = topology orientation × transform handedness × projection/raster convention
```

- **Topology orientation:** USD `rightHanded` keeps authored winding;
  `leftHanded` reverses triangle winding during triangulation. Authored normals
  are not negated; they already describe the authored outward side.
- **Transform handedness:** `det(M3) < 0` mirrors the surface. The renderer
  reverses the effective winding for that draw or instance so a mirrored
  instance has the same front faces as its unmirrored source. The normal matrix
  already carries the reflection for `Ns`.
- **Projection/raster convention:** Host projection flips and backend NDC
  conventions are compensated by the camera's front-face winding. Backends map
  that one logical winding to their native rasterizer state.

Normal signs are never corrected by the determinant alone, and backends do not
compensate orientation in shaders.

### Culling and double-sided surfaces

A single-sided surface culls back faces. A double-sided surface (host
`doubleSided` on the Mesh, or the material's double-sided state) disables
culling, and back-facing fragments flip both normals:

```text
if (!front) { Ng = -Ng; Ns_base = -Ns_base; }
```

The flip happens before normal mapping and tangent-frame construction, so a
normal map perturbs the visible side consistently. When `dot(Ns, Ng) < 0` after
interpolation or mapping, the shading normal is bent back to the `Ng`
hemisphere rather than lighting a surface from behind.

## Tangent frame

The tangent frame is stored as a world-space tangent plus handedness:

```text
T.xyz = tangent, T.w = ±1
B     = cross(Ns_base, T.xyz) * T.w
```

`T` is Gram–Schmidt orthogonalized against `Ns_base` in the fragment stage.
Handedness includes transform handedness, so mirrored instances and mirrored
UVs reconstruct the same bitangent sense as authored. Tangents are surface
directions and transform with `M3`, which the vertex stage recovers from the
normal matrix as `cof(N) * sign(det N)`; `T.w` is multiplied by the sign of
`det(M3)`.

Tangent source priority:

1. Authored tangent primvar with handedness, using the normal interpolation
   rules. Hydra reads `primvars:tangents` as float4 (handedness in `w`) or
   float3 (right-handed). Values are renormalized with `w` = ±1; a zero-length,
   non-finite or normal-parallel value uses the generated tangent and is
   counted (`mesh.tangents.degenerate`); a malformed primvar rejects the prim
   (`mesh.tangents.malformed`).
2. Renderer-generated tangents from positions, normals and texture coordinate
   set 0, with handedness from UV winding (MikkTSpace-compatible conventions):
   per-triangle `dP/du` is projected onto the corner normal, angle-weighted
   and accumulated over corners that share a point, texture coordinate, normal
   and handedness.
3. Screen-space derivative frame in the fragment stage, from the derivatives
   of the object-space position and texture coordinate.
4. No usable frame: normal mapping is disabled for that fragment, and for a
   draw whose geometry has no texture coordinates it is disabled and reported.

Mirrored UV islands and UV seams are first-class cases: tangents are not
averaged across a handedness change, and seams split tangent elements just as
`faceVarying` normals split normal elements. An all-zero tangent marks an
element without a frame. A triangle uses its interpolated vertex frame only
when its three corners carry the same handedness (`|T.w|` stays 1); a corner
without a frame selects the derivative frame for the triangle. Native indexed
meshes store one tangent per vertex, so a vertex shared by triangles of
opposite UV handedness is left without a frame and counted; producers that
need vertex frames there split the vertex as they would on a texture seam.

On a double-sided back face `T` and `T.w` are negated together with
`Ns_base`, so `B` keeps its direction and a normal map yields the negated
front-side normal, as for a thin two-sided sheet.

## Normal mapping

Texture decode and frame application are separate steps:

```text
texel (linear, non-color) ─► n = 2 * texel.rgb - 1 ─► scale xy ─► normalize
n ─► Ns = normalize(n.x * T + n.y * B + n.z * Ns_base)
```

Normal textures are sampled without color-space conversion. The default
convention is +Y up (OpenGL/MaterialX); a material declares a green-channel
flip explicitly rather than per backend (`MaterialParameterBlock`
`normal_scale` and `normal_flip_green`). A material that returns a world-space
normal bypasses the tangent frame but is still subject to the facing and
hemisphere rules.

UsdPreviewSurface `normal` connected to a `UsdUVTexture` output becomes the
material's normal texture. The texture's `texel * scale + bias` must express
this decode: `z` as `2t - 1` and `xy` as `s * (2t - 1)` with `y` optionally
negated, which maps to `normal_scale = s` and the green flip. A texture
without scale/bias (identity) is decoded as `2t - 1` and reported
(`hydra.material.normal-map`); any other encoding, a non-`UsdUVTexture`
source or an unreadable image disables normal mapping for the material with
the same diagnostic. A draw whose geometry has no texture coordinates drops
the normal map; Hydra reports the count (`surface.normal-map.disabled`) and
`render::CountForwardNormalMapDisabledDraws` exposes it to other hosts.

## Shared surface context

Shaders build one logical context before material evaluation:

```text
SurfaceGeometry
  position_ws, Ng, Ns_base, tangent (xyz, sign), uv[], front_facing
```

The GPU layout is not fixed by this structure; ABI packing follows the
[renderer architecture](renderer-architecture.md) and shader ABI contracts. The
requirement is that every material source and backend reads the same concepts
from one shared Slang module rather than re-deriving them.

The current Forward implementation (`forward-policy.slang`) derives `Ng` from
`cross(ddx(p), ddy(p))` of the interpolated object-space position, mapped by
the normal matrix. That cross product is the oriented triangle normal divided
by its signed framebuffer area, and a mirrored draw reverses both its winding
and that sign, so a per-frame sign (camera front-face winding × the backend's
NDC Y direction) and the sign of `det(M3)` orient `Ng` toward the visible side
without a world matrix in the draw constants. `MaterialConstants.surface_policy`
carries that sign and the debug view. Positions with no usable derivative fall
back to `Ns_base`. The same function builds the tangent frame;
`MaterialConstants.normal_map` carries the normal scale and green sign, and
paths that bind textures by table index read the normal texture slots from
`MaterialConstants.normal_map_resource` or `GpuMaterial`.

## Debug views

Diagnostic views render, as unlit RGB (`0.5 * v + 0.5` for vectors):

- `Ng`, `Ns_base` and final `Ns`;
- tangent and bitangent (mid-grey without a frame);
- front-facing (white front, black back).

They use the same surface context as lit shading, so a view isolates the stage
that differs between backends or hosts.

## Validation contract

Normal behavior is validated with small deterministic fixtures before
representative scenes:

| Area | Fixtures |
| --- | --- |
| Geometry | Hard-edge cube, smooth and low-poly spheres, plane, mirrored mesh |
| Transform | Uniform, non-uniform and negative scale; rotation with non-uniform scale; nested transforms |
| Interpolation | `constant`, `uniform`, `vertex`, `varying`, `faceVarying`, indexed and unindexed |
| Orientation | `rightHanded`/`leftHanded`, single/double-sided, back-face views |
| Tangent frame | Authored and generated tangents, mirrored UVs, UV seams |
| Material | Diffuse, directional light, normal map, exaggerated normal map, double-sided normal map |

Analytic fixtures compare normals and lit centers against a CPU oracle; debug
views compare exactly between repeated frames. Vulkan/Metal and native/Hydra
comparisons must agree on orientation, highlight direction, seams, normal-map
sign and back-face behavior. Representative scenes are not pixel-exact gates.

## Out of scope

Displacement, parallax and advanced bump mapping; anisotropic tangent
reconstruction beyond the frame above; subdivision-specific tangents;
micropolygon or path-tracer shading-normal corrections; Gaussian surface
orientation; and geometry or meshlet normal compression.
