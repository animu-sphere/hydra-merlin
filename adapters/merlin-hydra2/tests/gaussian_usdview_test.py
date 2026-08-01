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
