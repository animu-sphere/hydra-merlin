import os
import json

from pxr import Gf, Sdf, Trace, UsdGeom, UsdShade, Vt
from pxr.Usdviewq.common import PickModes, SelectionHighlightModes
from PySide6.QtCore import QPoint, Qt
from PySide6.QtGui import QImage
from PySide6.QtTest import QTest


def _read_events():
    marker = os.environ["MERLIN_HYDRA2_REGRESSION_LOG"]
    if not os.path.exists(marker):
        return []
    string_fields = {
        "phase",
        "material_effective_fallback",
        "hgi_transfer_mode",
        "hgi_direct_share_rejection",
        "hgi_metal_transfer_mode",
        "hgi_metal_direct_share_rejection",
        "gaussian_gpu_mode",
        "gaussian_raster_path",
    }
    events = []
    with open(marker, encoding="utf-8") as stream:
        for line in stream:
            event = {}
            for field in line.split():
                key, value = field.split("=", 1)
                event[key] = value if key in string_fields else int(value)
            events.append(event)
    return events


def _render_phase(app_controller, phase, edit=None, size=None):
    trace = Trace.Collector()
    trace_label = f"MerlinPhase:{phase}"
    trace.BeginEvent(trace_label)
    try:
        return _render_phase_traced(
            app_controller, phase, edit=edit, size=size)
    finally:
        trace.EndEvent(trace_label)


def _render_phase_traced(app_controller, phase, edit=None, size=None):
    os.environ["MERLIN_HYDRA2_REGRESSION_PHASE"] = phase
    previous_count = sum(
        event["phase"] == phase for event in _read_events())

    if edit is not None:
        edit(app_controller._dataModel.stage)
    if size is not None:
        app_controller._stageView.SetPhysicalWindowSize(*size)
    app_controller._stageView.SetForceRefresh(True)
    app_controller._stageView.updateView()

    image_root, image_extension = os.path.splitext(
        os.environ["MERLIN_HYDRA2_SMOKE_IMAGE"])
    app_controller._takeShot(
        f"{image_root}-{phase}{image_extension}",
        iterations=20,
        waitForConvergence=True)

    matching = [
        event for event in _read_events() if event["phase"] == phase
    ]
    assert len(matching) > previous_count, f"no render completed for {phase}"
    event = matching[-1]
    assert event["buffers_written"] == 4
    assert event["covered_pixels"] > 0
    assert event["validation_enabled"] == 1
    assert event["validation_messages"] == 0
    return event


def _events_with_aspect(phase, field, aspect):
    return [
        event for event in _read_events()
        if event["phase"] == phase and event[field] & aspect
    ]


def _move_triangle(stage):
    stage.GetPrimAtPath(
        "/World/MovingTriangle").GetAttribute(
            "xformOp:translate").Set(Gf.Vec3d(-0.15, 0.0, 0.0))


def _edit_points(stage):
    stage.GetPrimAtPath(
        "/World/MovingTriangle").GetAttribute("points").Set(
            Vt.Vec3fArray([
                Gf.Vec3f(-0.35, -0.45, 0.0),
                Gf.Vec3f(0.35, -0.45, 0.0),
                Gf.Vec3f(0.0, 0.60, 0.0),
            ]))


def _edit_topology(stage):
    stage.GetPrimAtPath(
        "/World/MovingTriangle").GetAttribute(
            "faceVertexIndices").Set(Vt.IntArray([1, 2, 0]))


def _edit_primvar(stage):
    stage.GetPrimAtPath(
        "/World/ConcavePrimvarMesh").GetAttribute(
            "primvars:displayColor").Set(Vt.Vec3fArray([
                Gf.Vec3f(0.8, 0.1, 0.2),
                Gf.Vec3f(0.1, 0.8, 0.2),
                Gf.Vec3f(0.1, 0.3, 1.0),
            ]))


def _hide_triangle(stage):
    UsdGeom.Imageable(
        stage.GetPrimAtPath("/World/VisibilityTriangle")
    ).GetVisibilityAttr().Set(UsdGeom.Tokens.invisible)


