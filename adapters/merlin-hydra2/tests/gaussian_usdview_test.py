import os


def testUsdviewInputFunction(appController):
    appController._dataModel.viewSettings.showBBoxes = False
    appController._dataModel.viewSettings.showHUD = False
    appController._stageView.SetForceRefresh(True)
    appController._stageView.updateView()
    appController._takeShot(
        os.environ["MERLIN_HYDRA2_SMOKE_IMAGE"],
        iterations=4,
        waitForConvergence=True,
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
