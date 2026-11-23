"""Measure paired Exact/adaptive camera sequences in testusdview."""

import hashlib
import json
import math
import os
from pathlib import Path
from statistics import median

from PySide6 import QtWidgets
from PySide6.QtGui import QImage


TIMINGS = ("gaussian_gpu_preparation_ns", "gaussian_gpu_sort_ns", "gaussian_gpu_tile_ns",
           "gaussian_raster_ns", "gpu_execution_ns")
COUNTERS = ("gaussian_gpu_pre_sort_culled_count", "gaussian_gpu_tile_requested_pair_count",
            "gaussian_gpu_tile_rejected_pair_count", "gaussian_gpu_tile_saturated_count",
            "gaussian_gpu_tile_early_terminated_count", "gaussian_gpu_tile_raster_frame_count",
            "gaussian_gpu_tile_raster_overflow_fallback_count")


def summarize(samples):
    ordered = sorted(samples)
    return {"median": median(ordered),
            "p95": ordered[math.ceil(0.95 * len(ordered)) - 1],
            "p99": ordered[math.ceil(0.99 * len(ordered)) - 1]}


def color_metrics(exact, adaptive, width, height, previous_error):
    pixel_count = width * height
    if exact == adaptive:
        errors = [0] * (3 * pixel_count)
        alpha_max, mse, ssim = 0, 0.0, 1.0
    else:
        channels = [list(map(int.__sub__, adaptive[i::4], exact[i::4])) for i in range(3)]
        errors = [error for channel in channels for error in channel]
        alpha_max = max(map(abs, map(int.__sub__, adaptive[3::4], exact[3::4])))
        mse = sum(error * error for error in errors) / len(errors)
        # Average luminance SSIM over non-overlapping 8x8 blocks, including
        # partial edge blocks. Coefficients apply to the encoded RGB capture.
        luma = [tuple(0.2126 * r + 0.7152 * g + 0.0722 * b
                      for r, g, b in zip(image[0::4], image[1::4], image[2::4]))
                for image in (exact, adaptive)]
        scores = []
        c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
        for y in range(0, height, 8):
            for x in range(0, width, 8):
                indices = [row * width + col for row in range(y, min(y + 8, height))
                           for col in range(x, min(x + 8, width))]
                a, b = ([image[i] for i in indices] for image in luma)
                n = len(indices)
                ma, mb = sum(a) / n, sum(b) / n
                va = sum((v - ma) ** 2 for v in a) / n
                vb = sum((v - mb) ** 2 for v in b) / n
                cov = sum((u - ma) * (v - mb) for u, v in zip(a, b)) / n
                scores.append((2 * ma * mb + c1) * (2 * cov + c2) /
                              ((ma ** 2 + mb ** 2 + c1) * (va + vb + c2)))
        ssim = sum(scores) / len(scores)
    temporal = (list(map(int.__sub__, errors, previous_error))
                if previous_error is not None else [])
    return {"rgba_byte_identical": exact == adaptive,
            "max_rgb_error_8bit": max(map(abs, errors)), "max_alpha_error_8bit": alpha_max,
            "rgb_psnr_db": 10 * math.log10(255 ** 2 / mse) if mse else None,
            "luma_ssim_8x8": ssim,
            "temporal_rgb_max_8bit": max(map(abs, temporal)) if temporal else None,
            "temporal_rgb_rmse_8bit": (math.sqrt(sum(e * e for e in temporal) / len(temporal))
                                      if temporal else None)}, errors


