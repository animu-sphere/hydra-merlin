"""Exercise usdview's shading switches with overlapping polygon meshes."""

import os
import runpy

from pxr import Gf, Sdf, UsdGeom, UsdShade
from pxr.Usdviewq.common import ColorCorrectionModes, RenderModes
from PySide6.QtGui import QImage


def testUsdviewInputFunction(controller):
    data = controller._dataModel
    settings = data.viewSettings
    stage = data.stage
    view = controller._stageView
    stage.GetPrimAtPath("/World").SetActive(False)
    data.selection.clearPrims()
    settings.showHUD = False
    settings.showBBoxes = False
    settings.ambientLightOnly = True
    settings.domeLightEnabled = False
    settings.enableSceneLights = False
    settings.enableSceneMaterials = True
    settings.colorCorrectionMode = ColorCorrectionModes.DISABLED
    view.DrawAxis = lambda _: None
    view.SetPhysicalWindowSize(401, 301)

    camera = UsdGeom.Camera(stage.GetPrimAtPath("/Camera"))
    camera.GetFocalLengthAttr().Set(25)
    camera.GetPrim().GetAttribute("xformOp:transform").Set(
        Gf.Matrix4d().SetTranslate(Gf.Vec3d(0, 0, 5)))

    mesh = UsdGeom.Mesh.Define(stage, "/ShadingProbe")
    mesh.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
    mesh.CreatePointsAttr([
        (-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0),
        (-0.4, -0.4, -0.5), (0.4, -0.4, -0.5),
        (0.4, 0.4, -0.5), (-0.4, 0.4, -0.5),
    ])
    mesh.CreateFaceVertexCountsAttr([4, 4])
    mesh.CreateFaceVertexIndicesAttr(list(range(8)))
    mesh.CreateDisplayColorAttr([(0.2, 0.7, 0.4)])
    mesh.CreateNormalsAttr([(0.866, 0, 0.5)] * 8)
    mesh.SetNormalsInterpolation(UsdGeom.Tokens.vertex)
    material = UsdShade.Material.Define(stage, "/ShadingMaterial")
    shader = UsdShade.Shader.Define(stage, "/ShadingMaterial/Preview")
    shader.CreateIdAttr("UsdPreviewSurface")
    shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(
        Gf.Vec3f(0.8, 0.2, 0.43))
    material.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(), "surface")
    UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(material)

    helpers = runpy.run_path(os.environ["MERLIN_USDVIEW_SMOKE_HELPERS"])
    root, extension = os.path.splitext(os.environ["MERLIN_HYDRA2_SMOKE_IMAGE"])

    def render(mode, phase):
        settings.renderMode = mode
        helpers["_render_phase"](controller, phase)
        image = QImage(f"{root}-{phase}{extension}").convertToFormat(
            QImage.Format.Format_RGBA8888)
        assert not image.isNull(), phase
        return image

    def center(image):
        return image.pixelColor(image.width() // 2, image.height() // 2).getRgb()[:3]

    def pixels(image):
        return bytes(image.constBits())[:image.sizeInBytes()]

    smooth = render(RenderModes.SMOOTH_SHADED, "baseline")
    flat = render(RenderModes.FLAT_SHADED, "flat")
    assert center(smooth)[0] > center(smooth)[1], center(smooth)
    assert center(flat)[0] > center(smooth)[0] + 10, (center(flat), center(smooth))
    assert pixels(render(RenderModes.GEOM_SMOOTH, "geom-smooth")) == pixels(smooth)
    assert pixels(render(RenderModes.GEOM_FLAT, "geom-flat")) == pixels(flat)

    geom = render(RenderModes.GEOM_ONLY, "geom-only")
    assert all(abs(a - b) <= 1 for a, b in zip(center(geom), (51, 178, 102))), center(geom)
    settings.ambientLightOnly = False
    assert pixels(render(RenderModes.GEOM_ONLY, "geom-lights-off")) == pixels(geom)
    settings.ambientLightOnly = True
    settings.enableSceneMaterials = False
    fallback = center(render(RenderModes.SMOOTH_SHADED, "materials-off"))
    assert fallback[1] > fallback[0] + 10, fallback
    settings.enableSceneMaterials = True

    wire = render(RenderModes.WIREFRAME, "wireframe")
    hidden = render(RenderModes.HIDDEN_SURFACE_WIREFRAME, "hidden-wireframe")
    assert pixels(render(RenderModes.WIREFRAME, "wireframe-restored")) == pixels(wire)
    overlay = render(RenderModes.WIREFRAME_ON_SURFACE, "wireframe-on-surface")
    points = render(RenderModes.POINTS, "points")
    background = wire.pixel(0, 0)
    cx, cy = wire.width() // 2, wire.height() // 2
    assert wire.pixel(cx, cy) == background, "wireframe drew a triangulation diagonal"
    assert points.pixel(cx, cy) == background, "points filled the mesh interior"
    assert center(overlay) == center(smooth), "edges changed the surface shading"
    assert pixels(overlay) != pixels(smooth), "wireframe overlay is missing"
    assert max(center(hidden)) - min(center(hidden)) <= 1 and 60 <= center(hidden)[0] <= 90
    assert sum(wire.pixel(x, y) != background
               for x in range(cx - 50, cx + 50) for y in range(cy - 50, cy + 50)) > 100
    assert all(hidden.pixel(x, y) == hidden.pixel(cx, cy)
               for x in range(cx - 50, cx + 50) for y in range(cy - 50, cy + 50)), (
                   "hidden surface mode exposed rear edges")
    point_pixels = sum(points.pixel(x, y) != background
                       for x in range(points.width()) for y in range(points.height()))
    assert 20 <= point_pixels <= 150, point_pixels
    assert pixels(render(RenderModes.SMOOTH_SHADED, "smooth-restored")) == pixels(smooth)
