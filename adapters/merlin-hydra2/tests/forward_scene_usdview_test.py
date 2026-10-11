"""Bounded first-frame navigation of an external multi-mesh USD stage."""

import hashlib
import os
import runpy

from pxr import UsdGeom
from pxr.Usdviewq.common import ColorCorrectionModes, RenderModes


def testUsdviewInputFunction(appController):
    stage = appController._dataModel.stage
    settings = appController._dataModel.viewSettings
    view = appController._stageView
    settings.showBBoxes = False
    settings.showHUD = False
    settings.renderMode = RenderModes.SMOOTH_SHADED
    settings.colorCorrectionMode = ColorCorrectionModes.DISABLED
    settings.ambientLightOnly = False
    settings.domeLightEnabled = False
    # Isolate the host camera light while preserving authored materials,
    # topology, transforms, visibility and the stage's up axis.
    settings.enableSceneLights = False
    settings.autoComputeClippingPlanes = False
    appController._dataModel.selection.clearPrims()
    view.DrawAxis = lambda _: None
    view.SetPhysicalWindowSize(401, 301)
    view.updateView(resetCam=True, forceComputeBBox=True)
    mesh_count = sum(prim.IsA(UsdGeom.Mesh) for prim in stage.Traverse())
    assert mesh_count > 1, "external Forward fixture requires multiple meshes"

    helper_dir = os.path.dirname(os.environ["MERLIN_USDVIEW_SMOKE_HELPERS"])
    helpers = runpy.run_path(os.path.join(helper_dir, "usdview_smoke_test.py"))
    helpers["_render_phase"](appController, "baseline")
    view.switchToFreeCamera(False)
    framed = view.copyViewState()
    viewpoints = []
    view_metadata = []
    for name, yaw, pitch, distance_scale in (
            ("full", 0, 0, 1.0), ("quarter", 45, -15, 0.8),
            ("opposite", -45, 15, 0.65)):
        # Each pose derives independently from frame-all, preserving the
        # stage's up-axis handling and scene-relative navigation scale.
        view.restoreViewState(framed)
        settings.freeCamera.Tumble(yaw, pitch)
        settings.freeCamera.AdjustDistance(distance_scale)
        camera = view.resolveCamera()[0]
        viewpoints.append((name, view.copyViewState()))
        view_metadata.append({"name": name, "yaw_degrees": yaw,
                              "pitch_degrees": pitch,
                              "distance_scale": distance_scale,
                              "camera_transform": [list(row) for row in camera.transform]})
    view.restoreViewState(framed)
    source = stage.GetRootLayer().realPath
    with open(source, "rb") as stream:
        source_hash = hashlib.sha256(stream.read()).hexdigest()
    metadata = {"source": source, "root_layer_sha256": source_hash,
                "mesh_count": mesh_count, "up_axis": str(UsdGeom.GetStageUpAxis(stage)),
                "scene_lights": False, "materials": "authored",
                "view_policy": "independent-offsets-from-usdview-frame-all",
                "viewpoints": view_metadata}
    root, ext = os.path.splitext(os.environ["MERLIN_HYDRA2_SMOKE_IMAGE"])
    navigation = runpy.run_path(os.path.join(helper_dir, "forward_navigation_usdview_test.py"))
    navigation["check_forward_navigation"](
        appController, None, root, ext, helpers["_render_phase"],
        helpers["_read_events"], scene_metadata=metadata, viewpoints=viewpoints)
