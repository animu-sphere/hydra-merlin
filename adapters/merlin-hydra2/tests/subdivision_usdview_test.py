"""Exercise subdivision selection and live cage updates through usdview."""

import os
import runpy

from pxr import Gf, UsdGeom, UsdShade, Vt
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
        action = next(action for action in controller._complexityActions
                      if action.text() == value.name)

        def select(_):
            action.trigger()
            assert action.isChecked() and settings.complexity == value

        return select

    cage, events = capture("baseline", complexity(RefinementComplexities.LOW))
    assert counter(events, "subdivision_eval_count") == 0
    medium, events = capture("subdivision-medium", complexity(RefinementComplexities.MEDIUM))
    assert medium != cage
    assert counter(events, "subdivision_plan_count") == 4, events
    stable, events = capture("subdivision-static")
    assert stable == medium
    assert counter(events, "subdivision_eval_count") == 0
    assert counter(events, "upload_bytes") == 0
    high, events = capture("subdivision-high", complexity(RefinementComplexities.HIGH))
    assert high != medium
    assert counter(events, "subdivision_plan_count") == 4
    very_high, events = capture("subdivision-very-high", complexity(RefinementComplexities.VERY_HIGH))
    assert very_high != high
    assert counter(events, "subdivision_plan_count") == 4
    restored, events = capture("subdivision-low-restored", complexity(RefinementComplexities.LOW))
    assert restored == cage
    assert counter(events, "subdivision_eval_count") == 0
    restored, events = capture("subdivision-medium-restored", complexity(RefinementComplexities.MEDIUM))
    assert restored == medium
    assert counter(events, "subdivision_plan_count") == 4

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

    def wireframe(_):
        settings.renderMode = RenderModes.WIREFRAME_ON_SURFACE

    capture("subdivision-refined-wire", wireframe)
    _, events = capture("subdivision-hull", lambda _: setattr(settings, "renderMode", RenderModes.FLAT_SHADED))
    assert counter(events, "subdivision_eval_count") == 0

    strip = UsdGeom.Mesh(stage.GetPrimAtPath("/Subdivision/OpenEdgeAndCorner"))

    def isolate_strip(_):
        for child in fixture.GetChildren():
            if child != strip.GetPrim():
                UsdGeom.Imageable(child).MakeInvisible()
        settings.renderMode = RenderModes.SMOOTH_SHADED
        settings.complexity = RefinementComplexities.MEDIUM
        view.SetRendererSetting("merlin:debugView", "shading-normal-base")

    normals, _ = capture("subdivision-shading-normal-base", isolate_strip)
    tangents, _ = capture("subdivision-tangent", lambda _: view.SetRendererSetting(
        "merlin:debugView", "tangent"))
    bitangents, _ = capture("subdivision-bitangent", lambda _: view.SetRendererSetting(
        "merlin:debugView", "bitangent"))
    assert len({normals, tangents, bitangents}) == 3

    def bind_checker(_):
        material = UsdShade.Material.Define(stage, "/SubdivisionChecker")
        material.GetPrim().GetReferences().AddInternalReference("/World/BoundMaterial")
        UsdShade.MaterialBindingAPI.Apply(strip.GetPrim()).Bind(material)
        view.SetRendererSetting("merlin:debugView", "none")

    seam, seam_events = capture("subdivision-uv-seam", bind_checker)
    st = UsdGeom.PrimvarsAPI(strip).GetPrimvar("st")

    def join_uv_edge(_):
        # Reuse the right face's indices at the two shared geometric vertices.
        indices = Vt.IntArray(st.GetIndices())
        indices[2], indices[3] = indices[5], indices[4]
        st.SetIndices(indices)

    joined, events = capture("subdivision-uv-joined", join_uv_edge)
    assert joined != seam
    assert counter(events, "subdivision_plan_count") == 1, events
    for field in ("covered_pixels", "covered_x_sum", "covered_y_sum"):
        assert events[-1][field] == seam_events[-1][field], field
