import os

from pxr import Trace, UsdImagingGL
from PySide6 import QtWidgets
from PySide6.QtGui import QImage


MAX_REFERENCE_CHANGED_PIXEL_FRACTION = 0.005
MAX_REFERENCE_MEAN_CHANNEL_ERROR = 0.5
# A channel counts as changed above this many 8-bit steps. Keep distinct host
# and offscreen policies and the existing six-step tile bound. Vulkan compares
# against its float-composite baseline; Metal retains the legacy UNorm image.
REFERENCE_CHANNEL_TOLERANCE = {"sorted-stream": 2, "tiled": 6}
GAUSSIAN_MODE_SETTING = "merlin:gpuDrivenGaussian:mode"
GAUSSIAN_RASTER_SETTING = "merlin:gpuDrivenGaussian:raster"
GAUSSIAN_ENABLED_SETTING = "merlin:gpuDrivenGaussian:enabled"
GAUSSIAN_TILED_SETTING = "merlin:gpuDrivenGaussian:tiled"
CPU_PHASE = "cpu-sorted-stream"


def check_gaussian_settings(stage_view):
    """Hydra lists the Gaussian policy as Hydra Settings menu flags."""
    settings = {
        str(setting.key): setting
        for setting in stage_view.GetRendererSettingsList()
    }
    # Only flags keep usdview from adding its More... dialog, which re-sends
    # stale values.
    assert set(settings) == {
        GAUSSIAN_ENABLED_SETTING, GAUSSIAN_TILED_SETTING
    }, f"usdview lists unexpected Merlin settings: {sorted(settings)}"
    for key in settings:
        assert (
            settings[key].type == UsdImagingGL.RendererSettingType.FLAG
        ), f"{key} is not a menu flag"
        assert settings[key].defValue is False, f"{key} default changed"
        assert stage_view.GetRendererSetting(key) is False, (
            f"{key} does not start at its default"
        )
    assert stage_view.GetRendererSetting(GAUSSIAN_MODE_SETTING) == "disabled"
    assert (
        stage_view.GetRendererSetting(GAUSSIAN_RASTER_SETTING)
        == "sorted-stream"
    )


def select_gaussian_policy(stage_view, mode, raster):
    """Selects the Gaussian execution policy through Hydra render settings."""
    # A name outside the renderer vocabulary is rejected explicitly and does
    # not replace the applied policy.
    stage_view.SetRendererSetting(GAUSSIAN_MODE_SETTING, "always")
    assert stage_view.GetRendererSetting(GAUSSIAN_MODE_SETTING) == "disabled"
    # The mode takes its name, since require has no flag; the raster path
    # goes through the menu flag.
    stage_view.SetRendererSetting(GAUSSIAN_MODE_SETTING, mode)
    stage_view.SetRendererSetting(GAUSSIAN_TILED_SETTING, raster == "tiled")
    assert stage_view.GetRendererSetting(GAUSSIAN_MODE_SETTING) == mode, (
        "usdview could not select the GPU Gaussian policy"
    )
    assert stage_view.GetRendererSetting(GAUSSIAN_RASTER_SETTING) == raster
    assert stage_view.GetRendererSetting(GAUSSIAN_ENABLED_SETTING) is (
        mode != "disabled"
    ), "the GPU Gaussian flag does not report the selected policy"


def render_phase(app_controller, phase, iterations, motion_frames):
    # The phase labels both the renderer events and the host trace scope, so
    # the performance report can attribute host presentation to each policy.
    trace = Trace.Collector()
    label = f"MerlinPhase:{phase}"
    trace.BeginEvent(label)
    try:
        os.environ["MERLIN_HYDRA2_REGRESSION_PHASE"] = phase
        stage_view = app_controller._stageView
        camera = app_controller._dataModel.viewSettings.freeCamera
        # usdview draws only when the view changes. Tumbling the free camera
        # back and forth draws one frame per step, re-projects every Gaussian
        # as camera motion does, and returns to the framed pose.
        for frame in range(motion_frames if camera else 0):
            camera.Tumble(0.5 if frame % 2 == 0 else -0.5, 0.0)
            stage_view.updateGL()
            QtWidgets.QApplication.processEvents()
        stage_view.SetForceRefresh(True)
        stage_view.updateView()
        app_controller._takeShot(
            os.environ["MERLIN_HYDRA2_SMOKE_IMAGE"],
            iterations=iterations,
            waitForConvergence=True,
        )
    finally:
        trace.EndEvent(label)


