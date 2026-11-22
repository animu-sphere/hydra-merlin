"""Bounded first-frame navigation of an external multi-mesh USD stage."""

import hashlib
import json
import math
import os
import runpy

from pxr import Gf, UsdGeom
from pxr.Usdviewq.common import ColorCorrectionModes, RenderModes
from pxr.Usdviewq.freeCamera import FreeCamera


def _scene_camera(recipe, is_z_up):
    eye = Gf.Vec3d(*recipe["eye"])
    target = Gf.Vec3d(*recipe["target"])
    fov = recipe["vertical_fov_degrees"]
    up = Gf.Vec3d.ZAxis() if is_z_up else Gf.Vec3d.YAxis()
    direction = target - eye
    assert all(math.isfinite(value) for value in (*eye, *target)), recipe["name"]
    assert Gf.Cross(direction, up).GetLength() > 0, f"{recipe['name']}: invalid look-at direction"
    assert 0 < fov < 180, f"{recipe['name']}: invalid vertical field of view"
    camera = Gf.Camera()
    camera.transform = Gf.Matrix4d().SetLookAt(eye, target, up).GetInverse()
    camera.focusDistance = direction.GetLength()
    camera.SetPerspectiveFromAspectRatioAndFieldOfView(1.0, fov, Gf.Camera.FOVVertical)
    return FreeCamera.FromGfCamera(camera, is_z_up)


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
    up_axis = UsdGeom.GetStageUpAxis(stage)
    recipe_path = os.environ.get("MERLIN_FORWARD_SCENE_VIEWS")
    if recipe_path:
        with open(recipe_path, encoding="utf-8") as stream:
            recipes = json.load(stream)
        view_policy = "world-space-look-at"
    else:
        recipes = [
            {"name": "full", "yaw_degrees": 0, "pitch_degrees": 0, "distance_scale": 1.0},
            {"name": "quarter", "yaw_degrees": 45, "pitch_degrees": -15, "distance_scale": 0.8},
            {"name": "opposite", "yaw_degrees": -45, "pitch_degrees": 15, "distance_scale": 0.65},
        ]
        view_policy = "independent-offsets-from-usdview-frame-all"
    viewpoints = []
    view_metadata = []
    for recipe in recipes:
        view.restoreViewState(framed)
        if recipe_path:
            settings.freeCamera = _scene_camera(recipe, up_axis == UsdGeom.Tokens.z)
        else:
            settings.freeCamera.Tumble(recipe["yaw_degrees"], recipe["pitch_degrees"])
            settings.freeCamera.AdjustDistance(recipe["distance_scale"])
        camera = view.resolveCamera()[0]
        viewpoints.append((recipe["name"], view.copyViewState()))
        view_metadata.append({**recipe,
                              "camera_transform": [list(row) for row in camera.transform]})
    view.restoreViewState(framed)
    source = stage.GetRootLayer().realPath
    with open(source, "rb") as stream:
        source_hash = hashlib.sha256(stream.read()).hexdigest()
    metadata = {"source": source, "root_layer_sha256": source_hash,
                "mesh_count": mesh_count, "up_axis": str(up_axis),
                "scene_lights": False, "materials": "authored",
                "view_policy": view_policy,
                "viewpoints": view_metadata}
    root, ext = os.path.splitext(os.environ["MERLIN_HYDRA2_SMOKE_IMAGE"])
    navigation = runpy.run_path(os.path.join(helper_dir, "forward_navigation_usdview_test.py"))
    navigation["check_forward_navigation"](
        appController, None, root, ext, helpers["_render_phase"],
        helpers["_read_events"], scene_metadata=metadata, viewpoints=viewpoints)
