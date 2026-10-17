"""Surface geometry normals through the real usdview host.

Each probe is a quad facing the static camera, activated alone so its center
pixel is the only sample. The renderer debug views are compared with an
analytic oracle in the linear Color AOV; Tier 0 and native Hgi presentation run
the same captures and are compared image-for-image afterwards.
"""

import json
import os

from pxr import Gf, UsdGeom, Vt
from pxr.Usdviewq.common import ColorCorrectionModes
from PySide6.QtGui import QImage

TILTED = (0.6, 0.0, 0.8)
VIEWS = ("geometric-normal", "shading-normal-base", "normal", "front-facing")

# name: (Ng, Ns_base, front-facing) as seen from the +Z camera, after the
# double-sided flip. All shading normals already face Ng, so Ns == Ns_base.
PROBES = {
    "flat": ((0, 0, 1), (0, 0, 1), True),
    "face-varying": ((0, 0, 1), TILTED, True),
    "left-handed": ((0, 0, 1), (0, 0, 1), True),
    "negative-scale": ((0, 0, 1), (-0.6, 0.0, 0.8), True),
    "double-sided-back": ((0, 0, 1), (0, 0, 1), False),
    "single-sided-back": ((0, 0, 1), (0, 0, 1), True),
}
PHASES = tuple(f"normals-{probe}-{view}" for probe in PROBES for view in VIEWS)


def _quad(stage, path, indices, normals=None, interpolation=None,
          orientation=None, double_sided=False, depth=0.0):
    mesh = UsdGeom.Mesh.Define(stage, path)
    mesh.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
    mesh.CreatePointsAttr(Vt.Vec3fArray([
        Gf.Vec3f(-1.2, -1.2, depth), Gf.Vec3f(1.2, -1.2, depth),
        Gf.Vec3f(1.2, 1.2, depth), Gf.Vec3f(-1.2, 1.2, depth)]))
    mesh.CreateFaceVertexCountsAttr(Vt.IntArray([4]))
    mesh.CreateFaceVertexIndicesAttr(Vt.IntArray(indices))
    if normals is not None:
        mesh.CreateNormalsAttr(Vt.Vec3fArray([Gf.Vec3f(*n) for n in normals]))
        mesh.SetNormalsInterpolation(interpolation)
    if orientation is not None:
        mesh.CreateOrientationAttr(orientation)
    mesh.CreateDoubleSidedAttr(double_sided)
    return mesh


def _define_probes(stage):
    counter_clockwise = [0, 1, 2, 3]
    clockwise = [0, 3, 2, 1]
    roots = {}
    for name in PROBES:
        # Phase names use hyphens; prim names cannot.
        roots[name] = UsdGeom.Xform.Define(
            stage, f"/NormalProbes/{name.replace('-', '_')}")
    _quad(stage, "/NormalProbes/flat/Quad", counter_clockwise)
    _quad(stage, "/NormalProbes/face_varying/Quad", counter_clockwise,
          [TILTED] * 4, UsdGeom.Tokens.faceVarying)
    # leftHanded reverses the authored clockwise order before Ng and the
    # generated flat normal are derived, so the quad faces +Z.
    _quad(stage, "/NormalProbes/left_handed/Quad", clockwise,
          orientation=UsdGeom.Tokens.leftHanded)
    # A mirrored transform keeps the quad front-facing and mirrors Ns.
    roots["negative-scale"].AddScaleOp().Set(Gf.Vec3f(-1, 1, 1))
    _quad(stage, "/NormalProbes/negative_scale/Quad", counter_clockwise,
          [TILTED] * 4, UsdGeom.Tokens.vertex)
    # Outward -Z, seen from behind: doubleSided keeps it and flips Ng/Ns_base.
    _quad(stage, "/NormalProbes/double_sided_back/Quad", clockwise,
          [(0, 0, -1)] * 4, UsdGeom.Tokens.vertex, double_sided=True)
    # A single-sided back face in front of a +Z reference must be culled; if
    # it were drawn, its unflipped -Z Ns_base would replace the reference.
    _quad(stage, "/NormalProbes/single_sided_back/Reference", counter_clockwise)
    _quad(stage, "/NormalProbes/single_sided_back/Back", clockwise,
          [(0, 0, -1)] * 4, UsdGeom.Tokens.vertex, depth=0.5)
    for root in roots.values():
        root.GetPrim().SetActive(False)
    return roots


def check_forward_normals(app_controller, root, ext, render_phase, read_events):
    stage = app_controller._dataModel.stage
    settings = app_controller._dataModel.viewSettings
    view = app_controller._stageView
    settings.colorCorrectionMode = ColorCorrectionModes.DISABLED
    stage.GetPrimAtPath("/ForwardProbe").SetActive(False)
    probes = _define_probes(stage)

    def center(phase):
        image = QImage(f"{root}-{phase}{ext}").convertToFormat(
            QImage.Format.Format_RGBA8888)
        assert not image.isNull(), f"{phase}: missing image"
        color = image.pixelColor(image.width() // 2, image.height() // 2)
        return bytes(image.constBits())[:image.sizeInBytes()], [
            color.red(), color.green(), color.blue()]

    def encode(vector):
        return [round(255 * (0.5 * value + 0.5)) for value in vector]

    evidence = []
    for name, (geometric, shading, front) in PROBES.items():
        probes[name].GetPrim().SetActive(True)
        view.SetRendererSetting("merlin:debugView", "none")
        render_phase(app_controller, f"normals-{name}-warmup")
        for debug_view in VIEWS:
            view.SetRendererSetting("merlin:debugView", debug_view)
            assert view.GetRendererSetting("merlin:debugView") == debug_view
            phase = f"normals-{name}-{debug_view}"
            render_phase(app_controller, phase)
            image, rgb = center(phase)
            event = render_phase(app_controller, phase)
            assert center(phase)[0] == image, f"{phase}: debug view flicker"
            for field in ("points_fetch_count", "topology_fetch_count",
                          "primvar_fetch_count", "upload_bytes",
                          "pipeline_creation_count"):
                assert event[field] == 0, f"{phase}: unexpected {field}"
            if debug_view == "front-facing":
                expected = [255, 255, 255] if front else [0, 0, 0]
            elif debug_view == "geometric-normal":
                expected = encode(geometric)
            else:
                expected = encode(shading)
            assert max(abs(a - b) for a, b in zip(rgb, expected)) <= 2, (
                phase, rgb, expected)
            evidence.append({"phase": phase, "probe": name,
                             "debug_view": debug_view, "center_rgb": rgb,
                             "expected_rgb": expected,
                             "upload_bytes": event["upload_bytes"],
                             "pipeline_creation_count":
                                 event["pipeline_creation_count"]})
        probes[name].GetPrim().SetActive(False)
    view.SetRendererSetting("merlin:debugView", "none")
    with open(root + "-forward-normals.json", "w", encoding="utf-8") as stream:
        json.dump({"schema": "merlin-usdview-forward-normals/v1",
                   "backend": os.environ["MERLIN_HYDRA2_TEST_BACKEND"],
                   "center_channel_tolerance": 2,
                   "phases": evidence}, stream, indent=2)
        stream.write("\n")
