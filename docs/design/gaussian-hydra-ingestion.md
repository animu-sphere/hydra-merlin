# Gaussian ingestion through Hydra

**Status:** accepted integration boundary  
**Validated baseline:** OpenUSD 26.05  
**Implementation milestone:** v0.14.0 Gaussian MVP

## Decision

hdMerlin will consume the standard OpenUSD
`UsdVolParticleField3DGaussianSplat` representation through Hydra's
`particleField` Rprim. It will not define a Merlin USD schema, a Merlin USD prim
type, renderer-specific USD attributes, or a PLY/SPLAT parser.

OpenUSD 26.05 already supplies both sides of the stage-to-Hydra boundary:

- `ParticleField3DGaussianSplat` is a concrete `UsdVolParticleField` with the
  position, orientation, scale, opacity, Gaussian-ellipsoid kernel, and
  spherical-harmonic APIs applied by the schema.
- `usdVolImaging` registers `UsdImagingParticleFieldAdapter` for
  `ParticleField` and derived prim types. It supplies legacy imaging and
  scene-index data-source access.
- Hydra publishes `HdPrimTypeTokens->particleField`. The render delegate can
  advertise this Rprim without inspecting the USD stage or linking `usdVol`
  into renderer-neutral Core.

```text
UsdVolParticleField3DGaussianSplat
    ↓ OpenUSD usdVolImaging adapter
Hydra particleField Rprim / scene-index data sources
    ↓ hdMerlin adapter validation and normalization
host-neutral GaussianResource (introduced in v0.14.0)
    ↓ revisions and changed ranges
Gaussian GPU resources and render pipeline
```

OpenUSD's `HdsiParticleFieldConversionSceneIndex` points conversion is a host
preview fallback, not the native hdMerlin path. It cannot preserve the full
ellipsoid, opacity, projection/sorting, and spherical-harmonic rendering
contract. Selecting it must produce a versioned lossy-fallback diagnostic.

## Standard attribute contract

Float precision is preferred when both float and half attributes are authored,
as required by the OpenUSD schemas.

| Meaning | Standard OpenUSD source | Adapter result | Revision domain |
| --- | --- | --- | --- |
| Count and local position | `positions` (`point3f[]`), else `positionsh` | float local positions | positions |
| Kernel orientation | `orientations` (`quatf[]`), else `orientationsh` | normalized quaternion; identity fallback | orientation/covariance |
| Linear kernel scale | `scales` (`float3[]`), else `scalesh` | float linear scale; unit fallback | scale/covariance |
| Linear opacity | `opacities` (`float[]`), else `opacitiesh` | float `[0,1]`; opaque fallback | opacity |
| Radiance degree | `radiance:sphericalHarmonicsDegree` | one non-negative field degree | radiance layout |
| Radiance coefficients | `radiance:sphericalHarmonicsCoefficients` (`float3[]`), else the half variant | particle-major float coefficients | radiance payload |
| Kernel | auto-applied `ParticleFieldKernelGaussianEllipsoidAPI` | Gaussian ellipsoid with the standard 3-sigma convention | kernel |
| Projection hint | `projectionModeHint` | `perspective` or `tangential` | projection policy |
| Sorting hint | `sortingModeHint` | `zDepth`, `cameraDistance`, or supported fallback | sorting policy |
| Shared scene state | inherited xform, visibility, extent, purpose | existing instance/visibility/bounds state | transform, visibility, bounds |

Positions define the particle count. For every other per-particle attribute,
the adapter follows the standard length policy: truncate a longer array; ignore
a shorter array and use the schema fallback. Spherical-harmonic coefficient
length is `particle_count * (degree + 1)^2`. Non-finite positions, scales, or
coefficients reject the prim; non-unit orientations are normalized; opacity
outside `[0,1]` is clamped. Every rejection or lossy fallback is reported as
`merlin-diagnostic/v1` with the USD path, stable code, disposition, and named
recovery.

## Synchronization boundary

