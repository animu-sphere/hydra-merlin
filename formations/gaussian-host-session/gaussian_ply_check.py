def testUsdviewInputFunction(appController):
    model = appController._dataModel
    assert model.stage.GetRootLayer().GetFileFormat().formatId == "ply"
    splat = model.stage.GetPrimAtPath("/Asset/Splat")
    assert splat.GetTypeName() == "ParticleField3DGaussianSplat"
    assert model.stage.GetDefaultPrim().GetCustomDataByKey("gs:gaussianCount") == 3

    view = appController._stageView
    assert str(view.GetCurrentRendererId()) == "HdMerlinRendererPlugin"
    model.viewSettings.showBBoxes = False
    model.viewSettings.showHUD = False
    model.selection.clearPrims()
    view.SetForceRefresh(True)
    view.updateView()
    appController._takeShot("gaussian-ply.png", iterations=4, waitForConvergence=True)

    image = appController.GrabViewportShot()
    assert not image.isNull(), "usdview returned an empty viewport image"
    red_pixels = green_pixels = blue_pixels = 0
    for y in range(0, image.height(), 2):
        for x in range(0, image.width(), 2):
            pixel = image.pixel(x, y)
            red = (pixel >> 16) & 255
            green = (pixel >> 8) & 255
            blue = pixel & 255
            red_pixels += red > max(green, blue) + 20
            green_pixels += green > max(red, blue) + 20
            blue_pixels += blue > max(red, green) + 20
    color_counts = (red_pixels, green_pixels, blue_pixels)
    assert min(color_counts) > 200, f"Expected three visible splats, got {color_counts}"
    print(f"Merlin drew the Gaussian PLY fixture (R/G/B sample counts: {color_counts})")