def compare_reference(actual, reference_path, channel_tolerance):
    reference = QImage(reference_path)
    assert not reference.isNull(), "could not load Gaussian reference image"
    actual = actual.convertToFormat(QImage.Format.Format_RGBA8888)
    reference = reference.convertToFormat(QImage.Format.Format_RGBA8888)
    assert actual.size() == reference.size(), (
        "Gaussian reference-image dimensions changed"
    )
    actual_bytes = bytes(actual.constBits())[:actual.sizeInBytes()]
    reference_bytes = bytes(reference.constBits())[:reference.sizeInBytes()]
    differences = [
        abs(actual_value - reference_value)
        for actual_value, reference_value in zip(actual_bytes, reference_bytes)
    ]
    pixel_count = actual.width() * actual.height()
    changed_pixels = sum(
        any(
            value > channel_tolerance
            for value in differences[offset:offset + 4]
        )
        for offset in range(0, len(differences), 4)
    )
    changed_fraction = changed_pixels / pixel_count
    mean_channel_error = sum(differences) / len(differences)
    assert changed_fraction <= MAX_REFERENCE_CHANGED_PIXEL_FRACTION, (
        "Gaussian reference changed-pixel fraction "
        f"{changed_fraction:.6f} exceeds "
        f"{MAX_REFERENCE_CHANGED_PIXEL_FRACTION:.6f}"
    )
    assert mean_channel_error <= MAX_REFERENCE_MEAN_CHANNEL_ERROR, (
        "Gaussian reference mean channel error "
        f"{mean_channel_error:.6f} exceeds "
        f"{MAX_REFERENCE_MEAN_CHANNEL_ERROR:.6f}"
    )
    return {"width": actual.width(), "height": actual.height(),
            "max_channel_error": max(differences),
            "changed_pixel_fraction": changed_fraction,
            "mean_channel_error": mean_channel_error}


def check_cpu_sorted_stream(events):
    assert all(
        event.get("gaussian_gpu_mode") == "disabled" for event in events
    ), "the default Gaussian policy did not keep the CPU-sorted reference"
    prepared = [
        event for event in events
        if int(event.get("gaussian_candidate_count", "0")) > 0
    ]
    assert prepared, "Gaussian particles did not reach CPU projection"
    assert any(
        int(event.get("gaussian_preparation_cache_hits", "0")) > 0
        for event in prepared
    ), "Static Gaussian frames did not reuse CPU preparation"
    assert any(
        int(event.get("gaussian_preparation_cache_misses", "0")) > 0
        for event in prepared
    ), "Gaussian preparation never recorded initial work"
    rasterized = [
        event for event in prepared
        if int(event.get("gaussian_visible_count", "0")) > 0
    ]
    assert rasterized, "Gaussian projection retained no rasterizable particles"
    assert any(
        int(event.get("gaussian_draw_count", "0")) == 2
        for event in rasterized
    ), "Visible Gaussian stream did not reach the color and ID Vulkan draws"
    assert any(
        int(event.get("gaussian_upload_bytes", "0")) > 0
        for event in rasterized
    ), "Prepared Gaussian stream was never uploaded"
    assert any(
        int(event.get("gaussian_preparation_cache_hits", "0")) > 0
        and int(event.get("gaussian_upload_bytes", "0")) == 0
        for event in rasterized
    ), "Static Gaussian frame did not reuse its frame-local upload"


def check_gpu_policy(events, mode, raster):
    assert events, "the GPU Gaussian phase completed no render"
    for event in events:
        assert event.get("gaussian_gpu_mode") == mode and event.get(
            "gaussian_raster_path") == raster, (
            "a GPU-phase frame did not apply the selected Gaussian policy"
        )
        visible = int(event["gaussian_visible_count"])
        assert visible > 0, "GPU preparation retained no visible Gaussians"
        assert int(event["gaussian_gpu_sorted_count"]) == visible, (
            "the GPU sort did not order every visible Gaussian"
        )
        assert int(event["gaussian_gpu_fallback_count"]) == 0, (
            "a required GPU Gaussian stage fell back"
        )
        # The GPU chain replaces the CPU reference preparation and its
        # prepared-stream upload.
        assert int(event["gaussian_preparation_ns"]) == 0
        assert int(event["gaussian_upload_bytes"]) == 0
        tile_frames = int(event["gaussian_gpu_tile_raster_frame_count"])
        overflow = int(
            event["gaussian_gpu_tile_raster_overflow_fallback_count"])
        if raster == "tiled":
            assert tile_frames + overflow == 1, (
                "tile raster neither composited nor reported its fallback"
            )
        else:
            assert tile_frames == 0 and overflow == 0
            assert int(event["gaussian_gpu_raster_instance_count"]) == visible
    if raster == "tiled":
        assert any(
            int(event["gaussian_gpu_tile_raster_frame_count"]) == 1
            for event in events
        ), "tile raster never produced the Gaussian image"


