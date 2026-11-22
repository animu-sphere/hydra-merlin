"""Open Chess Set material and normal-map checks through usdview."""

import hashlib
import json
import os
from pathlib import Path
import runpy

from pxr.Usdviewq.common import ColorCorrectionModes, RenderModes
from PySide6.QtGui import QImage


def _changed_pixels(first, second):
    assert len(first) == len(second)
    return sum(
        any(abs(first[i + c] - second[i + c]) > 4 for c in range(3))
        for i in range(0, len(first), 4)
    )


def testUsdviewInputFunction(appController):
    stage = appController._dataModel.stage
    settings = appController._dataModel.viewSettings
    view = appController._stageView
    assert stage.GetDefaultPrim().GetName() == "ChessSet"
    settings.showBBoxes = False
    settings.showHUD = False
    settings.renderMode = RenderModes.SMOOTH_SHADED
    settings.colorCorrectionMode = ColorCorrectionModes.SRGB
    settings.enableSceneMaterials = True
    settings.enableSceneLights = False
    settings.ambientLightOnly = True
    settings.domeLightEnabled = False
    settings.autoComputeClippingPlanes = False
    appController._dataModel.selection.clearPrims()
    view.DrawAxis = lambda _: None
    view.SetPhysicalWindowSize(801, 601)
    view.updateView(resetCam=True, forceComputeBBox=True)
    view.switchToFreeCamera(False)

    helper_path = Path(os.environ["MERLIN_USDVIEW_SMOKE_HELPERS"])
    render_phase = runpy.run_path(str(helper_path))["_render_phase"]
    scene_camera = runpy.run_path(
        str(helper_path.with_name("forward_scene_usdview_test.py")))["_scene_camera"]
    camera = {"name": "board", "eye": [0.51, 0.63, 0.88],
              "target": [0, 0.035, 0], "vertical_fov_degrees": 42,
              "near": 0.02, "far": 5.0}
    free_camera = scene_camera(camera, False)
    free_camera.overrideNear = camera["near"]
    free_camera.overrideFar = camera["far"]
    settings.freeCamera = free_camera
    exposure_ev = 1.5
    view.SetRendererSetting("merlin:exposureEv", exposure_ev)
    root, ext = os.path.splitext(os.environ["MERLIN_HYDRA2_SMOKE_IMAGE"])

    def capture(phase):
        event = render_phase(appController, phase)
        image = QImage(f"{root}-{phase}{ext}").convertToFormat(
            QImage.Format.Format_RGBA8888)
        assert not image.isNull(), phase
        return event, bytes(image.constBits())[:image.sizeInBytes()]

    baseline, material_pixels = capture("baseline")
    # Board and all 32 pieces, including both point-instanced pawn groups.
    assert baseline["draw_count"] >= 33, baseline["draw_count"]
    assert baseline["textured_materials"] >= 13, baseline["textured_materials"]
    assert baseline["normal_map_materials"] >= 13, baseline["normal_map_materials"]
    assert baseline["normal_map_disabled_draws"] == 0, baseline
    assert baseline["material_fallbacks"] == 0, baseline

    settings.renderMode = RenderModes.GEOM_ONLY
    _, geometry_pixels = capture("chess-geometry")
    settings.renderMode = RenderModes.SMOOTH_SHADED
    view.SetRendererSetting("merlin:debugView", "shading-normal-base")
    _, base_normals = capture("chess-normal-base")
    view.SetRendererSetting("merlin:debugView", "normal")
    _, mapped_normals = capture("chess-normal")
    view.SetRendererSetting("merlin:debugView", "none")
    restored, restored_pixels = capture("chess-restored")
    changed_material = _changed_pixels(material_pixels, geometry_pixels)
    changed_normal = _changed_pixels(base_normals, mapped_normals)
    assert changed_material > baseline["covered_pixels"] * 0.25, changed_material
    assert changed_normal > baseline["covered_pixels"] * 0.01, changed_normal
    assert restored_pixels == material_pixels, "material capture did not restore"
    assert restored["upload_bytes"] == 0, restored
    assert restored["pipeline_creation_count"] == 0, restored

    evidence = {
        "schema": "merlin-usdview-forward-chess/v1",
        "backend": os.environ["MERLIN_HYDRA2_TEST_BACKEND"],
        "root_layer_sha256": hashlib.sha256(
            Path(stage.GetRootLayer().realPath).read_bytes()).hexdigest(),
        "camera": camera,
        "exposure_ev": exposure_ev,
        "draw_count": baseline["draw_count"],
        "textured_materials": baseline["textured_materials"],
        "normal_map_materials": baseline["normal_map_materials"],
        "covered_pixels": baseline["covered_pixels"],
        "material_changed_pixels": changed_material,
        "normal_changed_pixels": changed_normal,
    }
    Path(f"{root}-forward-chess.json").write_text(
        json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