def _move_camera(stage):
    transform = Gf.Matrix4d(1.0)
    transform.SetTranslate(Gf.Vec3d(0.35, 0.0, 5.0))
    stage.GetPrimAtPath(
        "/Camera").GetAttribute("xformOp:transform").Set(transform)


def _edit_material_parameter(stage):
    stage.GetPrimAtPath(
        "/World/BoundMaterial/Preview").GetAttribute(
            "inputs:roughness").Set(0.2)


def _break_topology(stage):
    stage.GetPrimAtPath(
        "/World/MovingTriangle").GetAttribute(
            "faceVertexIndices").Set(Vt.IntArray([0, 1, 99]))


def _recover_topology(stage):
    stage.GetPrimAtPath(
        "/World/MovingTriangle").GetAttribute(
            "faceVertexIndices").Set(Vt.IntArray([0, 1, 2]))


def _remove_mesh(stage):
    stage.GetPrimAtPath("/World/MovingTriangle").SetActive(False)


def _readd_mesh(stage):
    stage.GetPrimAtPath("/World/MovingTriangle").SetActive(True)


def _check_mesh_orientation(app_controller):
    """Equivalent handedness must preserve lighting and face-varying inputs."""
    stage = app_controller._dataModel.stage
    mesh = UsdGeom.Mesh.Define(stage, "/World/OrientationProbe")
    mesh.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
    mesh.CreatePointsAttr(Vt.Vec3fArray([
        Gf.Vec3f(-0.55, -0.5, 0.2), Gf.Vec3f(0.55, -0.5, 0.2),
        Gf.Vec3f(0.55, 0.5, 0.2), Gf.Vec3f(-0.55, 0.5, 0.2)]))
    mesh.CreateFaceVertexCountsAttr(Vt.IntArray([4]))
    primvars = UsdGeom.PrimvarsAPI(mesh)
    colors = primvars.CreatePrimvar(
        "displayColor", Sdf.ValueTypeNames.Color3fArray,
        UsdGeom.Tokens.faceVarying)
    colors.Set(Vt.Vec3fArray([
        Gf.Vec3f(1, 0.2, 0.1), Gf.Vec3f(0.1, 0.8, 0.2),
        Gf.Vec3f(0.1, 0.3, 1), Gf.Vec3f(0.8, 0.7, 0.2)]))
    uv = primvars.CreatePrimvar(
        "st", Sdf.ValueTypeNames.TexCoord2fArray,
        UsdGeom.Tokens.faceVarying)
    uv.Set(Vt.Vec2fArray([
        Gf.Vec2f(0, 0), Gf.Vec2f(1, 0),
        Gf.Vec2f(1, 1), Gf.Vec2f(0, 1)]))
    UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(
        UsdShade.Material(stage.GetPrimAtPath("/World/BoundMaterial")))

    def set_handedness(left):
        order = [0, 3, 2, 1] if left else [0, 1, 2, 3]
        mesh.GetOrientationAttr().Set(
            UsdGeom.Tokens.leftHanded if left else UsdGeom.Tokens.rightHanded)
        mesh.GetFaceVertexIndicesAttr().Set(Vt.IntArray(order))
        colors.SetIndices(Vt.IntArray(order))
        uv.SetIndices(Vt.IntArray(order))
        normals = primvars.GetPrimvar("normals")
        if normals and normals.HasValue():
            normals.SetIndices(Vt.IntArray(order))

    def pixels(phase):
        root, ext = os.path.splitext(os.environ["MERLIN_HYDRA2_SMOKE_IMAGE"])
        image = QImage(f"{root}-{phase}{ext}").convertToFormat(
            QImage.Format.Format_RGBA8888)
        assert not image.isNull()
        return bytes(image.constBits())[:image.sizeInBytes()]

    for authored in (False, True):
        if authored:
            normals = primvars.CreatePrimvar(
                "normals", Sdf.ValueTypeNames.Normal3fArray,
                UsdGeom.Tokens.faceVarying)
            # Distinct normals expose accidental corner remapping or sign flips.
            normals.Set(Vt.Vec3fArray([
                Gf.Vec3f(-0.2, 0, 1), Gf.Vec3f(0.2, 0, 1),
                Gf.Vec3f(0.2, 0.3, 1), Gf.Vec3f(-0.2, 0.3, 1)]))
        prefix = "orientation-authored" if authored else "orientation-generated"
        right = prefix + "-right"
        left = prefix + "-left"
        _render_phase(app_controller, right, edit=lambda _: set_handedness(False))
        _render_phase(app_controller, left, edit=lambda _: set_handedness(True))
        assert pixels(right) == pixels(left), (
            f"{prefix}: equivalent left/right meshes changed the lit image")

    # An orientation-only edit must invalidate topology, even without changing
    # points or indices. Generated normals must change and then recover.
    mesh.GetPrim().RemoveProperty("primvars:normals")
    mesh.GetPrim().RemoveProperty("primvars:normals:indices")
    _render_phase(app_controller, "orientation-restored-left")
    _render_phase(
        app_controller, "orientation-only-right",
        edit=lambda _: mesh.GetOrientationAttr().Set(UsdGeom.Tokens.rightHanded))
    assert (pixels("orientation-restored-left") !=
            pixels("orientation-only-right"))
    _render_phase(
        app_controller, "orientation-only-left",
        edit=lambda _: mesh.GetOrientationAttr().Set(UsdGeom.Tokens.leftHanded))
    assert (pixels("orientation-restored-left") ==
            pixels("orientation-only-left"))