def testUsdviewInputFunction(controller):
    view = controller._stageView
    settings = controller._dataModel.viewSettings
    settings.showBBoxes = settings.showHUD = settings.showAxis = False
    controller._dataModel.selection.clearPrims()
    view.SetPhysicalWindowSize(int(os.environ.get("MERLIN_GAUSSIAN_BENCHMARK_WIDTH", "597")),
                               int(os.environ.get("MERLIN_GAUSSIAN_BENCHMARK_HEIGHT", "540")))
    frames = int(os.environ["MERLIN_GAUSSIAN_BENCHMARK_FRAMES"])
    assert frames >= 2
    camera = settings.freeCamera
    assert camera is not None, "comparison requires a framed free camera"
    theta, phi = camera.rotTheta, camera.rotPhi
    view.SetRendererSetting("merlin:gpuDrivenGaussian:mode", "require")
    view.SetRendererSetting("merlin:gpuDrivenGaussian:tiled", True)
    key = "merlin:gpuDrivenGaussian:adaptiveBounds"
    phase_key = "MERLIN_HYDRA2_REGRESSION_PHASE"
    log = Path(os.environ["MERLIN_HYDRA2_REGRESSION_LOG"])

    def set_pose(frame):
        progress = 2.0 * frame / (frames - 1) - 1.0
        camera.rotTheta = theta + 15.0 * progress
        camera.rotPhi = phi + 5.0 * math.sin(math.pi * progress / 2.0)

    def draw(adaptive, phase):
        os.environ[phase_key] = "unmeasured"
        view.SetRendererSetting(key, adaptive)
        assert view.GetRendererSetting(key) is adaptive
        os.environ[phase_key] = phase
        try:
            view.SetForceRefresh(True)
            view.updateGL()
            QtWidgets.QApplication.processEvents()
        finally:
            os.environ[phase_key] = "unmeasured"

    for i in range(6):
        set_pose(i % frames)
        for adaptive in (False, True):
            draw(adaptive, "warmup")

    set_pose(0)
    for motion in ("static", "camera-motion"):
        for i in range(frames):
            if motion == "camera-motion":
                set_pose(i)
            for adaptive in (bool(i % 2), not bool(i % 2)):
                mode = "adaptive" if adaptive else "exact"
                draw(adaptive, f"{motion}-{mode}")

    readouts = {}
    for line in log.read_text(encoding="utf-8").splitlines():
        event = dict(field.split("=", 1) for field in line.split() if "=" in field)
        phase = event.get("phase", "")
        if not phase.startswith(("static-", "camera-motion-")):
            continue
        assert int(event["gaussian_adaptive_tile_bounds"]) == phase.endswith("-adaptive")
        assert int(event["gaussian_visible_count"]) > 0
        assert int(event["gaussian_gpu_fallback_count"]) == 0
        assert int(event["gaussian_attribute_upload_bytes"]) == 0
        assert sum(int(event[key]) for key in COUNTERS[-2:]) == 1
        assert event["gaussian_raster_path"] == "tiled"
        readouts.setdefault(phase, []).append(event)
    assert len(readouts) == 4 and all(len(events) == frames for events in readouts.values())
    products = {(event["width"], event["height"], event["requested_aov_mask"])
                for events in readouts.values() for event in events}
    assert len(products) == 1, products
    expected_transfer = os.environ["MERLIN_GAUSSIAN_EXPECT_TRANSFER_MODE"]
    # The Vulkan bridge's historical log fields use the shorter "hgi_" prefix.
    metal = os.environ["MERLIN_HYDRA2_TEST_BACKEND"] == "metal"
    transfer_key = "hgi_metal_transfer_mode" if metal else "hgi_transfer_mode"
    assert {event[transfer_key] for events in readouts.values() for event in events} == {expected_transfer}

    comparison = []
    previous_error = None
    references = set()
    for i in range(frames):
        set_pose(i)
        captures = []
        for adaptive in (False, True):
            mode = "adaptive" if adaptive else "exact"
            draw(adaptive, "capture")
            path = log.parent / f"{mode}-{i:03}.png"
            controller._takeShot(str(path), iterations=4, waitForConvergence=True)
            image = QImage(str(path)).convertToFormat(QImage.Format.Format_RGBA8888)
            assert not image.isNull(), path
            captures.append((image.width(), image.height(), bytes(image.constBits())))
        width, height, exact = captures[0]
        assert captures[1][:2] == (width, height)
        references.add(hashlib.sha256(exact).digest())
        result, previous_error = color_metrics(exact, captures[1][2], width, height, previous_error)
        comparison.append({"frame": i, **result})
    assert len(references) == frames, "camera poses did not generate distinct reference images"

    report = {"schema": "merlin-gaussian-adaptive-sequence/v1",
              "host_transfer_mode": expected_transfer,
              "render_extent": [int(value) for value in next(iter(products))[:2]],
              "image_extent": [width, height], "image_comparisons": comparison, "phases": {}}
    for phase, events in readouts.items():
        report["phases"][phase] = {
            "samples": len(events),
            "timings_ns": {key: summarize([int(event[key]) for event in events])
                           for key in TIMINGS if all(
                               event["gaussian_gpu_timestamps_available"] == "1" for event in events)},
            "counters": {key: summarize([int(event[key]) for event in events]) for key in COUNTERS},
            "overflow_frames": sum(int(event[COUNTERS[-1]]) for event in events),
        }
    (log.parent / "gaussian-adaptive-sequence.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if os.environ.get("MERLIN_GAUSSIAN_ALLOW_IMAGE_DIFFERENCES") != "1":
        assert all(frame["rgba_byte_identical"] for frame in comparison), "adaptive changed a camera frame"
