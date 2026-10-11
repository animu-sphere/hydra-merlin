"""Forward first-frame checks through usdview's Qt mouse navigation path."""

import json
import os

from pxr import Gf, Trace
from pxr.Usdviewq.common import ColorCorrectionModes
from PySide6.QtCore import QEvent, QPointF, Qt
from PySide6.QtGui import QImage, QMouseEvent
from PySide6.QtWidgets import QApplication


def check_forward_navigation(app_controller, shader, root, ext,
                             render_phase, read_events):
    view = app_controller._stageView
    settings = app_controller._dataModel.viewSettings
    # Resolve the authored starting camera before switching. Avoid the optional
    # closest-distance pick, which is unrelated to the navigation image gate.
    view.switchToFreeCamera(False)
    render_phase(app_controller, "navigation-start")
    initial = view.copyViewState()
    origin = QPointF(view.width() // 2, view.height() // 2)
    offsets = (8, 16, 24, 32, 24, 16, 8, 0)
    gestures = (("tumble", Qt.MouseButton.LeftButton),
                ("truck", Qt.MouseButton.MiddleButton),
                ("zoom", Qt.MouseButton.RightButton))
    evidence = []

    def send_mouse(kind, point, button, held):
        # Send actual Qt events to the widget so StageView chooses/updates its
        # camera mode. Synchronous dispatch avoids QTest mouseMove's event-loop
        # waits and preserves the one-render first-frame contract.
        event = QMouseEvent(kind, point, point, view.mapToGlobal(point),
                            button, held, Qt.KeyboardModifier.AltModifier)
        QApplication.sendEvent(view, event)

    def pixels(phase):
        image = QImage(f"{root}-{phase}{ext}").convertToFormat(
            QImage.Format.Format_RGBA8888)
        assert not image.isNull(), f"{phase}: missing image"
        center = image.pixelColor(image.width() // 2, image.height() // 2)
        return bytes(image.constBits())[:image.sizeInBytes()], [
            center.red(), center.green(), center.blue()]

    def camera_matrix():
        return view.resolveCamera()[0].transform

    for albedo, value in (("color", Gf.Vec3f(0.4, 0.1875, 0.1)),
                          ("white", Gf.Vec3f(1))):
        shader.GetInput("diffuseColor").Set(value)
        for output, correction in (("linear", ColorCorrectionModes.DISABLED),
                                   ("srgb", ColorCorrectionModes.SRGB)):
            settings.colorCorrectionMode = correction
            for enabled in (False, True):
                settings.ambientLightOnly = enabled
                for gesture, button in gestures:
                    prefix = f"navigation-{albedo}-{output}-{'on' if enabled else 'off'}-{gesture}"
                    view.restoreViewState(initial)
                    render_phase(app_controller, prefix + "-start")
                    starting_image = pixels(prefix + "-start")[0]
                    references = []
                    reference_cameras = []
                    for reference in (True, False):
                        view.restoreViewState(initial)
                        render_phase(app_controller, prefix + "-warmup")
                        os.environ["MERLIN_HYDRA2_REGRESSION_PHASE"] = prefix + "-press"
                        send_mouse(QEvent.Type.MouseButtonPress, origin, button, button)
                        assert view._cameraMode == gesture, "mouse press chose wrong navigation mode"
                        assert settings.cameraPrim is None, "navigation did not use the free camera"
                        previous_camera = camera_matrix()
                        try:
                            for index, offset in enumerate(offsets):
                                phase = f"{prefix}-{'reference' if reference else 'immediate'}-{index:02d}"
                                point = origin + QPointF(offset, 0)
                                trace = Trace.Collector()
                                trace.BeginEvent("MerlinPhase:" + phase)
                                try:
                                    os.environ["MERLIN_HYDRA2_REGRESSION_PHASE"] = phase
                                    send_mouse(QEvent.Type.MouseMove, point,
                                               Qt.MouseButton.NoButton, button)
                                    current_camera = camera_matrix()
                                    assert current_camera != previous_camera, f"{phase}: mouse did not move camera"
                                    previous_camera = current_camera
                                    if reference:
                                        render_phase(app_controller, phase)
                                    else:
                                        app_controller._takeShot(
                                            f"{root}-{phase}{ext}", iterations=0,
                                            waitForConvergence=False)
                                finally:
                                    trace.EndEvent("MerlinPhase:" + phase)
                                image, center = pixels(phase)
                                if reference:
                                    references.append(image)
                                    reference_cameras.append(current_camera)
                                    continue
                                events = [event for event in read_events() if event["phase"] == phase]
                                assert len(events) == 1, f"{phase}: expected one render, got {len(events)}"
                                event = events[0]
                                assert current_camera == reference_cameras[index], f"{phase}: navigation replay changed pose"
                                assert image == references[index], f"{phase}: first frame differs from settled pose"
                                assert event["camera_aspects"] & (1 << 7), f"{phase}: camera edit was not rendered"
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
                                assert all(0 < channel < 255 for channel in center), f"{phase}: lost headroom: {center}"
                                evidence.append({
                                    "phase": phase, "reference_phase": f"{prefix}-reference-{index:02d}",
                                    "gesture": gesture, "step": index,
                                    "logical_offset": offset, "device_pixel_ratio": view.devicePixelRatioF(),
                                    "albedo": albedo, "output": output, "camera_light": enabled,
                                    "center_rgb": center, "render_count": len(events),
                                    "camera_resource_revision": event["camera_resource_revision"],
                                    "upload_bytes": event["upload_bytes"],
                                    "pipeline_creation_count": event["pipeline_creation_count"]})
                        finally:
                            os.environ["MERLIN_HYDRA2_REGRESSION_PHASE"] = prefix + "-release"
                            send_mouse(QEvent.Type.MouseButtonRelease, origin, button,
                                       Qt.MouseButton.NoButton)
                        assert not view._dragActive, "mouse release did not end navigation"
                    assert starting_image != references[3], f"{prefix}: navigation had no image effect"
                    view.restoreViewState(initial)
                    render_phase(app_controller, prefix + "-restored")
                    assert pixels(prefix + "-restored")[0] == starting_image, f"{prefix}: restoring camera changed image"

    with open(root + "-forward-navigation.json", "w", encoding="utf-8") as stream:
        json.dump({"schema": "merlin-usdview-forward-navigation/v1",
                   "input_policy": "qt-alt-mouse-press-move-release",
                   "capture_policy": "one-render-no-event-loop-no-convergence-wait",
                   "gestures": [name for name, _ in gestures], "steps_per_gesture": len(offsets),
                   "first_frame_reference_channel_tolerance": 0,
                   "phases": evidence}, stream, indent=2)
        stream.write("\n")
