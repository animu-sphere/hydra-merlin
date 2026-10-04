"""Opt-in, warmed Gaussian host comparison, run by testusdview.

Same stage, viewport and alternating camera sequence for each supported policy.
Warmup/capture frames have separate labels and never enter measured phases.
"""
import importlib.util
import hashlib
import json
import os
from pathlib import Path

from pxr import Trace
from PySide6 import QtWidgets


def testUsdviewInputFunction(appController):
    spec = importlib.util.spec_from_file_location(
        "gaussian_smoke", Path(testUsdviewInputFunction.__code__.co_filename).with_name("gaussian_usdview_test.py"))
    smoke = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(smoke)
    view = appController._stageView
    settings = appController._dataModel.viewSettings
    settings.showBBoxes = False
    settings.showHUD = False
    settings.showAxis = False
    appController._dataModel.selection.clearPrims()
    view.SetPhysicalWindowSize(
        int(os.environ.get("MERLIN_GAUSSIAN_BENCHMARK_WIDTH", "597")),
        int(os.environ.get("MERLIN_GAUSSIAN_BENCHMARK_HEIGHT", "540")))
    frames = int(os.environ.get("MERLIN_GAUSSIAN_BENCHMARK_FRAMES", "40"))
    warmups = int(os.environ.get("MERLIN_GAUSSIAN_BENCHMARK_WARMUPS", "6"))
    assert frames >= 2 and frames % 2 == 0 and warmups >= 4 and warmups % 2 == 0
    camera = settings.freeCamera
    assert camera is not None, "host benchmark requires the framed free camera"
    backend = os.environ.get("MERLIN_HYDRA2_TEST_BACKEND", "vulkan")
    assert backend in ("vulkan", "metal"), backend

    def draw(motion, index):
        if motion:
            camera.Tumble(0.5 if index % 2 == 0 else -0.5, 0.0)
        view.SetForceRefresh(True)
        view.updateGL()
        QtWidgets.QApplication.processEvents()

    output = Path(os.environ["MERLIN_HYDRA2_SMOKE_IMAGE"]).parent
    comparisons = []
    references = {}
    policies = (
        ("disabled", "sorted-stream", "cpu-sorted-stream"),
        ("require", "sorted-stream", "gpu-sorted-stream"),
        ("require", "tiled", "gpu-tiled"),
    )
    for mode, raster, path in policies:
        os.environ["MERLIN_HYDRA2_REGRESSION_PHASE"] = "warmup"
        view.SetRendererSetting(smoke.GAUSSIAN_MODE_SETTING, mode)
        view.SetRendererSetting(smoke.GAUSSIAN_TILED_SETTING, raster == "tiled")
        assert view.GetRendererSetting(smoke.GAUSSIAN_MODE_SETTING) == mode
        for motion in (False, True):
            for index in range(warmups):
                draw(motion, index)
            phase = ("camera-motion-" if motion else "static-") + path
            os.environ["MERLIN_HYDRA2_REGRESSION_PHASE"] = phase
            label = "MerlinPhase:" + phase
            collector = Trace.Collector()
            collector.BeginEvent(label)
            try:
                for index in range(frames):
                    draw(motion, index)
            finally:
                collector.EndEvent(label)
            os.environ["MERLIN_HYDRA2_REGRESSION_PHASE"] = "capture"
            image_path = output / (phase + ".png")
            appController._takeShot(str(image_path), iterations=4, waitForConvergence=True)
            if mode == "disabled":
                references[motion] = image_path
            else:
                difference = smoke.compare_reference(appController.GrabViewportShot(), str(references[motion]),
                    smoke.REFERENCE_CHANNEL_TOLERANCE[raster])
                comparisons.append({"phase": phase, "difference": difference,
                    "image_sha256": hashlib.sha256(image_path.read_bytes()).hexdigest()})

    events = [dict(field.split("=", 1) for field in line.split())
              for line in Path(os.environ["MERLIN_HYDRA2_REGRESSION_LOG"]).read_text().splitlines()]
    measured = [event for event in events if event["phase"].startswith(("static-", "camera-motion-"))]
    phase_counts = {}
    products = set()
    transfer_modes = set()
    bridge = "hgi_metal_" if backend == "metal" else "hgi_"
    for event in measured:
        phase_counts[event["phase"]] = phase_counts.get(event["phase"], 0) + 1
        assert int(event["validation_messages"]) == 0
        # Metal's CPU reference replaces its prepared stream on camera edits.
        if backend == "metal" and event["phase"] == "camera-motion-cpu-sorted-stream":
            assert int(event["allocation_count"]) <= 1
        else:
            assert int(event["allocation_count"]) == 0
        assert int(event["pipeline_creation_count"]) == 0
        assert int(event["gaussian_visible_count"]) > 0
        products.add(tuple(event[key] for key in (
            "width", "height", "requested_aov_mask", "cpu_readback_aov_mask")))
        transfer_modes.add(event[bridge + "transfer_mode"])
        gpu_copy = event[bridge + "transfer_mode"] == "gpu-copy"
        cpu_aovs = (0 if backend == "vulkan" else 3) if gpu_copy else 4
        assert int(event["cpu_readback_aov_count"]) == cpu_aovs
        if backend == "metal":
            # GPU execution reads 32 bytes of control plus 32 bytes of
            # counters per resource, separate from the three image AOVs.
            expected = int(event["width"]) * int(event["height"]) * 12
            if event["gaussian_gpu_mode"] != "disabled":
                expected += 32 + 32 * int(event["gaussian_resources"])
                if backend == "metal" and event["gaussian_raster_path"] == "tiled":
                    expected += 44
            assert int(event["readback_bytes"]) == expected
        assert int(event["map_count"]) == (1 if backend == "metal" else cpu_aovs)
        if backend == "vulkan" and gpu_copy:
            assert int(event["readback_bytes"]) == 0
            assert int(event["hgi_cpu_download_count"]) == 0
            assert int(event["hgi_cpu_download_bytes"]) == 0
        assert bool(int(event["cpu_readback_aov_mask"]) & 1) is (not gpu_copy)
        assert int(event[bridge + "coarse_wait_count"]) == 0
        if gpu_copy:
            assert int(event[bridge + "gpu_copy_count"]) > 0
            assert int(event[bridge + "gpu_copy_pending_count"]) <= (4 if backend == "vulkan" else 1)
        if event["gaussian_gpu_mode"] != "disabled":
            smoke.check_gpu_policy([event], "require", event["gaussian_raster_path"])
            assert int(event["upload_bytes"]) == 0
            assert int(event["gaussian_attribute_upload_bytes"]) == 0
            assert int(event["gaussian_gpu_tile_raster_overflow_fallback_count"]) == 0
        elif event["phase"].startswith("static-"):
            assert int(event["upload_bytes"]) == 0
            assert int(event["gaussian_preparation_cache_hits"]) > 0
    assert len(phase_counts) == 6 and all(value == frames for value in phase_counts.values()), phase_counts
    assert len(products) == 1, "host comparison changed extent or readback products"
    readback_sizes = {int(event["readback_bytes"]) for event in measured}
    if backend == "vulkan":
        assert len(readback_sizes) == 1, "Vulkan host readback size changed between policies"
    assert len(transfer_modes) == 1, "host comparison changed presentation transfer mode"
    assert next(iter(transfer_modes)) == os.environ["MERLIN_GAUSSIAN_EXPECT_TRANSFER_MODE"], transfer_modes
    audit_keys = ("upload_bytes", "gaussian_attribute_upload_bytes", "gaussian_upload_bytes",
        "allocation_count", "pipeline_creation_count", "gaussian_gpu_fallback_count",
        "gaussian_gpu_tile_raster_overflow_fallback_count", "readback_bytes", "map_count",
        "cpu_readback_aov_count", "cpu_readback_aov_mask") + tuple(bridge + suffix for suffix in (
            "coarse_wait_count", "gpu_copy_count", "gpu_copy_completion_count", "gpu_copy_pending_count"))
    if backend == "vulkan":
        audit_keys += ("hgi_cpu_download_count", "hgi_cpu_download_bytes")
    phase_audit = {phase: {key: {"min": min(int(event[key]) for event in measured if event["phase"] == phase),
                              "max": max(int(event[key]) for event in measured if event["phase"] == phase)}
                         for key in audit_keys} for phase in phase_counts}
    product = dict(zip(("width", "height", "requested_aov_mask", "cpu_readback_aov_mask"),
                       next(iter(products))))
    # This product value is the image payload; GPU-only counter readback is
    # recorded per phase in the audit instead of changing the image contract.
    product["readback_bytes"] = str(min(readback_sizes))
    (output / "gaussian-host-verification.json").write_text(json.dumps({
        "schema": "merlin-gaussian-host-verification/v1",
        "backend": backend,
        "tiled_request": "tiled",
        "camera_sequence": "alternating-tumble-reset-per-path/v1",
        "warmup_frames": warmups, "measured_frames": frames,
        "phase_samples": phase_counts, "image_comparisons": comparisons,
        "per_sample_counter_ranges": phase_audit,
        "host_transfer_mode": next(iter(transfer_modes)),
        "products": product,
        "reference_channel_tolerance": smoke.REFERENCE_CHANNEL_TOLERANCE,
        "max_changed_pixel_fraction": smoke.MAX_REFERENCE_CHANGED_PIXEL_FRACTION,
        "max_mean_channel_error": smoke.MAX_REFERENCE_MEAN_CHANNEL_ERROR,
    }, indent=2) + "\n")
