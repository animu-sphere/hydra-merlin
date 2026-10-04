#!/usr/bin/env python3
"""Create the renderer benchmark's deterministic degree-0 USD distribution.

Run with the same OpenUSD Python/runtime used by testusdview. The camera is
framed by usdview; renderer and host captures are separate experiments.
"""
import argparse
from pathlib import Path

from pxr import Gf, Sdf, Usd, UsdGeom, Vt


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--particles", type=int, choices=(1_000_000, 5_000_000, 10_000_000), default=1_000_000)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    stage = Usd.Stage.CreateNew(str(args.output))
    root = UsdGeom.Xform.Define(stage, "/Asset").GetPrim()
    stage.SetDefaultPrim(root)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    UsdGeom.SetStageMetersPerUnit(stage, 1.0)
    splat = stage.DefinePrim("/Asset/Splat", "ParticleField3DGaussianSplat")
    count = args.particles
    positions = Vt.Vec3fArray(count)
    for index in range(count):
        positions[index] = Gf.Vec3f((index % 1000) * 0.0018 - 0.9,
            ((index // 1000) % 1000) * 0.0018 - 0.9, 0.1 + ((index // 1_000_000) % 10) * 0.03)
    splat.CreateAttribute("positions", Sdf.ValueTypeNames.Point3fArray).Set(positions)
    # ParticleField has no automatic extent computation in this OpenUSD
    # runtime. Author conservative three-sigma bounds so usdview frames the
    # particles instead of leaving the free camera at its default distance.
    UsdGeom.Boundable(splat).CreateExtentAttr(Vt.Vec3fArray([
        Gf.Vec3f(-0.906, -0.906, 0.094),
        Gf.Vec3f(0.9042, 0.9042, 0.106 + ((count - 1) // 1_000_000) * 0.03),
    ]))
    splat.CreateAttribute("scales", Sdf.ValueTypeNames.Float3Array).Set(
        Vt.Vec3fArray([Gf.Vec3f(0.002)] * count))
    splat.CreateAttribute("orientations", Sdf.ValueTypeNames.QuatfArray).Set(
        Vt.QuatfArray([Gf.Quatf(1)] * count))
    splat.CreateAttribute("opacities", Sdf.ValueTypeNames.FloatArray).Set(Vt.FloatArray([0.7] * count))
    splat.CreateAttribute("radiance:sphericalHarmonicsDegree", Sdf.ValueTypeNames.Int).Set(0)
    splat.CreateAttribute("radiance:sphericalHarmonicsCoefficients", Sdf.ValueTypeNames.Float3Array).Set(
        Vt.Vec3fArray([Gf.Vec3f(0.35, 0.55, 0.8)] * count))
    stage.GetRootLayer().Save()


if __name__ == "__main__":
    main()