def _check_click_picking(app_controller):
    """Send real Qt mouse events through usdview's narrowed-frustum pick task."""
    view = app_controller._stageView
    model = app_controller._dataModel
    model.viewSettings.pickMode = PickModes.PRIMS
    model.viewSettings.selHighlightMode = SelectionHighlightModes.ALWAYS
    model.selection.clearPrims()
    evidence = []
    path = Sdf.Path("/World/OrientationProbe")

    def click(phase, point, expected):
        os.environ["MERLIN_HYDRA2_REGRESSION_PHASE"] = phase
        hits = []
        def selected(prim_path, *args):
            hits.append(str(prim_path))
        view.signalPrimSelected.connect(selected)
        try:
            QTest.mouseClick(view, Qt.MouseButton.LeftButton,
                             Qt.KeyboardModifier.NoModifier, point)
        finally:
            view.signalPrimSelected.disconnect(selected)
        assert hits == [expected], f"{phase}: expected {expected}, got {hits}"
        if expected:
            assert model.selection.getPrimPaths() == [Sdf.Path(expected)]
        else:
            assert model.selection.getPrimPaths() == [Sdf.Path.absoluteRootPath]
        _render_phase(app_controller, phase)
        evidence.append({"phase": phase, "hit": hits[0],
                         "logical_position": [point.x(), point.y()],
                         "device_pixel_ratio": view.devicePixelRatioF()})

    def projected_center():
        camera, aspect = view.resolveCamera()
        frustum = camera.frustum
        transform = UsdGeom.Xformable(model.stage.GetPrimAtPath(path)).ComputeLocalToWorldTransform(
            model.currentFrame)
        center = transform.Transform(Gf.Vec3d(0, 0, 0.2))
        ndc = (frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix()).Transform(center)
        viewport = (view.computeCameraViewport(aspect) if view._cropImageToCameraViewport
                    else view.computeWindowViewport())
        ratio = view.devicePixelRatioF()
        return QPoint(round((viewport[0] + (ndc[0] + 1) * viewport[2] / 2) / ratio),
                      round((viewport[1] + (1 - ndc[1]) * viewport[3] / 2) / ratio))

    _render_phase(app_controller, "click-unselected", size=(597, 540))
    click("click-hit", projected_center(), str(path))
    # Check that the host's real OpenGL axis overlay is present, then compare
    # its composited image across Tier 0 and GPU copy using the existing policy.
    _render_phase(app_controller, "click-overlay")
    draw_axis = view.DrawAxis
    view.DrawAxis = lambda _: None
    try:
        _render_phase(app_controller, "click-overlay-hidden")
    finally:
        view.DrawAxis = draw_axis
    root, _ = os.path.splitext(os.environ["MERLIN_HYDRA2_SMOKE_IMAGE"])
    def pixels(phase):
        image = QImage(root + "-" + phase + ".png").convertToFormat(
            QImage.Format.Format_RGBA8888)
        assert not image.isNull()
        return bytes(image.constBits())[:image.sizeInBytes()]
    assert pixels("click-unselected") != pixels("click-hit"), "selection highlight absent"
    assert pixels("click-overlay") != pixels("click-overlay-hidden"), "axis overlay absent"
    click("click-miss", QPoint(5, 5), "")
    assert pixels("click-unselected") == pixels("click-miss"), "deselection changed the scene"
    UsdGeom.Xformable(model.stage.GetPrimAtPath(path)).AddTranslateOp().Set(Gf.Vec3d(0.65, 0, 0))
    _render_phase(app_controller, "click-moved", size=(401, 301))
    click("click-resized-hit", projected_center(), str(path))
    model.selection.clearPrims()
    with open(root + "-click-picking.json", "w", encoding="utf-8") as stream:
        json.dump({"schema": "merlin-usdview-click-picking/v1", "clicks": evidence},
                  stream, indent=2)
        stream.write("\n")


