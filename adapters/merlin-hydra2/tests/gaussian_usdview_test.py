import os

from PySide6.QtGui import QImage


MAX_REFERENCE_CHANGED_PIXEL_FRACTION = 0.005
MAX_REFERENCE_MEAN_CHANNEL_ERROR = 0.5


def compare_reference(actual, reference_path):
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
        any(value > 2 for value in differences[offset:offset + 4])
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


def testUsdviewInputFunction(appController):
    appController._dataModel.viewSettings.showBBoxes = False
    appController._dataModel.viewSettings.showHUD = False
    appController._dataModel.selection.clearPrims()
    appController._stageView.SetForceRefresh(True)
    appController._stageView.updateView()
    appController._takeShot(
        os.environ["MERLIN_HYDRA2_SMOKE_IMAGE"],
        iterations=int(os.environ.get(
            "MERLIN_GAUSSIAN_USDVIEW_ITERATIONS", "4")),
        waitForConvergence=True,
    )

    image = appController.GrabViewportShot()
    reference_path = os.environ.get("MERLIN_GAUSSIAN_REFERENCE_IMAGE")
    if reference_path:
        compare_reference(image, reference_path)
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
    prepared = [
        event for event in events
        if int(event.get("gaussian_candidate_count", "0")) > 0
    ]
    assert prepared, "Gaussian particles did not reach CPU projection"
    for event in prepared:
        candidates = int(event["gaussian_candidate_count"])
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
        int(event.get("gaussian_draw_count", "0")) == 1
        for event in rasterized
    ), "Visible Gaussian stream did not reach the procedural Vulkan draw"
    assert any(
        int(event.get("gaussian_upload_bytes", "0")) > 0
        for event in rasterized
    ), "Prepared Gaussian stream was never uploaded"
    assert any(
        int(event.get("gaussian_preparation_cache_hits", "0")) > 0
        and int(event.get("gaussian_upload_bytes", "0")) == 0
        for event in rasterized
    ), "Static Gaussian frame did not reuse its frame-local upload"
