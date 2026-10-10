"""Focused Forward fixture, independent of mesh/picking smoke failures."""

import json
import os
import runpy

from pxr import Gf, Sdf, UsdGeom, UsdShade, Vt
from pxr.Usdviewq.common import ColorCorrectionModes, RenderModes
from PySide6.QtGui import QImage

# testusdview executes callbacks without __file__; the CMake runner supplies
# the helper path explicitly, independent of cwd and host script location.
_helpers = runpy.run_path(os.environ["MERLIN_USDVIEW_SMOKE_HELPERS"])
_render_phase = _helpers["_render_phase"]
_read_events = _helpers["_read_events"]


def _check_forward_lighting(app_controller):
    """Isolate the real Hdx camera light and the host output transform."""
    stage = app_controller._dataModel.stage
    settings = app_controller._dataModel.viewSettings
    stage.GetPrimAtPath("/World").SetActive(False)
    mesh = UsdGeom.Mesh.Define(stage, "/ForwardProbe")
    mesh.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
    mesh.CreatePointsAttr(Vt.Vec3fArray([
        Gf.Vec3f(-1.2, -1.2, 0), Gf.Vec3f(1.2, -1.2, 0),
        Gf.Vec3f(1.2, 1.2, 0), Gf.Vec3f(-1.2, 1.2, 0)]))
    mesh.CreateFaceVertexCountsAttr(Vt.IntArray([4]))
    mesh.CreateFaceVertexIndicesAttr(Vt.IntArray([0, 1, 2, 3]))
    mesh.CreateNormalsAttr(Vt.Vec3fArray([Gf.Vec3f(0, 0, 1)] * 4))
    mesh.SetNormalsInterpolation(UsdGeom.Tokens.vertex)
    material = UsdShade.Material.Define(stage, "/ForwardMaterial")
    shader = UsdShade.Shader.Define(stage, "/ForwardMaterial/Preview")
    shader.CreateIdAttr("UsdPreviewSurface")
    shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(
        Gf.Vec3f(0.4, 0.1875, 0.1))
    material.CreateSurfaceOutput().ConnectToSource(
        shader.ConnectableAPI(), "surface")
    UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(material)
    app_controller._dataModel.selection.clearPrims()
    settings.renderMode = RenderModes.SMOOTH_SHADED
    settings.domeLightEnabled = False
    settings.enableSceneLights = True
    view = app_controller._stageView
    view.DrawAxis = lambda _: None
    view.SetPhysicalWindowSize(401, 301)
    camera_attr = stage.GetPrimAtPath("/Camera").GetAttribute("xformOp:transform")
    static = Gf.Matrix4d(1).SetTranslate(Gf.Vec3d(0, 0, 5))
    moving = Gf.Matrix4d(1).SetLookAt(
        Gf.Vec3d(2.5, 0, 4.330127), Gf.Vec3d(0), Gf.Vec3d(0, 1, 0)).GetInverse()
    root, ext = os.path.splitext(os.environ["MERLIN_HYDRA2_SMOKE_IMAGE"])
    evidence = []
    centers = {}
    settings.colorCorrectionMode = ColorCorrectionModes.DISABLED
    settings.ambientLightOnly = False
    camera_attr.Set(static)
    _render_phase(app_controller, "baseline")

    def pixels(phase):
        image = QImage(f"{root}-{phase}{ext}").convertToFormat(
            QImage.Format.Format_RGBA8888)
        assert not image.isNull()
        rgb = image.pixelColor(image.width() // 2, image.height() // 2)
        return bytes(image.constBits())[:image.sizeInBytes()], [
            rgb.red(), rgb.green(), rgb.blue()]

    for albedo, value in (("color", Gf.Vec3f(0.4, 0.1875, 0.1)),
                          ("white", Gf.Vec3f(1))):
        shader.GetInput("diffuseColor").Set(value)
        for output, correction in (
                ("linear", ColorCorrectionModes.DISABLED),
                ("srgb", ColorCorrectionModes.SRGB)):
            settings.colorCorrectionMode = correction
            for enabled in (False, True):
                settings.ambientLightOnly = enabled  # usdview's Camera Light checkbox
                images = {}
                for pose, transform in (("static", static), ("moving", moving),
                                        ("restored", static)):
                    camera_attr.Set(transform)
                    phase = f"lighting-{albedo}-{output}-{'on' if enabled else 'off'}-{pose}"
                    # First capture consumes the edit. Repeated converged captures
                    # check stability after motion and light re-creation.
                    _render_phase(app_controller, phase)
                    image, center = pixels(phase)
                    for _ in range(2):
                        event = _render_phase(app_controller, phase)
                        assert pixels(phase)[0] == image, f"{phase}: static flicker"
                    events = [e for e in _read_events() if e["phase"] == phase]
                    for event in events:
                        for field in ("points_fetch_count", "topology_fetch_count",
                                      "primvar_fetch_count", "upload_bytes",
                                      "pipeline_creation_count"):
                            assert event[field] == 0, f"{phase}: unexpected {field}"
                    assert event["draw_count"] == 1
                    assert event["generated_material_fallback_count"] == 0
                    assert all(0 < value < 255 for value in center), (
                        f"{phase}: center lost color headroom: {center}")
                    images[pose] = image
                    centers[(output, enabled, pose)] = center
                    evidence.append({"phase": phase, "center_rgb": center,
                                     "camera_light": enabled, "output": output,
                                     "albedo": albedo, "upload_bytes": event["upload_bytes"],
                                     "pipeline_creation_count": event["pipeline_creation_count"]})
                assert images["static"] == images["restored"], "camera return changed image"
                assert images["static"] != images["moving"], "camera motion had no effect"

        def srgb(value):
            linear = value / 255.0
            return round(255 * (12.92 * linear if linear <= 0.0031308 else
                                1.055 * linear ** (1 / 2.4) - 0.055))

        for enabled in (False, True):
            for pose in ("static", "moving", "restored"):
                linear = centers[("linear", enabled, pose)]
                display = centers[("srgb", enabled, pose)]
                assert all(abs(actual - srgb(value)) <= 2
                           for actual, value in zip(display, linear)), (
                    f"unexpected host sRGB transform: {linear} -> {display}")
        for output in ("linear", "srgb"):
            off = centers[(output, False, "static")]
            on = centers[(output, True, "static")]
            moved = centers[(output, True, "moving")]
            assert all(a > b for a, b in zip(off, on)), "camera-light energy regression"
            assert all(a >= b for a, b in zip(on, moved)), "camera-light direction regression"
            assert off == centers[(output, False, "moving")], "world-space fallback moved"
    with open(root + "-forward-lighting.json", "w", encoding="utf-8") as stream:
        json.dump({"schema": "merlin-usdview-forward-lighting/v1",
                   "off_policy": "unit-white-plus-z-diagnostic-fallback",
                   "srgb_center_tolerance": 2, "phases": evidence}, stream, indent=2)
        stream.write("\n")


def testUsdviewInputFunction(appController):
    settings = appController._dataModel.viewSettings
    settings.showBBoxes = False
    settings.showHUD = False
    _check_forward_lighting(appController)