def testUsdviewInputFunction(appController):
    appController._dataModel.viewSettings.showBBoxes = False
    appController._dataModel.viewSettings.showHUD = False
    appController._dataModel.selection.clearPrims()
    stage_view = appController._stageView
    check_gaussian_settings(stage_view)
    iterations = int(os.environ.get("MERLIN_GAUSSIAN_USDVIEW_ITERATIONS", "4"))
    motion_frames = int(
        os.environ.get("MERLIN_GAUSSIAN_USDVIEW_MOTION_FRAMES", "0"))
    mode = os.environ.get("MERLIN_GAUSSIAN_USDVIEW_GPU_MODE")
    raster = os.environ.get(
        "MERLIN_GAUSSIAN_USDVIEW_GPU_RASTER", "sorted-stream")
    render_phase(appController, CPU_PHASE, iterations, motion_frames)
    gpu_phase = None
    if mode:
        select_gaussian_policy(stage_view, mode, raster)
        gpu_phase = f"gpu-{raster}"
        render_phase(appController, gpu_phase, iterations, motion_frames)

    image = appController.GrabViewportShot()
    reference_path = os.environ.get("MERLIN_GAUSSIAN_REFERENCE_IMAGE")
    if reference_path:
        compare_reference(image, reference_path,
            REFERENCE_CHANNEL_TOLERANCE[raster if mode else "sorted-stream"])
    background = image.pixel(0, 0)
    background_rgb = (
        (background >> 16) & 0xff,
        (background >> 8) & 0xff,
        background & 0xff,
    )
    changed_pixels = 0
    for y in range(0, image.height(), 2):
        for x in range(0, image.width(), 2):
            pixel = image.pixel(x, y)
            red = (pixel >> 16) & 0xff
            green = (pixel >> 8) & 0xff
            blue = pixel & 0xff
            if max(abs(red - background_rgb[0]),
                   abs(green - background_rgb[1]),
                   abs(blue - background_rgb[2])) > 20:
                changed_pixels += 1
    # usdview's origin axes account for only a narrow pair of lines. Requiring
    # a wider non-background footprint proves that the Gaussian color target,
    # rather than an overlay, reached host composition.
    assert changed_pixels > 200, (
        "usdview captured no visible Gaussian color"
    )

    marker = os.environ["MERLIN_HYDRA2_REGRESSION_LOG"]
    assert os.path.exists(marker), "Merlin produced no regression record"
    with open(marker, encoding="utf-8") as stream:
        events = [
            dict(field.split("=", 1) for field in line.split())
            for line in stream
        ]
    assert events, "Merlin completed no render"
    assert any(
        int(event.get("gaussian_resources", "0")) == 1
        for event in events
    ), "Hydra particleField did not reach the Gaussian snapshot"
    for event in events:
        candidates = int(event.get("gaussian_candidate_count", "0"))
        if candidates == 0:
            continue
        accounted = sum(
            int(event.get(field, "0"))
            for field in (
                "gaussian_visible_count",
                "gaussian_hidden_count",
                "gaussian_opacity_culled_count",
                "gaussian_frustum_culled_count",
                "gaussian_invalid_culled_count",
            )
        )
        assert accounted == candidates, (
            "Gaussian preparation did not account for every particle"
        )
        assert int(event["gaussian_sorted_count"]) == int(
            event["gaussian_visible_count"]
        )
    # Frames before the phase label and the CPU phase run the default policy.
    check_cpu_sorted_stream([
        event for event in events if event.get("phase") != gpu_phase
    ])
    if gpu_phase:
        check_gpu_policy(
            [event for event in events if event.get("phase") == gpu_phase],
            mode, raster)