def testUsdviewInputFunction(appController):
    appController._dataModel.viewSettings.showBBoxes = False
    appController._dataModel.viewSettings.showHUD = False

    baseline = _render_phase(appController, "baseline")
    assert baseline["textured_materials"] >= 1
    assert baseline["vertex_color_materials"] == 1
    assert baseline["neutral_vertex_color_materials"] == 1
    assert baseline["texcoord_geometries"] == 1
    assert baseline["missing_texcoord_geometries"] == 4
    assert baseline["material_fallbacks"] == 0
    assert baseline["generated_material_draw_count"] == 0
    assert baseline["generated_material_fallback_count"] == 0
    assert baseline["material_fallback_recorded_count"] == 0
    assert baseline["material_effective_fallback"] == "none"
    assert baseline["texture_cache_hits"] >= 1
    assert baseline["schema_version"] == 4
    assert baseline["upload_bytes"] == 0
    assert baseline["allocation_count"] == 0
    assert baseline["pipeline_creation_count"] == 0
    assert baseline["shader_module_cache_misses"] == 0
    assert baseline["geometry_cache_misses"] == 0
    points = _render_phase(appController, "points", edit=_edit_points)
    topology = _render_phase(
        appController, "topology", edit=_edit_topology)
    primvar = _render_phase(
        appController, "primvar", edit=_edit_primvar)
    transformed = _render_phase(
        appController, "transform", edit=_move_triangle)
    hidden = _render_phase(
        appController, "visibility", edit=_hide_triangle)
    camera = _render_phase(appController, "camera", edit=_move_camera)
    material = _render_phase(
        appController, "material_parameter",
        edit=_edit_material_parameter)
    diagnostic = _render_phase(
        appController, "diagnostic", edit=_break_topology)
    recovery = _render_phase(
        appController, "recovery", edit=_recover_topology)
    removed = _render_phase(
        appController, "remove", edit=_remove_mesh)
    readded = _render_phase(
        appController, "readd", edit=_readd_mesh)
    resized = _render_phase(appController, "resize", size=(401, 301))

    assert baseline["draw_count"] == 7
    assert points["draw_count"] == 7
    assert points["scene_revision"] > baseline["scene_revision"]
    point_changes = _events_with_aspect("points", "mesh_aspects", 1 << 1)
    assert point_changes
    assert all(event["instance_aspects"] == 0 for event in point_changes)
    assert all(
        event["mesh_resource_revision"] >= 2
        for event in point_changes)

    assert topology["draw_count"] == 7
    assert topology["scene_revision"] > points["scene_revision"]
    topology_changes = _events_with_aspect(
        "topology", "mesh_aspects", 1 << 0)
    assert topology_changes
    assert all(
        event["instance_aspects"] == 0 for event in topology_changes)

    assert primvar["draw_count"] == 7
    assert primvar["scene_revision"] > topology["scene_revision"]
    primvar_changes = _events_with_aspect(
        "primvar", "mesh_aspects", 1 << 2)
    assert primvar_changes
    # OpenUSD 26.05 currently emits a primvars-container locator for this
    # edit. Merlin conservatively re-fetches cached source values, compares
    # them, and still avoids unrelated triangulation and upload.
    assert all(event["points_fetch_count"] == 1
               for event in primvar_changes)
    assert all(event["topology_fetch_count"] == 0
               for event in primvar_changes)
    assert all(event["primvar_fetch_count"] == 4
               for event in primvar_changes)
    assert all(event["coarse_primvar_invalidation_count"] == 1
               for event in primvar_changes)
    assert all(event["triangulation_rebuild_count"] == 0
               for event in primvar_changes)
    assert all(event["packed_mesh_rebuild_count"] == 1
               for event in primvar_changes)
    assert all(0 < event["changed_vertex_count"] < 9
               for event in primvar_changes)
    if os.environ.get("MERLIN_HYDRA2_TEST_BACKEND", "vulkan") == "vulkan":
        assert all(event["upload_bytes"] ==
                   event["changed_vertex_count"] * 48
                   for event in primvar_changes)
    else:
        # Metal currently replaces the changed geometry's retained vertex and
        # index buffers together; it must still avoid unrelated resources.
        assert all(event["upload_bytes"] >=
                   event["changed_vertex_count"] * 48
                   for event in primvar_changes)
        assert all(event["geometry_reconcile_count"] == 1
                   for event in primvar_changes)

    assert transformed["draw_count"] == 7
    assert transformed["scene_revision"] > primvar["scene_revision"]
    assert transformed["covered_x_sum"] != primvar["covered_x_sum"]
    transform_changes = _events_with_aspect(
        "transform", "instance_aspects", 1 << 3)
    assert transform_changes
    assert all(
        event["instance_aspects"] == 1 << 3
        for event in transform_changes)
    assert all(event["mesh_aspects"] == 0 for event in transform_changes)
    assert all(event["points_fetch_count"] == 0 for event in transform_changes)
    assert all(event["topology_fetch_count"] == 0 for event in transform_changes)
    assert all(event["primvar_fetch_count"] == 0 for event in transform_changes)
    assert all(event["triangulation_rebuild_count"] == 0
               for event in transform_changes)
    assert all(event["packed_mesh_rebuild_count"] == 0
               for event in transform_changes)
    assert all(event["upload_bytes"] == 0 for event in transform_changes)
    assert all(
        event["instance_resource_revision"] >= 2
        for event in transform_changes)

    assert hidden["draw_count"] == 6
    assert hidden["scene_revision"] > transformed["scene_revision"]
    assert hidden["covered_x_sum"] != transformed["covered_x_sum"]
    visibility_changes = _events_with_aspect(
        "visibility", "instance_aspects", 1 << 4)
    assert visibility_changes
    assert all(
        event["instance_aspects"] == 1 << 4
        for event in visibility_changes)
    assert all(event["mesh_aspects"] == 0 for event in visibility_changes)
    assert all(event["points_fetch_count"] == 0
               for event in visibility_changes)
    assert all(event["topology_fetch_count"] == 0
               for event in visibility_changes)
    assert all(event["primvar_fetch_count"] == 0
               for event in visibility_changes)
    assert all(event["triangulation_rebuild_count"] == 0
               for event in visibility_changes)
    assert all(event["packed_mesh_rebuild_count"] == 0
               for event in visibility_changes)
    assert all(event["upload_bytes"] == 0 for event in visibility_changes)

    assert camera["draw_count"] == 6
    assert camera["scene_revision"] > hidden["scene_revision"]
    assert camera["covered_x_sum"] != hidden["covered_x_sum"]
    camera_changes = _events_with_aspect(
        "camera", "camera_aspects", 1 << 7)
    assert camera_changes
    assert all(event["instance_aspects"] == 0 for event in camera_changes)
    assert all(
        event["camera_resource_revision"] >= 2
        for event in camera_changes)
    assert all(event["mesh_sync_count"] == 0 for event in camera_changes)
    assert all(event["points_fetch_count"] == 0 for event in camera_changes)
    assert all(event["topology_fetch_count"] == 0 for event in camera_changes)
    assert all(
        event["primvar_descriptor_fetch_count"] == 0
        for event in camera_changes)
    assert all(event["primvar_fetch_count"] == 0 for event in camera_changes)
    assert all(event["upload_bytes"] == 0 for event in camera_changes)
    assert all(
        event["pipeline_creation_count"] == 0
        for event in camera_changes)
    assert all(
        event["geometry_cache_misses"] == 0
        for event in camera_changes)

    assert material["draw_count"] == 6
    assert material["scene_revision"] > camera["scene_revision"]
    material_changes = _events_with_aspect(
        "material_parameter", "material_aspects", 1 << 6)
    assert material_changes
    assert all(event["material_aspects"] == 1 << 6
               for event in material_changes)
    assert all(event["material_fetch_count"] == 1
               for event in material_changes)
    assert all(event["mesh_sync_count"] == 0
               for event in material_changes)
    assert all(event["points_fetch_count"] == 0
               for event in material_changes)
    assert all(event["topology_fetch_count"] == 0
               for event in material_changes)
    assert all(event["primvar_fetch_count"] == 0
               for event in material_changes)
    assert all(event["triangulation_rebuild_count"] == 0
               for event in material_changes)
    assert all(event["packed_mesh_rebuild_count"] == 0
               for event in material_changes)
    assert all(event["upload_bytes"] == 0 for event in material_changes)
    assert all(event["pipeline_creation_count"] == 0
               for event in material_changes)

    assert diagnostic["draw_count"] == 5
    assert diagnostic["scene_revision"] > material["scene_revision"]
    diagnostic_changes = _events_with_aspect(
        "diagnostic", "mesh_aspects", 1 << 0)
    assert diagnostic_changes
    assert all(event["diagnostic_count"] >= 1
               for event in diagnostic_changes)
    assert all(event["triangulation_rebuild_count"] == 1
               for event in diagnostic_changes)

    assert recovery["draw_count"] == 6
    assert recovery["scene_revision"] > diagnostic["scene_revision"]
    assert recovery["diagnostic_count"] == 0
    recovery_changes = _events_with_aspect(
        "recovery", "mesh_aspects", 1 << 0)
    assert recovery_changes
    assert all(event["upload_bytes"] > 0 for event in recovery_changes)

    assert removed["draw_count"] == 5
    assert removed["scene_revision"] > recovery["scene_revision"]
    assert removed["mesh_sync_count"] == 0
    assert removed["upload_bytes"] == 0

    assert readded["draw_count"] == 6
    assert readded["scene_revision"] > removed["scene_revision"]
    readd_changes = _events_with_aspect(
        "readd", "mesh_aspects", 1 << 0)
    assert readd_changes
    assert all(event["mesh_sync_count"] >= 1
               for event in readd_changes)
    assert all(event["points_fetch_count"] >= 1
               for event in readd_changes)
    assert all(event["topology_fetch_count"] >= 1
               for event in readd_changes)
    assert all(event["upload_bytes"] > 0
               for event in readd_changes)

    assert resized["draw_count"] == 6
    assert resized["completion_value"] > readded["completion_value"]
    assert (resized["width"], resized["height"]) == (401, 301)
    assert (resized["width"], resized["height"]) != (
        camera["width"], camera["height"])
    resize_settings_changes = _events_with_aspect(
        "resize", "render_settings_aspects", 1 << 9)
    assert resize_settings_changes
    assert all(
        event["render_settings_resource_revision"] >= 2
        for event in resize_settings_changes)
    _check_mesh_orientation(appController)

    # Hdx selection colorization consumes ID buffers through Map. The native
    # AOV test independently checks their exact current-frame contents.
    appController._dataModel.selection.addPrimPath(Sdf.Path("/World/OrientationProbe"))
    # usdview draws its OpenGL axes unconditionally after host composition.
    # Isolate the Hdx selection image from that unrelated depth-overlay tail.
    draw_axis = appController._stageView.DrawAxis
    appController._stageView.DrawAxis = lambda _: None
    try:
        selected = _render_phase(appController, "selection", size=(597, 540))
    finally:
        appController._stageView.DrawAxis = draw_axis
    if selected["hgi_transfer_mode"] == "gpu-copy":
        assert selected["hgi_cpu_download_count"] >= 2
        assert selected["hgi_cpu_download_bytes"] > 0
    if selected.get("hgi_metal_transfer_mode") == "gpu-copy":
        assert selected["hgi_metal_cpu_download_count"] >= 2
        assert selected["hgi_metal_cpu_download_bytes"] > 0
    _check_click_picking(appController)
