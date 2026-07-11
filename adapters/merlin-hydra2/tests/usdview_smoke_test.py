import os


def testUsdviewInputFunction(appController):
    appController.setFrame(1)
    appController._takeShot(
        os.environ["MERLIN_HYDRA2_SMOKE_IMAGE"],
        iterations=10,
        waitForConvergence=True)