The `particleField` implementation belongs in `adapters/merlin-hydra2`.
Renderer-neutral Core receives only validated arrays, stable handles,
per-aspect revisions, and changed ranges.

| Hydra change | Work allowed |
| --- | --- |
| Transform or visibility | Update only shared instance state; fetch no Gaussian arrays |
| Positions | Fetch positions; update bounds and ranges; invalidate affected covariance |
| Orientations or scales | Fetch only that semantic; invalidate affected covariance ranges |
| Opacity | Fetch and upload only opacity ranges |
| SH degree | Rebuild radiance layout and payload |
| SH coefficients | Fetch and upload coefficient ranges when layout is unchanged |
| Projection or sorting hint | Update policy state; create no geometry payload |
| Removal and re-addition | Retire the old generation; create a new cache/handle generation |

Terminal scene-index locators are the preferred invalidation signal. If an
OpenUSD notice is coarser than one attribute, the adapter may fetch the enclosing
data source but must compare it with cached values before advancing a semantic
revision or uploading. This is the v0.6.0 Mesh primvar contract as well.

## Compatibility and fallback policy

- v0.14.0 adds `HdPrimTypeTokens->particleField` to the supported Rprim list and
  creates an adapter-owned Rprim. No `usdVol` type crosses into Core.
- A host missing `usdVolImaging` or the `particleField` Rprim receives a
  rejection diagnostic naming the plugin or incompatible SDK.
- A points approximation is opt-in and diagnostic-bearing; it is not evidence
  that native Gaussian rendering works.
- External formats are converted before the render delegate by a FileFormat
  plugin or importer. hdMerlin never applies log-scale or sigmoid transforms
  based on a source-file convention.
- Hydra configuration accepts the validated OpenUSD 26.05 shared SDK. On MSVC,
  Debug hdMerlin is rejected when the SDK only exports Release libraries.

## Evidence

- OpenUSD 26.05
  [ParticleField3DGaussianSplat guide](https://openusd.org/release/user_guides/schemas/usdVol/ParticleField3DGaussianSplat.html)
  and
  [schema source](https://github.com/PixarAnimationStudios/OpenUSD/blob/v26.05/pxr/usd/usdVol/schema.usda)
- Standard attribute rules:
  [position](https://openusd.org/release/user_guides/schemas/usdVol/ParticleFieldPositionAttributeAPI.html),
  [orientation](https://openusd.org/release/user_guides/schemas/usdVol/ParticleFieldOrientationAttributeAPI.html),
  [scale](https://openusd.org/release/user_guides/schemas/usdVol/ParticleFieldScaleAttributeAPI.html),
  [opacity](https://openusd.org/release/user_guides/schemas/usdVol/ParticleFieldOpacityAttributeAPI.html), and
  [spherical harmonics](https://openusd.org/release/user_guides/schemas/usdVol/ParticleFieldSphericalHarmonicsAttributeAPI.html)
- Standard USD-to-Hydra adapter:
  [particleFieldAdapter.h](https://github.com/PixarAnimationStudios/OpenUSD/blob/v26.05/pxr/usdImaging/usdVolImaging/particleFieldAdapter.h)
  and
  [usdVolImaging registration](https://github.com/PixarAnimationStudios/OpenUSD/blob/v26.05/pxr/usdImaging/usdVolImaging/plugInfo.json)
- Hydra
  [Rprim token](https://github.com/PixarAnimationStudios/OpenUSD/blob/v26.05/pxr/imaging/hd/tokens.h)
  and
  [optional points conversion](https://github.com/PixarAnimationStudios/OpenUSD/blob/v26.05/pxr/imaging/hdsi/particleFieldConversionSceneIndex.h)

The locally validated SDK contains `usd_usdVolImaging.dll`, registers the
adapter with `includeDerivedPrimTypes`, publishes the `particleField` token, and
includes the conversion scene index. This establishes the integration boundary;
native resources and rendering remain v0.14.0 scope.
