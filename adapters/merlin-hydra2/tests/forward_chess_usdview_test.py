"""Open Chess Set and controlled Forward material checks through usdview."""

import hashlib
import json
import math
import os
from pathlib import Path
import runpy

from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux, UsdShade
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
        return event, image

    def pixels(image):
        return bytes(image.constBits())[:image.sizeInBytes()]

    baseline, material_pixels = capture("baseline")
    # Board and all 32 pieces, including both point-instanced pawn groups.
    assert baseline["draw_count"] >= 33, baseline["draw_count"]
    assert baseline["textured_materials"] >= 13, baseline["textured_materials"]
    assert baseline["normal_map_materials"] >= 13, baseline["normal_map_materials"]
    assert baseline["metallic_map_materials"] >= 11, baseline
    assert baseline["roughness_map_materials"] >= 15, baseline
    assert baseline["normal_map_disabled_draws"] == 0, baseline
    assert baseline["material_fallbacks"] == 0, baseline

    settings.domeLightEnabled = True
    _, dome_pixels = capture("chess-dome")
    settings.domeLightEnabled = False

    settings.renderMode = RenderModes.GEOM_ONLY
    _, geometry_pixels = capture("chess-geometry")
    settings.renderMode = RenderModes.SMOOTH_SHADED
    view.SetRendererSetting("merlin:debugView", "shading-normal-base")
    _, base_normals = capture("chess-normal-base")
    view.SetRendererSetting("merlin:debugView", "normal")
    _, mapped_normals = capture("chess-normal")
    view.SetRendererSetting("merlin:debugView", "none")
    restored, restored_pixels = capture("chess-restored")
    changed_material = _changed_pixels(pixels(material_pixels), pixels(geometry_pixels))
    changed_dome = _changed_pixels(pixels(material_pixels), pixels(dome_pixels))
    changed_normal = _changed_pixels(pixels(base_normals), pixels(mapped_normals))
    assert changed_material > baseline["covered_pixels"] * 0.25, changed_material
    assert changed_normal > baseline["covered_pixels"] * 0.01, changed_normal
    assert changed_dome > baseline["covered_pixels"] * 0.25, changed_dome
    assert pixels(restored_pixels) == pixels(material_pixels), "material capture did not restore"
    assert restored["upload_bytes"] == 0, restored
    assert restored["pipeline_creation_count"] == 0, restored

    with Usd.EditContext(stage, stage.GetSessionLayer()):
        material_differences = _check_chess_inputs(
            stage, pixels(material_pixels), capture, pixels)
        specular, emission = _check_standard_surface(
            stage, settings, view, scene_camera, capture)

    evidence = {
        "schema": "merlin-usdview-forward-chess/v2",
        "backend": os.environ["MERLIN_HYDRA2_TEST_BACKEND"],
        "root_layer_sha256": hashlib.sha256(
            Path(stage.GetRootLayer().realPath).read_bytes()).hexdigest(),
        "camera": camera,
        "exposure_ev": exposure_ev,
        "draw_count": baseline["draw_count"],
        "textured_materials": baseline["textured_materials"],
        "normal_map_materials": baseline["normal_map_materials"],
        "metallic_map_materials": baseline["metallic_map_materials"],
        "roughness_map_materials": baseline["roughness_map_materials"],
        "covered_pixels": baseline["covered_pixels"],
        "material_changed_pixels": changed_material,
        "normal_changed_pixels": changed_normal,
        "dome_changed_pixels": changed_dome,
        "authored_input_changed_pixels": material_differences,
        "specular_probe": specular,
        "emission_probe": emission,
    }
    Path(f"{root}-forward-chess.json").write_text(
        json.dumps(evidence, indent=2) + "\n", encoding="utf-8")


def _check_chess_inputs(stage, baseline, capture, pixels):
    # Make temporary overrides in usdview's session layer. Each image changes
    # one input on the board, the black pieces, or the white pieces.
    inputs = (("base_color", Gf.Vec3f(0.18)), ("normal", None),
              ("metalness", 0.75), ("specular_roughness", 0.8))
    differences = {}
    for group in ("Chessboard", "Black", "White"):
        prims = Usd.PrimRange(stage.GetPrimAtPath(f"/ChessSet/{group}"))
        shaders = [UsdShade.Shader(prim) for prim in prims
                   if prim.IsA(UsdShade.Shader) and UsdShade.Shader(prim).GetIdAttr().Get()
                   == "ND_standard_surface_surfaceshader"]
        assert shaders, group
        for name, value in inputs:
            connected = [shader.GetInput(name) for shader in shaders
                         if shader.GetInput(name).HasConnectedSource()]
            assert connected, (group, name)
            for shader_input in connected:
                shader_input.DisconnectSource()
                if value is not None:
                    shader_input.Set(value)
            phase = f"chess-{group.lower()}-{name}"
            _, image = capture(phase)
            differences[phase] = _changed_pixels(baseline, pixels(image))
            assert differences[phase] > 8, (phase, differences[phase])
            for shader_input in connected:
                shader_input.ClearSources()
                if value is not None:
                    shader_input.GetAttr().Clear()
    _, restored = capture("chess-inputs-restored")
    assert pixels(restored) == baseline, "authored inputs did not restore"
    return differences


