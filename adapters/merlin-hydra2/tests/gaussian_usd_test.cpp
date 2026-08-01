#include <merlin/core/gaussian.hpp>

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdVol/particleField3DGaussianSplat.h>

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

int main(int argc, char** argv) {
  assert(argc == 2);
  const std::filesystem::path path(argv[1]);
  assert(std::filesystem::exists(path));
  const auto stage = UsdStage::Open(path.string());
  assert(stage);
  assert(stage->GetDefaultPrim());
  assert(stage->GetDefaultPrim().GetPath() == SdfPath("/Asset"));
  assert(UsdGeomGetStageUpAxis(stage) == UsdGeomTokens->y);
  assert(UsdGeomGetStageMetersPerUnit(stage) == 1.0);

  UsdVolParticleField3DGaussianSplat splat;
  for (const auto& prim : stage->Traverse()) {
    if (prim.IsA<UsdVolParticleField3DGaussianSplat>()) {
      assert(!splat);
      splat = UsdVolParticleField3DGaussianSplat(prim);
    }
  }
  assert(splat);
  assert(splat.GetPath() == SdfPath("/Asset/Splat"));

  VtVec3fArray positions;
  VtQuatfArray orientations;
  VtVec3fArray scales;
  VtFloatArray opacities;
  VtVec3fArray coefficients;
  int degree{};
  assert(splat.GetPositionsAttr().Get(&positions));
  assert(splat.GetOrientationsAttr().Get(&orientations));
  assert(splat.GetScalesAttr().Get(&scales));
  assert(splat.GetOpacitiesAttr().Get(&opacities));
  assert(splat.GetRadianceSphericalHarmonicsDegreeAttr().Get(&degree));
  assert(splat.GetRadianceSphericalHarmonicsCoefficientsAttr().Get(
      &coefficients));
  assert(degree >= 0);

  merlin::GaussianSourceData source;
  source.label = splat.GetPath().GetString();
  source.source = source.label;
  source.spherical_harmonics_degree = static_cast<std::uint32_t>(degree);
  source.positions.reserve(positions.size());
  for (const auto& value : positions) {
    source.positions.push_back({value[0], value[1], value[2]});
  }
  source.orientations.reserve(orientations.size());
  for (const auto& value : orientations) {
    const auto imaginary = value.GetImaginary();
    source.orientations.push_back(
        {value.GetReal(), {imaginary[0], imaginary[1], imaginary[2]}});
  }
  source.scales.reserve(scales.size());
  for (const auto& value : scales) {
    source.scales.push_back({value[0], value[1], value[2]});
  }
  source.opacities.assign(opacities.begin(), opacities.end());
  source.spherical_harmonics_coefficients.reserve(coefficients.size());
  for (const auto& value : coefficients) {
    source.spherical_harmonics_coefficients.push_back(
        {value[0], value[1], value[2]});
  }

  const auto normalized = merlin::NormalizeGaussianSource(source);
  assert(normalized.accepted());
  assert(normalized.diagnostics.empty());
  assert(normalized.resource->positions.size() == positions.size());
  assert(normalized.resource->covariances.size() == positions.size());
  assert(normalized.resource->opacities.size() == positions.size());
  assert(normalized.resource->spherical_harmonics_coefficients.size() ==
         positions.size() * static_cast<std::size_t>((degree + 1) *
                                                     (degree + 1)));
  std::cout << "gaussian_count=" << positions.size()
            << " sh_degree=" << degree
            << " coefficient_count=" << coefficients.size() << '\n';
  return 0;
}
