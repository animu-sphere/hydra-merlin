"""Exercise subdivision selection and live cage updates through usdview."""

import os
import runpy

from pxr import Gf, UsdGeom, Vt
from pxr.UsdAppUtils.complexityArgs import RefinementComplexities
from pxr.Usdviewq.common import ColorCorrectionModes, RenderModes
from PySide6.QtGui import QImage


def testUsdviewInputFunction(controller):
    data = controller._dataModel
    settings = data.viewSettings
    stage = data.stage
    view = controller._stageView
    stage.SetEditTarget(stage.GetSessionLayer())
    stage.GetPrimAtPath("/World").SetActive(False)
    fixture = stage.DefinePrim("/Subdivision")
    fixture.GetReferences().AddReference(
        os.environ["MERLIN_SUBDIVISION_FIXTURE"], "/Subdivision")
    data.selection.clearPrims()
    settings.showHUD = False
    settings.showBBoxes = False
    settings.colorCorrectionMode = ColorCorrectionModes.DISABLED
    settings.renderMode = RenderModes.SMOOTH_SHADED
    view.DrawAxis = lambda _: None
    view.SetPhysicalWindowSize(401, 301)
    camera = UsdGeom.Camera(stage.GetPrimAtPath("/Camera"))
    camera.GetFocalLengthAttr().Set(27)
    camera.GetPrim().GetAttribute("xformOp:transform").Set(
        Gf.Matrix4d().SetLookAt(Gf.Vec3d(3, 3.2, 7), Gf.Vec3d(0, 0.5, 0),
                               Gf.Vec3d(0, 1, 0)).GetInverse())

    default = UsdGeom.Mesh(stage.GetPrimAtPath("/Subdivision/DefaultCage"))
    sharp = UsdGeom.Mesh(stage.GetPrimAtPath("/Subdivision/SharpCage"))
    # The fixture supplies the same cage through an internal reference.
    # Isolate the default cage's points edit from the two reference prims.
    for mesh in (sharp, UsdGeom.Mesh(stage.GetPrimAtPath("/Subdivision/PolygonCage"))):
        mesh.GetPointsAttr().Set(default.GetPointsAttr().Get())
        mesh.GetHoleIndicesAttr().Set([])
    assert not default.GetSubdivisionSchemeAttr().HasAuthoredValueOpinion()
    assert default.GetSubdivisionSchemeAttr().Get() == UsdGeom.Tokens.catmullClark

    helpers = runpy.run_path(os.environ["MERLIN_USDVIEW_SMOKE_HELPERS"])
    root, extension = os.path.splitext(os.environ["MERLIN_HYDRA2_SMOKE_IMAGE"])

    def capture(phase, edit=None):
        helpers["_render_phase"](controller, phase, edit)
        image = QImage(f"{root}-{phase}{extension}").convertToFormat(
            QImage.Format.Format_RGBA8888)
        assert not image.isNull(), phase
        events = [e for e in helpers["_read_events"]() if e["phase"] == phase]
        return bytes(image.constBits()), events

    def counter(events, name):
        return sum(event[name] for event in events)

    def complexity(value):
        return lambda _: setattr(settings, "complexity", value)

    cage, events = capture("baseline", complexity(RefinementComplexities.LOW))
    assert counter(events, "subdivision_eval_count") == 0
    medium, events = capture("subdivision-medium", complexity(RefinementComplexities.MEDIUM))
    assert medium != cage
    assert counter(events, "subdivision_plan_count") == 4, events
    stable, events = capture("subdivision-static")
    assert stable == medium
    assert counter(events, "subdivision_eval_count") == 0
    assert counter(events, "upload_bytes") == 0

    def points_edit(_):
        points = Vt.Vec3fArray(default.GetPointsAttr().Get())
        points[6] += Gf.Vec3f(0, 0.6, 0.2)
        default.GetPointsAttr().Set(points)

    moved, events = capture("subdivision-points", points_edit)
    assert moved != medium
    assert counter(events, "subdivision_eval_count") == 1, events
    assert counter(events, "subdivision_plan_count") == 0

    creased, events = capture("subdivision-sharpness",
                              lambda _: sharp.GetCreaseSharpnessesAttr().Set([0.0]))
    assert creased != moved
    assert counter(events, "subdivision_plan_count") == 1

    _, events = capture("subdivision-topology",
                        lambda _: default.GetHoleIndicesAttr().Set([1]))
    assert counter(events, "subdivision_plan_count") == 1
    _, events = capture("subdivision-high", complexity(RefinementComplexities.HIGH))
    assert counter(events, "subdivision_plan_count") == 4
    _, events = capture("subdivision-very-high", complexity(RefinementComplexities.VERY_HIGH))
    assert counter(events, "subdivision_plan_count") == 4

    def wireframe(_):
        settings.renderMode = RenderModes.WIREFRAME_ON_SURFACE

    capture("subdivision-refined-wire", wireframe)
    _, events = capture("subdivision-hull", lambda _: setattr(settings, "renderMode", RenderModes.FLAT_SHADED))
    assert counter(events, "subdivision_eval_count") == 0