def _check_standard_surface(stage, settings, view, scene_camera, capture):
    stage.GetDefaultPrim().SetActive(False)
    mesh = UsdGeom.Mesh.Define(stage, "/SpecularProbe")
    mesh.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
    mesh.CreatePointsAttr([(-8, -8, 0), (8, -8, 0), (8, 8, 0), (-8, 8, 0)])
    mesh.CreateFaceVertexCountsAttr([4])
    mesh.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    mesh.CreateNormalsAttr([(0, 0, 1)] * 4)
    mesh.SetNormalsInterpolation(UsdGeom.Tokens.vertex)
    material = UsdShade.Material.Define(stage, "/SpecularMaterial")
    shader = UsdShade.Shader.Define(stage, "/SpecularMaterial/Surface")
    shader.CreateIdAttr("ND_standard_surface_surfaceshader")
    shader.CreateInput("base_color", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(0.6, 0.2, 0.1))
    metallic = shader.CreateInput("metalness", Sdf.ValueTypeNames.Float)
    roughness = shader.CreateInput("specular_roughness", Sdf.ValueTypeNames.Float)
    material.CreateSurfaceOutput("mtlx").ConnectToSource(shader.ConnectableAPI(), "out")
    UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(material)
    light = UsdLux.DistantLight.Define(stage, "/SpecularLight")
    light.CreateAngleAttr(10)
    light.CreateIntensityAttr(0.32 / math.sin(math.radians(5)) ** 2)
    settings.freeCamera = scene_camera({"name": "specular probe", "eye": [0, 0, 3],
        "target": [0, 0, 0], "vertical_fov_degrees": 85}, False)
    settings.colorCorrectionMode = ColorCorrectionModes.DISABLED
    settings.domeLightTexturesVisible = False
    view.SetRendererSetting("merlin:exposureEv", -1.0)

    def sample(phase, metal, rough, dome):
        metallic.Set(metal)
        roughness.Set(rough)
        settings.domeLightEnabled = dome
        event, image = capture(phase)
        assert event["draw_count"] == 1 and event["material_fallbacks"] == 0, event
        x, y = image.width() // 2, image.height() // 2
        center = image.pixelColor(x, y).red()
        half_width = sum(image.pixelColor(column, y).red() * 2 >= center
                         for column in range(image.width()))
        return {"center_red": center, "width_at_half_peak": half_width,
                "image_width": image.width()}

    result = {}
    for source in ("camera", "distant"):
        settings.ambientLightOnly = source == "camera"
        settings.enableSceneLights = source == "distant"
        for dome in (False, True):
            prefix = f"specular-{source}-{'dome' if dome else 'direct'}"
            dielectric = sample(f"{prefix}-dielectric", 0.0, 0.4, dome)
            narrow = sample(f"{prefix}-narrow", 1.0, 0.4, dome)
            broad = sample(f"{prefix}-broad", 1.0, 0.6, dome)
            if not dome:
                assert narrow["center_red"] > broad["center_red"] > 0, (narrow, broad)
                assert narrow["center_red"] > dielectric["center_red"], (narrow, dielectric)
                assert (0 < narrow["width_at_half_peak"] < broad["width_at_half_peak"] <
                        broad["image_width"]), (narrow, broad)
            result[prefix] = {"dielectric": dielectric, "narrow": narrow, "broad": broad}
        direct, with_dome = (result[f"specular-{source}-{state}"] for state in ("direct", "dome"))
        assert with_dome["dielectric"]["center_red"] > direct["dielectric"]["center_red"]
        assert with_dome["narrow"]["center_red"] > direct["narrow"]["center_red"]
        assert with_dome["broad"]["center_red"] > direct["broad"]["center_red"]

    settings.ambientLightOnly = False
    settings.enableSceneLights = False
    unlit = sample("specular-dome-only-off", 1.0, 0.045, False)
    sharp = sample("specular-dome-only-sharp", 1.0, 0.045, True)
    rough = sample("specular-dome-only-rough", 1.0, 0.75, True)
    assert unlit["center_red"] == 0, unlit
    assert min(sharp["center_red"], rough["center_red"]) > 0, (sharp, rough)
    assert sharp != rough, (sharp, rough)
    result["specular-dome-only"] = {"unlit": unlit, "sharp": sharp, "rough": rough}
    settings.domeLightEnabled = False
    return result, _check_emission(stage, view, shader, capture)


def _check_emission(stage, view, shader, capture):
    # All three sources are disabled. In linear output the center should be
    # exactly the authored weight times color, within AOV quantization.
    view.SetRendererSetting("merlin:exposureEv", 0.0)
    view.SetRendererSetting("merlin:toneMapping", "none")
    weight = shader.CreateInput("emission", Sdf.ValueTypeNames.Float)
    color = shader.CreateInput("emission_color", Sdf.ValueTypeNames.Color3f)
    result = {}

    def sample(name, expected):
        event, image = capture(f"emission-{name}")
        center = image.pixelColor(image.width() // 2, image.height() // 2).getRgb()[:3]
        assert all(abs(actual - round(255 * value)) <= 1
                   for actual, value in zip(center, expected)), (name, center, expected)
        assert event["draw_count"] == 1 and event["pipeline_creation_count"] == 0, event
        result[name] = center

    sample("default", (0, 0, 0))
    weight.Set(0.25)
    sample("white", (0.25, 0.25, 0.25))
    color.Set(Gf.Vec3f(1, 0.5, 0.25))
    sample("color", (0.25, 0.125, 0.0625))
    weight.Set(0.5)
    sample("brighter", (0.5, 0.25, 0.125))
    weight.Set(-0.5)
    sample("invalid", (0, 0, 0))
    weight.Set(0.5)
    connected = UsdShade.Shader.Define(stage, "/SpecularMaterial/EmissionColor")
    connected.CreateIdAttr("ND_constant_color3")
    connected.CreateInput("value", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(1))
    color.ConnectToSource(connected.ConnectableAPI(), "out")
    sample("connected", (0, 0, 0))
    color.ClearSources()
    sample("restored", (0.5, 0.25, 0.125))
    return result
