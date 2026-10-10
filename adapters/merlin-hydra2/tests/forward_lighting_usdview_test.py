"""Focused Forward fixture, independent of mesh/picking smoke failures."""

import json
import math
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

    if os.environ.get("MERLIN_FORWARD_MOTION_ONLY") == "1":
        _check_forward_motion(app_controller, shader, camera_attr, root, ext)
        return

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


def _check_forward_motion(app_controller, shader, camera_attr, root, ext):
    """Compare the first render of every moving pose to a settled reference.

    No event-loop iteration or convergence wait is allowed between a camera
    edit and its screenshot. Qt's framebuffer grab renders synchronously; the
    event count below verifies that it rendered exactly once on this host.
    """
    from pxr import Trace

    settings = app_controller._dataModel.viewSettings
    view = app_controller._stageView
    poses = []
    for index in range(17):
        angle = math.radians(30 * index / 16)
        poses.append(Gf.Matrix4d(1).SetLookAt(
            Gf.Vec3d(5 * math.sin(angle), 0, 5 * math.cos(angle)),
            Gf.Vec3d(0), Gf.Vec3d(0, 1, 0)).GetInverse())

    def pixels(phase):
        image = QImage(f"{root}-{phase}{ext}").convertToFormat(
            QImage.Format.Format_RGBA8888)
        assert not image.isNull(), f"{phase}: missing image"
        center = image.pixelColor(image.width() // 2, image.height() // 2)
        return bytes(image.constBits())[:image.sizeInBytes()], [
            center.red(), center.green(), center.blue()]

    evidence = []
    for albedo, value in (("color", Gf.Vec3f(0.4, 0.1875, 0.1)),
                          ("white", Gf.Vec3f(1))):
        shader.GetInput("diffuseColor").Set(value)
        for output, correction in (
                ("linear", ColorCorrectionModes.DISABLED),
                ("srgb", ColorCorrectionModes.SRGB)):
            settings.colorCorrectionMode = correction
            for enabled in (False, True):
                settings.ambientLightOnly = enabled
                prefix = f"motion-{albedo}-{output}-{'on' if enabled else 'off'}"
                references = []
                for index, transform in enumerate(poses):
                    camera_attr.Set(transform)
                    phase = f"{prefix}-reference-{index:02d}"
                    _render_phase(app_controller, phase)
                    references.append(pixels(phase)[0])
                # Settle only the material/output/light transition and starting
                # pose. The following sweep has no convergence or idle frames.
                camera_attr.Set(poses[0])
                _render_phase(app_controller, prefix + "-warmup")
                for leg, indices in (("out", range(1, len(poses))),
                                     ("back", range(len(poses) - 2, -1, -1))):
                    for index in indices:
                        phase = f"{prefix}-{leg}-{index:02d}"
                        trace = Trace.Collector()
                        trace.BeginEvent("MerlinPhase:" + phase)
                        try:
                            os.environ["MERLIN_HYDRA2_REGRESSION_PHASE"] = phase
                            camera_attr.Set(poses[index])
                            view.SetForceRefresh(True)
                            view.updateView()
                            app_controller._takeShot(
                                f"{root}-{phase}{ext}", iterations=0,
                                waitForConvergence=False)
                        finally:
                            trace.EndEvent("MerlinPhase:" + phase)
                        events = [e for e in _read_events() if e["phase"] == phase]
                        assert len(events) == 1, (
                            f"{phase}: expected one immediate render, got {len(events)}")
                        event = events[0]
                        image, center = pixels(phase)
                        assert image == references[index], (
                            f"{phase}: first moving frame differs from settled pose")
                        assert event["camera_aspects"] & (1 << 7), (
                            f"{phase}: camera edit did not reach the renderer")
                        assert event["buffers_written"] == 4
                        assert event["covered_pixels"] > 0
                        assert event["draw_count"] == 1
                        assert event["validation_enabled"] == 1
                        assert event["validation_messages"] == 0
                        assert event["generated_material_fallback_count"] == 0
                        for field in ("points_fetch_count", "topology_fetch_count",
                                      "primvar_fetch_count", "upload_bytes",
                                      "pipeline_creation_count"):
                            assert event[field] == 0, f"{phase}: unexpected {field}"
                        assert all(0 < channel < 255 for channel in center), (
                            f"{phase}: lost color headroom: {center}")
                        evidence.append({
                            "phase": phase, "reference_phase":
                                f"{prefix}-reference-{index:02d}",
                            "pose_index": index, "yaw_degrees": 30 * index / 16,
                            "leg": leg, "albedo": albedo, "output": output,
                            "camera_light": enabled, "center_rgb": center,
                            "render_count": len(events),
                            "camera_resource_revision": event["camera_resource_revision"],
                            "upload_bytes": event["upload_bytes"],
                            "pipeline_creation_count": event["pipeline_creation_count"]})
                assert references[0] != references[-1], "motion had no image effect"
    with open(root + "-forward-motion.json", "w", encoding="utf-8") as stream:
        json.dump({"schema": "merlin-usdview-forward-motion/v1",
                   "capture_policy": "one-render-no-event-loop-no-convergence-wait",
                   "pose_count": len(poses), "maximum_yaw_degrees": 30,
                   "first_frame_reference_channel_tolerance": 0,
                   "phases": evidence}, stream, indent=2)
        stream.write("\n")


def testUsdviewInputFunction(appController):
    settings = appController._dataModel.viewSettings
    settings.showBBoxes = False
    settings.showHUD = False
    _check_forward_lighting(appController)
