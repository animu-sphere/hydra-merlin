import os

from pxr import Gf, UsdGeom, Vt


def _read_events():
    marker = os.environ["MERLIN_HYDRA2_REGRESSION_LOG"]
    if not os.path.exists(marker):
        return []
    events = []
    with open(marker, encoding="utf-8") as stream:
        for line in stream:
            event = {}
            for field in line.split():
                key, value = field.split("=", 1)
                event[key] = value if key == "phase" else int(value)
            events.append(event)
    return events


def _render_phase(app_controller, phase, edit=None, size=None):
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


def _hide_triangle(stage):
    UsdGeom.Imageable(
        stage.GetPrimAtPath("/World/VisibilityTriangle")
    ).GetVisibilityAttr().Set(UsdGeom.Tokens.invisible)


def _move_camera(stage):
    transform = Gf.Matrix4d(1.0)
    transform.SetTranslate(Gf.Vec3d(0.35, 0.0, 5.0))
    stage.GetPrimAtPath(
        "/Camera").GetAttribute("xformOp:transform").Set(transform)


def testUsdviewInputFunction(appController):
    appController._dataModel.viewSettings.showBBoxes = False
    appController._dataModel.viewSettings.showHUD = False

    baseline = _render_phase(appController, "baseline")
    assert baseline["textured_materials"] >= 1
    assert baseline["texcoord_geometries"] == 1
    assert baseline["missing_texcoord_geometries"] == 3
    assert baseline["material_fallbacks"] == 0
    assert baseline["texture_cache_hits"] >= 1
    points = _render_phase(appController, "points", edit=_edit_points)
    topology = _render_phase(
        appController, "topology", edit=_edit_topology)
    transformed = _render_phase(
        appController, "transform", edit=_move_triangle)
    hidden = _render_phase(
        appController, "visibility", edit=_hide_triangle)
    camera = _render_phase(appController, "camera", edit=_move_camera)
    resized = _render_phase(appController, "resize", size=(401, 301))

    assert baseline["draw_count"] == 6
    assert points["draw_count"] == 6
    assert points["scene_revision"] > baseline["scene_revision"]
    point_changes = _events_with_aspect("points", "mesh_aspects", 1 << 1)
    assert point_changes
    assert all(event["instance_aspects"] == 0 for event in point_changes)
    assert all(
        event["mesh_resource_revision"] >= 2
        for event in point_changes)

    assert topology["draw_count"] == 6
    assert topology["scene_revision"] > points["scene_revision"]
    topology_changes = _events_with_aspect(
        "topology", "mesh_aspects", 1 << 0)
    assert topology_changes
    assert all(
        event["instance_aspects"] == 0 for event in topology_changes)

    assert transformed["draw_count"] == 6
    assert transformed["scene_revision"] > topology["scene_revision"]
    assert transformed["covered_x_sum"] != topology["covered_x_sum"]
    transform_changes = _events_with_aspect(
        "transform", "instance_aspects", 1 << 3)
    assert transform_changes
    assert all(
        event["instance_aspects"] == 1 << 3
        for event in transform_changes)
    assert all(event["mesh_aspects"] == 0 for event in transform_changes)
    assert all(
        event["instance_resource_revision"] >= 2
        for event in transform_changes)

    assert hidden["draw_count"] == 5
    assert hidden["scene_revision"] > transformed["scene_revision"]
    assert hidden["covered_x_sum"] != transformed["covered_x_sum"]
    visibility_changes = _events_with_aspect(
        "visibility", "instance_aspects", 1 << 4)
    assert visibility_changes
    assert all(
        event["instance_aspects"] == 1 << 4
        for event in visibility_changes)
    assert all(event["mesh_aspects"] == 0 for event in visibility_changes)

    assert camera["draw_count"] == 5
    assert camera["scene_revision"] > hidden["scene_revision"]
    assert camera["covered_x_sum"] != hidden["covered_x_sum"]
    camera_changes = _events_with_aspect(
        "camera", "camera_aspects", 1 << 7)
    assert camera_changes
    assert all(event["instance_aspects"] == 0 for event in camera_changes)
    assert all(
        event["camera_resource_revision"] >= 2
        for event in camera_changes)

    assert resized["draw_count"] == 5
    assert resized["completion_value"] > camera["completion_value"]
    assert (resized["width"], resized["height"]) == (401, 301)
    assert (resized["width"], resized["height"]) != (
        camera["width"], camera["height"])
    resize_settings_changes = _events_with_aspect(
        "resize", "render_settings_aspects", 1 << 9)
    assert resize_settings_changes
    assert all(
        event["render_settings_resource_revision"] >= 2
        for event in resize_settings_changes)
